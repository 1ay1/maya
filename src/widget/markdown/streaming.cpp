// streaming.cpp — Progressive markdown rendering + top-level memo.
//
// Owns: markdown() — content-keyed LRU over (source → Element); and
// StreamingMarkdown — incremental committed-prefix + opaque-tail widget
// with a resumable boundary scanner, intra-list-blank classifier, and
// per-instance ComponentElement caching for the committed prefix.
//
// Reaches into parser.cpp via md_detail::{parse_markdown_impl,
// collect_ref_defs, parse_inlines} and into render.cpp via
// md_detail::{flatten_inline}.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "maya/element/builder.hpp"
#include "maya/style/border.hpp"
#include "maya/style/style.hpp"
#include "maya/text/stream_sink.hpp"
#include "maya/widget/markdown.hpp"
#include "maya/widget/markdown/internal.hpp"

namespace maya {

// Aliases so the verbatim bodies below resolve to the right TU.
using ::maya::md_detail::parse_markdown_impl;
using ::maya::md_detail::collect_ref_defs;
using ::maya::md_detail::parse_inlines;
using ::maya::md_detail::flatten_inline;
using ::maya::md_detail::build_inline_row;
using ::maya::md_detail::highlight_code;
// ul_marker_len / ol_marker_len / count_indent come in via internal.hpp.
using ::maya::md_detail::ul_marker_len;
using ::maya::md_detail::ol_marker_len;
using ::maya::md_detail::count_indent;

// Local stand-in for the original RefDefsGuard (which lived inside
// parser.cpp's anonymous namespace and is therefore not visible here).
// streaming.cpp's commit_range and render_tail used it to install a
// per-call ref-defs map so parse_inlines could resolve [label] links.
// Implemented as a free helper that pokes parser.cpp's TLS pointer
// through an extern hook published in internal.hpp — same effect, no
// duplicated TLS slot.

namespace {

[[nodiscard]] Element assemble_markdown(md::Document&& doc) {
    if (doc.blocks.empty()) return Element{TextElement{""}};

    if (doc.blocks.size() == 1)
        return detail::vstack().padding(0, 0, 0, 2)(
            md_block_to_element(doc.blocks[0]));

    std::vector<Element> blocks;
    blocks.reserve(doc.blocks.size());
    for (auto& block : doc.blocks)
        blocks.push_back(md_block_to_element(block));

    return detail::vstack().gap(1).padding(0, 0, 0, 2)(std::move(blocks));
}

// ── markdown() content-keyed LRU memo ───────────────────────────────────────
//
// Without this, every `maya::Turn` that uses `Turn::MarkdownText{...}`
// (the common path for settled assistant messages) re-runs
// parse_markdown + the full block-to-Element walk on every frame the
// renderer requests it. In a moha session with N settled turns and one
// live composer, that's O(sum of body bytes) per keystroke — the
// dominant per-frame cost the user reports as "lag grows with
// conversation length."
//
// Strategy:
//   * Bound the cache to a small fixed entry count (kCap). Each entry
//     owns a copy of the source string and a shared_ptr<const Element>
//     to the built tree. Memory ceiling is ~kCap × avg_body_size ≈
//     low-MB for typical moha sessions (kCap=64, ~32KB/turn).
//   * Key on a 64-bit content hash plus exact length match plus a
//     memcmp tiebreak. The combined check is collision-proof; the
//     hash alone narrows the search to ~1 candidate.
//   * MRU promotion via swap-to-back so LRU eviction is `pop_back`.
//   * shared_ptr<const Element>: caller receives the cached tree via
//     the Element(shared_ptr<const Element>) implicit conversion,
//     which auto-derives a stable cache_id from the control block.
//     The renderer's content-keyed component_cache then short-circuits
//     layout AND paint of the cached subtree to a cells-blit — so the
//     win compounds: parse skipped here, tree walk skipped in the
//     renderer.
//
// All state is thread_local — the renderer runs on a single thread per
// app, so no synchronisation is needed and the cache stays warm across
// the whole UI thread's lifetime.

constexpr std::size_t kMarkdownCacheCap = 64;

// FNV-1a 64-bit over a byte range. Tiny + branch-free + memory-bound;
// the compiler unrolls the inner loop comfortably. Used both by the
// markdown() LRU below and by StreamingMarkdown's per-frame tail/prefix
// equality short-circuits, where the alternative is copying multi-KB
// of bytes into a cache buffer every frame the live line grows.
[[nodiscard]] inline std::uint64_t fnv1a64(
    const char* data, std::size_t n) noexcept
{
    constexpr std::uint64_t kOffset = 0xcbf29ce484222325ULL;
    constexpr std::uint64_t kPrime  = 0x100000001b3ULL;
    std::uint64_t h = kOffset;
    for (std::size_t k = 0; k < n; ++k) {
        h ^= static_cast<unsigned char>(data[k]);
        h *= kPrime;
    }
    return h;
}

[[nodiscard]] inline std::uint64_t fnv1a64(std::string_view s) noexcept {
    return fnv1a64(s.data(), s.size());
}

// FNV-1a 64-bit over the source bytes. ~1 cycle/byte on the most
// constrained ARMv6 target; for a 10 KB body that's well under 50 µs
// on a Raspberry Pi 1 — and it's only paid on cache miss anyway since
// the hot path takes the hit branch below.
[[nodiscard]] inline std::uint64_t hash_markdown_source(
    std::string_view s) noexcept
{
    return fnv1a64(s);
}

struct MarkdownCacheEntry {
    std::uint64_t hash;
    std::string   source;       // owned for memcmp tiebreak
    Element       built;        // assembled Element tree; copied on hit
};

[[nodiscard]] inline std::vector<MarkdownCacheEntry>& markdown_cache() {
    thread_local std::vector<MarkdownCacheEntry> cache;
    return cache;
}

} // anonymous

Element markdown(std::string_view source) {
    // ── Tiny inputs: skip the cache entirely.
    // The memo overhead (hash + linear probe + LRU bookkeeping) is
    // larger than parsing a few bytes of markdown. Threshold is
    // deliberately small — most "trivially short" body slots in moha
    // (single-token labels, "(empty)" placeholders) take this path
    // and don't push real settled content out of the cache.
    if (source.size() < 32) {
        return assemble_markdown(parse_markdown(source));
    }

    auto& cache = markdown_cache();
    const std::uint64_t h = hash_markdown_source(source);

    // ── Hit path: linear scan (cache is small, predictable, branch-friendly).
    // On hit we MRU-promote by swapping the entry to the tail and
    // return a deep copy of the cached Element. NOTE: we intentionally
    // do NOT wrap the result in a shared_ptr (which would auto-cast
    // into a ComponentElement via the implicit Element ctor). Wrapping
    // would let the renderer's content-keyed component_cache cells-blit
    // future paints, BUT it forces the auto-measure path to run an
    // extra inner layout::compute on every cache miss — exactly the
    // regression that broke test_markdown's deep_blockquote stress
    // case. Apps that want cells-cache acceleration should opt in at
    // the WIDGET level (Turn::cache_id) where the outer wrapper makes
    // the inner markdown free anyway. The parse-only memo here is
    // strictly additive — it saves the bytes-to-AST work for
    // unchanged content, nothing else.
    for (std::size_t i = 0; i < cache.size(); ++i) {
        auto& e = cache[i];
        if (e.hash == h
            && e.source.size() == source.size()
            && std::memcmp(e.source.data(), source.data(), source.size()) == 0)
        {
            if (i + 1 != cache.size()) std::swap(cache[i], cache.back());
            return cache.back().built;   // variant copy, O(tree)
        }
    }

    // ── Miss: parse, assemble, store, evict oldest if needed.
    Element built = assemble_markdown(parse_markdown(source));
    if (cache.size() >= kMarkdownCacheCap) {
        // Drop the LRU (front) — single shift only happens when the
        // cache is full, which is itself the steady state, but the
        // shift cost is O(kCap × sizeof(entry)) ≈ small for kCap=64,
        // and dwarfed by the parse work we just did on this miss.
        cache.erase(cache.begin());
    }
    cache.push_back({h, std::string{source}, built});
    return built;
}

// ============================================================================
// StreamingMarkdown — progressive per-block rendering
// ============================================================================

namespace {

enum class IntraBlank : uint8_t { No, Yes, Unknown };

// Classify a blank-line position `i` (where src[i] == '\n' and that '\n' is
// at line start, meaning a \n\n separator has just landed) as either
//   No      — an ordinary block boundary, commit here
//   Yes     — intra-list whitespace, do NOT split the list at this point
//   Unknown — next line hasn't arrived yet, defer the decision
//
// Why we need this: find_block_boundary commits on every blank line. A
// loose ordered list like
//   1. item one
//
//   2. item two
//
//   3. item three
// emits two blank lines that BOTH satisfy the blank-line check. Without
// this guard each item commits as its own single-item md::List block. The
// rendered numbering is preserved (start_num carries through), but the
// per-item ComponentElement caching + diff path is fragile across the
// chain of mini-commits: a streaming-time frame where the live tail had
// already shown item N as inline text, captured into native scrollback
// before the commit re-rendered it as a proper list row, leaves the
// scrollback row holding the inline version while the live frame writes
// the list version one row higher. Net effect: individual items vanish
// from the user's view of the conversation (reported as "items 1 and 4
// of a numbered list disappear"). Keeping the list cohesive until it
// genuinely ends means commit_range sees the whole thing once and renders
// one md::List Element that the diff path treats as a single unit.
//
// The Unknown verdict matters because at streaming time we frequently
// reach a blank line while the NEXT line is still arriving — we can't
// tell yet whether it's "2. item two" (intra-list, defer) or "Plain
// paragraph" (terminate list, commit). Returning Unknown stops the
// scanner at the blank without consuming it; the next call resumes here
// once more bytes land.
IntraBlank classify_blank_line(std::string_view src, std::size_t i) noexcept {
    // i is the position of the second '\n' in a \n\n pair (line-start
    // sentinel). The previous character must be '\n', otherwise this
    // wasn't called from the blank-line branch — defensive.
    if (i == 0 || src[i - 1] != '\n') return IntraBlank::No;

    // ── Look back: walk to the start of the line ending at src[i-1] ──
    std::size_t prev_end = i - 1;            // exclusive of the '\n' at i-1
    std::size_t prev_start = 0;
    for (std::size_t k = prev_end; k > 0; --k) {
        if (src[k - 1] == '\n') { prev_start = k; break; }
    }
    std::string_view prev_line = src.substr(prev_start, prev_end - prev_start);
    bool prev_is_ol = ol_marker_len(prev_line) > 0;
    bool prev_is_ul = ul_marker_len(prev_line) > 0;
    if (!prev_is_ol && !prev_is_ul) return IntraBlank::No;
    int prev_indent = count_indent(prev_line);

    // ── Look forward: skip any additional consecutive blanks, then read
    //                  the next line. Loose lists sometimes carry two
    //                  blank lines between items; treat the whole run
    //                  as one separator.
    std::size_t j = i;
    while (j < src.size() && src[j] == '\n') ++j;
    if (j >= src.size()) return IntraBlank::Unknown;  // next line not here yet

    std::size_t next_end = src.find('\n', j);
    bool next_complete = (next_end != std::string_view::npos);
    if (!next_complete) next_end = src.size();
    std::string_view next_line = src.substr(j, next_end - j);

    bool next_is_ol = ol_marker_len(next_line) > 0;
    bool next_is_ul = ul_marker_len(next_line) > 0;

    if (!next_is_ol && !next_is_ul) {
        // Next line has no marker. If it's still incomplete we can't
        // tell — it might become "2. ..." once a space + body arrives,
        // or it might become "Plain paragraph". Wait.
        if (!next_complete) return IntraBlank::Unknown;
        return IntraBlank::No;
    }

    // Both prev and next are list markers. Same kind + same indent
    // (±1, matching parse_markdown_impl's tolerance at line 1845) means
    // they belong to the same list.
    int next_indent = count_indent(next_line);
    bool same_kind = (prev_is_ol && next_is_ol) || (prev_is_ul && next_is_ul);
    bool same_indent = std::abs(next_indent - prev_indent) <= 1;
    if (same_kind && same_indent) return IntraBlank::Yes;
    return IntraBlank::No;
}

// ── Table finality predicate ─────────────────────────────────────────────
//
// GFM table = header row (`| … |`), delimiter row (`| --- | :--: | …`),
// zero or more body rows (`| … |`).  The table ENDS at the first line
// that does not start with `|`.
//
// The streaming scanner's default rule ("commit on blank line") works
// for tables already — a model that emits `\ntable\n\nprose` gets the
// table committed at the blank.  This predicate adds the second case:
// `\ntable\nprose` (no blank line between) commits the table at the
// start of `prose`.  The motivation is that long tables (>10 rows)
// otherwise stay in the tail rendered as literal `|`-text — the user
// watches the formatted version snap in only at end-of-turn.
//
// Ghosting-free proof:
//   • We only return EndsAt(pos) when (a) the delimiter row is
//     fully terminated by '\n' (so the table shape is proven), AND
//     (b) we find a fully-terminated non-`|` line below the table
//     (so the table is proven complete).  Otherwise we return
//     NotATable (caller advances past this line as normal) or
//     Incomplete (caller must NOT advance past this line — a
//     pending table could still resolve once more bytes arrive).
//   • The committed range ends at the start of the non-`|` line, so
//     every byte of the table moves into prefix_->blocks in one
//     transition.  No row is painted-then-mutated.
//   • The non-`|` line itself stays in the tail (uncommitted) and
//     follows the usual inline-paragraph monotonicity rules.
enum class TableScan : std::uint8_t {
    NotATable,    // header line doesn't start with '|', or delimiter
                  // row is fully present but malformed. Caller treats
                  // this line as ordinary prose and advances.
    Incomplete,   // looks like a table so far but finality can't be
                  // proven yet (delimiter not arrived, or body still
                  // open). Caller MUST NOT advance past the header
                  // line — the scanner needs to re-examine it next
                  // call when more bytes have landed.
    EndsAt,       // table is complete; .pos is the byte index of the
                  // first non-`|` line.
};
struct TableScanResult {
    TableScan   kind;
    std::size_t pos;  // only meaningful for EndsAt
};

TableScanResult find_table_end(std::string_view src,
                               std::size_t line_start) noexcept
{
    // Skip leading spaces (up to 3 — GFM allows). A leading tab or
    // ≥4-space indent makes this a code line, not a table.
    auto first_non_space = [](std::string_view s) -> std::size_t {
        std::size_t k = 0;
        while (k < 4 && k < s.size() && s[k] == ' ') ++k;
        return k;
    };

    // Header row: if not yet terminated by '\n' we can't even check
    // that it starts with '|' meaningfully (the line is still being
    // built) — defer.
    std::size_t header_end = src.find('\n', line_start);
    if (header_end == std::string_view::npos) {
        // The caller already knows source_[line_start] == '|', so the
        // line at least PURPORTS to be a table row. Hold here.
        return {TableScan::Incomplete, 0};
    }
    auto header_line = src.substr(line_start, header_end - line_start);
    std::size_t h_off = first_non_space(header_line);
    if (h_off >= header_line.size() || header_line[h_off] != '|')
        return {TableScan::NotATable, 0};

    // Delimiter row.
    std::size_t delim_start = header_end + 1;
    if (delim_start >= src.size()) {
        // Header is terminated but delimiter hasn't started arriving.
        // Could still become a table — defer.
        return {TableScan::Incomplete, 0};
    }
    std::size_t delim_end = src.find('\n', delim_start);
    if (delim_end == std::string_view::npos) {
        // Delimiter line in progress — defer.
        return {TableScan::Incomplete, 0};
    }
    auto delim_line = src.substr(delim_start, delim_end - delim_start);
    std::size_t d_off = first_non_space(delim_line);
    if (d_off >= delim_line.size() || delim_line[d_off] != '|')
        return {TableScan::NotATable, 0};
    bool has_dash = false;
    bool ok = true;
    for (std::size_t k = d_off; k < delim_line.size(); ++k) {
        char c = delim_line[k];
        if (c == '-') { has_dash = true; continue; }
        if (c == '|' || c == ':' || c == ' ' || c == '\t' || c == '\r')
            continue;
        ok = false; break;
    }
    if (!ok || !has_dash) return {TableScan::NotATable, 0};

    // Walk body rows: each line that starts with '|' (after ≤3 spaces)
    // belongs to the table.  Stop at the first line that doesn't —
    // that line is the finality boundary.  Require it to be fully
    // terminated (have a '\n') so we don't commit speculatively on a
    // line that might yet become `| ...`.
    std::size_t pos = delim_end + 1;
    while (pos < src.size()) {
        std::size_t eol = src.find('\n', pos);
        if (eol == std::string_view::npos) {
            // Last line of the buffer isn't terminated. Can't prove
            // finality — might become a table row when more bytes
            // arrive. Defer (but the table SHAPE is proven, so this
            // is Incomplete, not NotATable).
            return {TableScan::Incomplete, 0};
        }
        auto line = src.substr(pos, eol - pos);
        std::size_t off = first_non_space(line);
        if (off >= line.size()) {
            // Blank line. The default blank-line rule will commit
            // here — hand off cleanly. Tell the caller to STOP the
            // scan at this line start (we don't want to advance past
            // the table-header line before the blank-line rule has
            // had a chance to fire normally).
            return {TableScan::Incomplete, 0};
        }
        if (line[off] != '|') {
            // Found the finality boundary.
            return {TableScan::EndsAt, pos};
        }
        pos = eol + 1;
    }
    // Reached end of buffer with all lines `|`-prefixed and terminated.
    // Still incomplete: a future byte could be more table or a closing
    // line.
    return {TableScan::Incomplete, 0};
}

} // anonymous namespace

size_t StreamingMarkdown::find_block_boundary() noexcept {
    // Resumable scan — see the scan_* / in_code_fence_ comments in the
    // header for the full design. The body below is byte-for-byte the
    // same boundary detection logic the prior const version had; only
    // the bookkeeping around it (cursor / fence parity / last-boundary)
    // is now persisted across calls instead of re-derived from
    // committed_ every time.
    //
    // The scan walks ONE LINE START at a time: i points at the byte
    // following a '\n' (or i == 0).  Detecting the blank-line separator
    // looks BACKWARD at line start: source_[i] == '\n' implies
    // source_[i-1] was also '\n' (or i == 0), i.e. we just crossed a
    // \n\n separator. Same correction applies to the heading marker.
    size_t i             = scan_cursor_;
    bool   in_fence      = scan_in_fence_;
    size_t last_boundary = scan_last_boundary_;

    while (i < source_.size()) {
        bool at_line_start = (i == 0 || source_[i - 1] == '\n');

        if (at_line_start) {
            bool is_code_fence = i + 3 <= source_.size() &&
                ((source_[i] == '`' && source_[i+1] == '`' && source_[i+2] == '`') ||
                 (source_[i] == '~' && source_[i+1] == '~' && source_[i+2] == '~'));
            bool is_math_fence = !is_code_fence && i + 2 <= source_.size() &&
                source_[i] == '$' && source_[i+1] == '$';
            if (is_code_fence || is_math_fence) {
                // Opening fence commits any prose that preceded it: the
                // paragraph above can be rendered immediately with its
                // final styling, even if no blank line separates them.
                if (!in_fence) last_boundary = i;
                size_t eol = source_.find('\n', i);
                if (eol == std::string::npos) break;
                in_fence = !in_fence;
                i = eol + 1;
                if (!in_fence) last_boundary = i;
                continue;
            }

            if (!in_fence) {
                // Blank line: we're at the second '\n' of a \n\n pair
                // (or source begins with '\n' at i == 0).  Default
                // behaviour: commit up to and including the second '\n';
                // the next block starts at i + 1.  Exception: intra-list
                // whitespace (loose-list blanks between two same-kind
                // list-item lines at the same indent) must NOT split
                // the list — see classify_blank_line for the rationale.
                if (source_[i] == '\n') {
                    auto verdict = classify_blank_line(
                        std::string_view{source_}, i);
                    if (verdict == IntraBlank::Yes) {
                        // Step past the blank without advancing
                        // last_boundary; the list stays cohesive in
                        // the tail until it genuinely ends.
                        i = i + 1;
                        continue;
                    }
                    if (verdict == IntraBlank::Unknown) {
                        // Next line hasn't fully arrived. Stop the
                        // scan here without consuming the blank so
                        // the next call resumes from this position
                        // when more bytes are present.
                        break;
                    }
                    last_boundary = i + 1;
                    i = i + 1;
                    continue;
                }
                // ATX heading at line start.  Commit the prose before
                // the heading; the heading line itself stays in the
                // tail until a later boundary (next blank, next heading,
                // fence open, or stream end) advances past it.  A list
                // marker (`-` `*` `+`) / blockquote (`>`) is
                // deliberately NOT treated as a boundary — a
                // list/blockquote is a single cohesive block and
                // committing each row separately would stretch a tight
                // list into singletons separated by the inter-block gap.
                //
                // Tables ARE treated as a boundary once proven complete
                // (see find_table_end below) — the rest of the table
                // commits as one cohesive block on the first non-`|`
                // line, instead of waiting for a trailing blank that
                // may never arrive in a long-table message.
                if (source_[i] == '#') {
                    last_boundary = i;
                }
                if (source_[i] == '|') {
                    auto r = find_table_end(
                        std::string_view{source_}, i);
                    if (r.kind == TableScan::EndsAt) {
                        // Commit the prose-before-table at the table's
                        // start (the table itself commits at r.pos
                        // — we set last_boundary there and fast-
                        // forward i past the proven table bytes).
                        last_boundary = i;
                        i = r.pos;
                        last_boundary = r.pos;
                        continue;
                    }
                    if (r.kind == TableScan::Incomplete) {
                        // Looks like a table but finality not yet
                        // proven. STOP the scan here without
                        // advancing past this line — the scanner is
                        // resumable, and we MUST re-enter this same
                        // `|` check on the next call once more bytes
                        // have landed. Advancing past the header now
                        // would leak the table into the
                        // blank-line-only commit path.
                        break;
                    }
                    // TableScan::NotATable — fall through to the
                    // normal line-step loop. The `|` is just a
                    // literal pipe in prose.
                }
            }
        }

        size_t eol = source_.find('\n', i);
        if (eol == std::string::npos) break;
        i = eol + 1;
    }

    // Persist where the scanner stopped so the next call resumes here.
    // i is either == source_.size() (drained), or pointing at a byte
    // we couldn't advance past (find('\n', i) returned npos because
    // the current line hasn't terminated yet) — both correct resume
    // points; new bytes appended to source_ will be visible on the
    // next call without re-walking the prefix.
    scan_cursor_        = i;
    scan_in_fence_      = in_fence;
    scan_last_boundary_ = last_boundary;
    return last_boundary;
}

// Parse [committed_, boundary), extend ref_defs_ with any new defs, push
// rendered blocks onto prefix_->blocks, and advance the fence-state tracker.  Shared
// between set_content() and append() — both need the exact same transition.
void StreamingMarkdown::commit_range(size_t boundary) {
    auto new_text = std::string_view{source_}.substr(committed_,
                                                     boundary - committed_);

    // Parse with our accumulated ref-def map as the resolution target, so
    // defs found in earlier commits are visible to links in this chunk, and
    // new defs found in this chunk become visible to future tail parses.
    std::string cleaned = collect_ref_defs(new_text, ref_defs_);
    ::maya::md_detail::RefDefsScope guard(&ref_defs_);
    auto parsed = parse_markdown_impl(cleaned, 0);

    auto& prefix_blocks = prefix_->blocks;
    prefix_blocks.reserve(prefix_blocks.size() + parsed.blocks.size());
    for (auto& block : parsed.blocks) {
        // Heap-allocate the rendered Element and stash a shared_ptr.
        // Subsequent prefix generations reference the same heap
        // Elements (no move/copy of the BoxElement body when the
        // blocks vector reallocates past capacity).
        prefix_blocks.push_back(
            std::make_shared<const Element>(md_block_to_element(block)));
    }
    if (!parsed.blocks.empty()) ++prefix_->generation;

    for (size_t j = committed_; j < boundary; ++j) {
        bool at_line_start = (j == 0 || source_[j - 1] == '\n');
        if (!at_line_start) continue;
        bool is_code = j + 3 <= boundary &&
            ((source_[j] == '`' && source_[j+1] == '`' && source_[j+2] == '`') ||
             (source_[j] == '~' && source_[j+1] == '~' && source_[j+2] == '~'));
        bool is_math = !is_code && j + 2 <= boundary &&
            source_[j] == '$' && source_[j+1] == '$';
        if (is_code || is_math) in_code_fence_ = !in_code_fence_;
    }

    committed_ = boundary;
    build_dirty_ = true;

    // Keep scanner state coherent with the new committed_:
    //   • scan_last_boundary_ has just been "consumed" — anchor it to
    //     committed_ so the next find call doesn't re-return the same
    //     boundary as if it were still pending.
    //   • finish() can call commit_range(source_.size()) which jumps
    //     committed_ past scan_cursor_; in that case the scanner needs
    //     to be repositioned to committed_ and its fence parity
    //     resynchronised with in_code_fence_ (which the loop above
    //     just updated to the parity at boundary). For the normal
    //     append_safe path (boundary == scan_last_boundary_ ≤
    //     scan_cursor_) this branch is a no-op.
    scan_last_boundary_ = committed_;
    if (scan_cursor_ < committed_) {
        scan_cursor_   = committed_;
        scan_in_fence_ = in_code_fence_;
    }
}

// Internal: append codepoint-clean bytes that have already passed through
// the StreamSink.  Public entry points all funnel here.
void StreamingMarkdown::append_safe(std::string_view safe_bytes) {
    if (safe_bytes.empty()) return;
    source_.append(safe_bytes.data(), safe_bytes.size());
    build_dirty_ = true;

    size_t boundary = find_block_boundary();
    if (boundary > committed_) commit_range(boundary);
}

void StreamingMarkdown::set_content(std::string_view content) {
    // Fast-path: unchanged → no work. This dominates because the view layer
    // calls set_content(streaming_text) every single frame.
    if (content.size() == source_.size() &&
        (content.empty() || std::memcmp(content.data(), source_.data(),
                                        content.size()) == 0)) {
        return;
    }

    // Growth with identical prefix → append only the new bytes through the
    // StreamSink so partial-codepoint suffixes are buffered safely.  This
    // dominates real-world streaming usage (each frame's set_content is
    // the previous frame's content + a few new bytes).
    if (content.size() > source_.size() &&
        (source_.empty() || std::memcmp(content.data(), source_.data(),
                                        source_.size()) == 0)) {
        std::string_view delta{content.data() + source_.size(),
                               content.size() - source_.size()};
        std::string safe = sink_.feed(delta);
        if (!safe.empty()) append_safe(safe);
        return;
    }

    // Replace path: the new content diverges from the old prefix, so we
    // can't just append.  Reset and feed everything through the sink to
    // keep the codepoint-integrity guarantee.
    clear();
    std::string safe = sink_.feed(content);
    if (!safe.empty()) append_safe(safe);
}

void StreamingMarkdown::append(std::string_view text) {
    if (text.empty()) return;
    // Route every incoming chunk through the sink so the source_ buffer
    // never contains a half-written multi-byte codepoint or ANSI escape.
    // Anything mid-sequence is held in the sink's carry buffer until its
    // continuation arrives in a later append.
    std::string safe = sink_.feed(text);
    if (!safe.empty()) append_safe(safe);
}

void StreamingMarkdown::finish() {
    // Drain any held tail bytes from the sink before committing.  At
    // end-of-stream we accept that a half-decoded codepoint may surface
    // (it's better than dropping bytes silently); the renderer will
    // display invalid bytes as the U+FFFD replacement glyph.
    std::string pending = sink_.flush();
    if (!pending.empty()) append_safe(pending);

    if (committed_ < source_.size()) {
        commit_range(source_.size());
        in_code_fence_ = false;
    }
}

void StreamingMarkdown::clear() {
    source_.clear();
    committed_ = 0;
    // Replace the prefix snapshot rather than mutating the existing one
    // — any ComponentElement still capturing the old shared_ptr (held
    // inside cached_build_ until that's reassigned below) sees the
    // pre-clear content, and the next build() will allocate a fresh
    // prefix_ + ComponentElement so the renderer's component_cache
    // misses cleanly on the new instance instead of holding stale
    // entries against an unrelated subsequent stream.
    prefix_ = std::make_shared<CommittedPrefix>();
    ref_defs_.clear();
    in_code_fence_ = false;
    sink_.reset();
    build_dirty_ = true;
    cached_tail_size_ = 0;
    // Reset the resumable boundary scanner.
    scan_cursor_        = 0;
    scan_in_fence_      = false;
    scan_last_boundary_ = 0;
    // Reset the inline-tail cache. Strictly speaking the prefix-match
    // check at render_tail entry is self-correcting (any new tail will
    // mismatch and re-parse), but clearing here avoids carrying KB of
    // stale string state past a logical reset.
    tail_inline_cache_prefix_hash_ = 0;
    tail_inline_cache_prefix_len_  = 0;
    tail_inline_cache_content_.clear();
    tail_inline_cache_runs_.clear();
    // Reset the build-cache shape flags so the next build() falls into
    // the full-rebuild path rather than trying to mutate a stale
    // structural template.
    cached_prefix_gen_    = 0;
    cached_has_tail_      = false;
    cached_has_prefix_    = false;
    cached_tail_in_fence_ = false;
    cached_tail_hash_     = 0;
    cached_tail_len_      = 0;
    cached_build_         = Element{TextElement{""}};
}

// Render the uncommitted tail as a monotonic in-progress paragraph.
//
// MONOTONICITY PROOF: this function never produces an element whose
// height *decreases* as bytes are appended to `tail`.
//
//   - Inside an open code fence: we render the tail as a literal text
//     element with TextWrap::Wrap, so its height is ceil(byte_count / W)
//     for terminal width W — strictly nondecreasing in byte_count.
//
//   - Otherwise: we parse `tail` as INLINE-only markdown (no block
//     recognition) via parse_inlines() and render through the same
//     wrapped-paragraph builder (build_inline_row).  Inline parse
//     produces a flat sequence of styled text spans whose total
//     character count is monotonic in byte_count.  Block markers at
//     line starts (|, -, *, >, #, etc.) are invisible to parse_inlines
//     and render as literal characters — so the tail never reflows
//     between "paragraph" and "table" / "list" / "blockquote" mid-stream.
//
//   - The block-level interpretation only happens when find_block_boundary
//     advances past the tail (on a blank line or closing fence), at
//     which point commit_range() moves the bytes into prefix_->blocks as a
//     properly-formatted Element and a new (empty) tail begins.  The
//     formatting "snap" is one-time per block and lands at a moment
//     when the inline diff sees a single coherent transition.
//
// This is the layer that makes ghosting structurally impossible for
// streaming markdown — there is no frame in which the renderer is
// asked to compare two element trees of different shape.
Element StreamingMarkdown::render_tail(std::string_view tail) const {
    // Skip leading newlines (whitespace from a prior commit boundary).
    std::size_t start = 0;
    while (start < tail.size() && tail[start] == '\n') ++start;
    if (start >= tail.size()) {
        return Element{TextElement{}};
    }
    std::string_view body = tail.substr(start);

    if (in_code_fence_) {
        // Inside an open code fence: render the literal bytes (including
        // the opening fence marker if it's still in this tail).  Wrap
        // mode keeps height monotonic in byte count.
        return Element{TextElement{
            .content = std::string{body},
            .style   = Style{}.with_fg(colors::code_fg),
            .wrap    = TextWrap::Wrap,
        }};
    }

    // ── Open code fence at tail start: render in its committed shape
    //    (round border + language label + syntax-highlighted body) so
    //    the user sees the formatted code block from the moment the
    //    opening ``` arrives, not after the closing ``` lands.  The
    //    closing fence hasn't arrived (find_block_boundary would have
    //    committed past it), so the tail is a verbatim opener line +
    //    in-progress body.  Monotonic in byte count: body grows row by
    //    row inside the same border frame.
    if (body.size() >= 3 &&
        ((body[0] == '`' && body[1] == '`' && body[2] == '`') ||
         (body[0] == '~' && body[1] == '~' && body[2] == '~')))
    {
        auto eol = body.find('\n');
        std::string lang;
        std::string code;
        if (eol == std::string_view::npos) {
            // Opener line not yet terminated: treat the post-fence
            // characters as the language hint and the body as empty.
            // The ``` itself is hidden so the row doesn't flicker the
            // marker bytes before the line break arrives.
            std::string_view lsv = body.substr(3);
            while (!lsv.empty() && (lsv.back() == ' ' || lsv.back() == '\r'))
                lsv.remove_suffix(1);
            while (!lsv.empty() && lsv.front() == ' ')
                lsv.remove_prefix(1);
            lang = std::string{lsv};
        } else {
            std::string_view lsv = body.substr(3, eol - 3);
            while (!lsv.empty() && (lsv.back() == ' ' || lsv.back() == '\r'))
                lsv.remove_suffix(1);
            while (!lsv.empty() && lsv.front() == ' ')
                lsv.remove_prefix(1);
            lang = std::string{lsv};
            code = std::string{body.substr(eol + 1)};
        }
        // Match md_block_to_element's CodeBlock styling exactly — same
        // builder shape, same align_self(Stretch) so the in-flight tail
        // render and the committed block render produce identical
        // borders. Without the stretch the streaming-tail border tracks
        // the longest emitted line, drifting frame-to-frame.
        auto builder = detail::vstack()
            .align_self(Align::Stretch)
            .border(BorderStyle::Round)
            .border_color(colors::code_border)
            .padding(0, 1, 0, 1);
        if (!lang.empty()) {
            std::string label = " " + lang + " ";
            if (lang == "mermaid" || lang == "matrix" ||
                lang == "math"    || lang == "latex"  ||
                lang == "dot"     || lang == "graphviz")
            {
                label = " " + lang + " (diagram) ";
            }
            builder = std::move(builder).border_text(
                std::move(label),
                BorderTextPos::Top,
                BorderTextAlign::Start);
        }
        return builder(highlight_code(code, lang));
    }

    // ── ATX heading at tail start: render the first line in its
    //    committed shape (bold + heading colour, hash markers stripped)
    //    so the heading style appears as soon as `# ` is typed instead
    //    of waiting for the next blank line to commit.  Monotonic: the
    //    heading line grows by one character per byte (single-line
    //    height stays at 1, multi-line wrap only ADDS rows).  Any
    //    follow-up content stays as inline parse below — promoting it
    //    to block parse would break monotonicity (list/table/quote
    //    snapping in/out of shape causes ghosting).
    int hashes = 0;
    while (hashes < 6 && static_cast<size_t>(hashes) < body.size() &&
           body[hashes] == '#') ++hashes;
    if (hashes > 0 && static_cast<size_t>(hashes) < body.size() &&
        body[hashes] == ' ')
    {
        auto eol = body.find('\n');
        std::string_view first = (eol == std::string_view::npos)
                                ? body
                                : body.substr(0, eol);
        std::string_view rest  = (eol == std::string_view::npos)
                                ? std::string_view{}
                                : body.substr(eol + 1);
        std::string_view text  = first.substr(static_cast<size_t>(hashes) + 1);

        Style sty = Style{}.with_bold();
        switch (hashes) {
            case 1: sty = sty.with_fg(colors::heading1); break;
            case 2: sty = sty.with_fg(colors::heading2); break;
            case 3: sty = sty.with_fg(colors::heading3); break;
            default: sty = sty.with_fg(colors::heading3).with_dim(); break;
        }
        Element heading_el = Element{TextElement{
            .content = std::string{text},
            .style   = sty,
        }};
        while (!rest.empty() && rest.front() == '\n') rest.remove_prefix(1);
        if (rest.empty()) return heading_el;
        auto spans = parse_inlines(rest);
        return detail::vstack().gap(1)(
            std::move(heading_el),
            build_inline_row(spans)
        );
    }

    // Inline-only parse for the rest — block markers (|, -, *, >) stay
    // literal until a real boundary advances past them.  This is the
    // monotonicity floor: a paragraph can only GROW row count as bytes
    // arrive, never shrink, so the renderer never has to clear stale
    // rows and ghosting is structurally impossible.  Lists / tables /
    // blockquotes pay a one-frame "snap" when the trailing blank line
    // commits the whole thing as one cohesive block.
    //
    // ── Sliding cache: parse only the live line ──
    //
    // Split body at the last '\n'. The "stable prefix" (everything
    // through and including the last '\n') is cache-keyed by its bytes;
    // the "live line" (post-newline tail) is re-parsed every frame.
    // Markdown emphasis / code-span / link delimiters never cross
    // newline boundaries in well-formed input — when the model emits
    // `**bold**\nNext line`, the parser state at the '\n' is "fresh,
    // no open delimiters" — so the split is parser-state-safe.
    //
    // Cost transition: prior code re-parsed the full body (O(tail)
    // per frame). Now per-frame work is O(live_line_length) on the
    // common case, with an O(tail) refresh once per newline boundary
    // (rare: bytes arrive faster than line breaks during streaming).
    auto last_nl = body.rfind('\n');
    std::string_view stable_prefix = (last_nl == std::string_view::npos)
        ? std::string_view{}
        : body.substr(0, last_nl + 1);
    std::string_view live_line = (last_nl == std::string_view::npos)
        ? body
        : body.substr(last_nl + 1);

    // Refresh the prefix cache only if it differs from what's stored.
    // Hash + length compare — zero allocations, single-pass over the
    // prefix bytes (vs. the prior approach which kept a std::string copy
    // and compared bytewise). Steady state with growing live line:
    // hash matches, content/runs are reused verbatim, no copy of the
    // multi-KB stable prefix every frame.
    const std::uint64_t prefix_hash = fnv1a64(stable_prefix);
    if (tail_inline_cache_prefix_len_  != stable_prefix.size()
        || tail_inline_cache_prefix_hash_ != prefix_hash) {
        tail_inline_cache_content_.clear();
        tail_inline_cache_runs_.clear();
        if (!stable_prefix.empty()) {
            auto prefix_spans = parse_inlines(stable_prefix);
            const Style base = Style{}.with_fg(colors::text);
            for (const auto& s : prefix_spans) {
                flatten_inline(s, base, tail_inline_cache_content_,
                               tail_inline_cache_runs_);
            }
        }
        tail_inline_cache_prefix_len_  = stable_prefix.size();
        tail_inline_cache_prefix_hash_ = prefix_hash;
    }

    // Build the result: cached prefix runs + freshly-parsed live-line
    // runs. flatten_inline writes run offsets relative to `out.size()`
    // at write time, so seeding `content` / `runs` from the cache
    // keeps the appended-line offsets globally correct.
    std::string content                  = tail_inline_cache_content_;
    std::vector<StyledRun> runs          = tail_inline_cache_runs_;
    if (!live_line.empty()) {
        auto live_spans = parse_inlines(live_line);
        const Style base = Style{}.with_fg(colors::text);
        for (const auto& s : live_spans) {
            flatten_inline(s, base, content, runs);
        }
    }

    if (runs.empty()) return Element{TextElement{}};
    if (runs.size() == 1) {
        return Element{TextElement{
            .content = std::move(content),
            .style   = runs[0].style,
        }};
    }
    return Element{TextElement{
        .content = std::move(content),
        .style   = Style{}.with_fg(colors::text),
        .runs    = std::move(runs),
    }};
}

const Element& StreamingMarkdown::build() const {
    // Untouched-since-last-build: return cached. Dominant case when the
    // widget is idle (no streaming).
    if (!build_dirty_) return cached_build_;

    std::string_view tail = (committed_ < source_.size())
        ? std::string_view{source_}.substr(committed_)
        : std::string_view{};
    const bool has_tail   = !tail.empty();
    const bool has_prefix = !prefix_->blocks.empty();

    // ── Empty special case ──
    if (!has_prefix && !has_tail) {
        cached_build_      = Element{TextElement{""}};
        cached_tail_size_  = 0;
        cached_prefix_gen_ = prefix_->generation;
        cached_has_tail_   = false;
        cached_has_prefix_ = false;
        build_dirty_       = false;
        return cached_build_;
    }

    // ── Tail-only fast path ──
    //
    // The hot streaming case: prefix unchanged, has_tail/has_prefix
    // shape unchanged, only the tail's content moved. We can mutate
    // cached_build_'s tail child (the LAST child of the outer vstack)
    // in place — every prefix child keeps its address and per-component
    // caches stay warm, while only render_tail's lightweight inline-
    // cache path runs.
    //
    // Sub-fast-path: if the tail body bytes AND the committed-side
    // fence parity are byte-identical to the cache, render_tail's
    // output is provably identical to what's already in
    // children.back() — skip the call and the assignment entirely,
    // leaving cached_build_ wholly untouched. The bytes-equality check
    // matters for the case where build_dirty_ flipped (e.g., a feed
    // arrived) but the resulting tail body didn't actually change
    // (commit_range absorbed the new bytes; the body shape happened
    // to round-trip; any future code path that bumps build_dirty_
    // without changing the tail).

    // Helper: build the prefix ComponentElement for the current
    // generation. Used by both the full-rebuild path and the
    // commit-fast-path below. Hoisted into a lambda so the cache_id
    // formatting and lambda-capture wiring don't drift between the two
    // call sites (a previous bug where they did caused the prefix
    // ComponentElement to skip its cache invalidation on commit and
    // ghost the previous frame's prefix bitmap into the new prefix's
    // layout slot).
    auto make_prefix_component = [&]() -> Element {
        auto p = prefix_;
        ComponentElement comp;
        char id_buf[48];
        std::snprintf(id_buf, sizeof(id_buf), "#strmd-%lx-%lu",
                      static_cast<unsigned long>(instance_id_),
                      static_cast<unsigned long>(p->generation));
        comp.cache_id = id_buf;
        comp.render = [p](int /*w*/, int /*h*/) -> Element {
            std::vector<Element> kids;
            kids.reserve(p->blocks.size());
            for (const auto& sp : p->blocks) {
                kids.push_back(*sp);
            }
            return detail::vstack().gap(1)(std::move(kids)).build();
        };
        return Element{std::move(comp)};
    };

    if (cached_prefix_gen_ == prefix_->generation
        && cached_has_prefix_ == has_prefix
        && cached_has_tail_   == has_tail
        && std::holds_alternative<BoxElement>(cached_build_.inner)) {
        auto& box = std::get<BoxElement>(cached_build_.inner);
        const std::size_t prefix_n =
            has_prefix ? prefix_->blocks.size() : 0u;
        const std::size_t expected = prefix_n + (has_tail ? 1u : 0u);
        if (box.children.size() == expected) {
            if (has_tail) {
                // Hash + length + fence parity together uniquely identify
                // render_tail's output. Compute the hash once; on hit we
                // skip render_tail entirely. On miss we recompute the
                // tail Element AND update the cached digest — zero heap
                // copy of the tail bytes regardless of outcome.
                const std::uint64_t tail_hash = fnv1a64(tail);
                const bool tail_unchanged =
                    cached_tail_in_fence_ == in_code_fence_
                    && cached_tail_len_   == tail.size()
                    && cached_tail_hash_  == tail_hash;
                if (!tail_unchanged) {
                    // Swap in the new tail. children.back() is the
                    // tail slot regardless of whether prefix is
                    // present:
                    //   [prefix, tail] → children[1]
                    //   [tail]         → children[0]
                    box.children.back() = render_tail(tail);
                    cached_tail_hash_     = tail_hash;
                    cached_tail_len_      = tail.size();
                    cached_tail_in_fence_ = in_code_fence_;
                }
            }
            cached_tail_size_ = tail.size();
            build_dirty_      = false;
            return cached_build_;
        }
    }

    // ── Commit fast path ──
    //
    // Generation moved (commit_range fired), but the outer vstack's
    // shape (has_prefix / has_tail) is unchanged. We can mutate the
    // existing BoxElement in place: swap children[0] (the prefix
    // ComponentElement) for a fresh one carrying the new generation in
    // its cache_id, and refresh the tail child if needed. The outer
    // BoxElement's layout/border/padding config stays as-is — no
    // builder pipeline, no ElementBuilder allocation, no .build()
    // call, no walk over the unrelated metadata fields.
    //
    // Without this branch every commit (which happens at every
    // paragraph boundary during a long streaming reply, ~10-100x per
    // turn) fell through to the full-rebuild path, which recreates
    // the outer vstack from scratch — a small cost individually but
    // taken at exactly the rate the user is most sensitive to
    // (visible UI tearing / pause when each new block lands).
    if (cached_prefix_gen_ != prefix_->generation
        && cached_has_prefix_ == has_prefix && has_prefix
        && cached_has_tail_   == has_tail
        && std::holds_alternative<BoxElement>(cached_build_.inner)) {
        auto& box = std::get<BoxElement>(cached_build_.inner);
        const std::size_t expected = 1u + (has_tail ? 1u : 0u);
        if (box.children.size() == expected) {
            box.children[0] = make_prefix_component();
            if (has_tail) {
                const std::uint64_t tail_hash = fnv1a64(tail);
                const bool tail_unchanged =
                    cached_tail_in_fence_ == in_code_fence_
                    && cached_tail_len_   == tail.size()
                    && cached_tail_hash_  == tail_hash;
                if (!tail_unchanged) {
                    box.children.back() = render_tail(tail);
                    cached_tail_hash_     = tail_hash;
                    cached_tail_len_      = tail.size();
                    cached_tail_in_fence_ = in_code_fence_;
                }
            }
            cached_prefix_gen_ = prefix_->generation;
            cached_tail_size_  = tail.size();
            build_dirty_       = false;
            return cached_build_;
        }
    }

    // ── Full rebuild ──
    //
    // Either the prefix generation moved (commit_range fired), the
    // shape (has_prefix / has_tail) changed, or the cache was reset.
    // Build the outer vstack with N + (has_tail ? 1 : 0) children:
    //   children[0 .. N-1] = committed prefix blocks (deep-copied)
    //   children[last]     = render_tail() output (when has_tail)
    //
    // Why direct children instead of wrapping in a ComponentElement.
    // The earlier approach wrapped each prefix shared_ptr<const Element>
    // via the Element{sp} converting constructor, producing one
    // ComponentElement per block whose cache_id was derived from sp
    // identity. The renderer then cached per-block layout+paint cells
    // by (sp, width), making per-commit cost O(new_block_size) instead
    // of O(transcript_size). The trade was correctness: the
    // ComponentElement cache hit a cells_rows < content_h blit path
    // where the cached cell snapshot was shorter than the layout-
    // allocated content height. When the inline frame later overflowed
    // term_h and pushed rows into the terminal's native (immutable)
    // scrollback, those short cell ranges left blank rows above and
    // below the block's actual content — the "ghost rectangles" that
    // accumulate after 2-3 turns of long streaming. Direct children
    // bypass the cells cache entirely: every frame's layout walks the
    // prefix's pre-built Element values directly, producing the same
    // cells the live frame would have rendered, so the cells captured
    // into native scrollback during overflow are correct.
    //
    // Cost: O(transcript_size) deep-copies of Element variant bodies
    // per generation bump. Pricey on long sessions, but maybe_virtualize
    // bounds the relevant tail of the transcript — older turns get
    // committed to scrollback and their blocks drop out of the active
    // prefix entirely.
    std::vector<Element> outer_children;
    outer_children.reserve(
        (has_prefix ? prefix_->blocks.size() : 0u) + (has_tail ? 1u : 0u));

    if (has_prefix) {
        // Wrap the committed blocks in a single ComponentElement with a
        // generation-derived cache_id. The renderer's component_cache
        // then stores the rendered cells against this key; between
        // commits (generation stable) every render hits the cache and
        // blits cells directly, skipping the deep-copy lambda below
        // entirely. On commit the generation bumps, the cache_id
        // changes, the next render misses and re-runs the lambda once,
        // then caches again. Avoids the per-block `Element{sp}`
        // wrapper (visible blank-row bug) while still skipping the
        // per-frame copy work the previous inline-into-outer_children
        // path was paying at 100% CPU on long streams.
        outer_children.push_back(make_prefix_component());
    }
    if (has_tail) {
        outer_children.push_back(render_tail(tail));
    }

    // align_self(Stretch) forces this vstack to claim the parent's full
    // cross-axis size, regardless of how much its OWN children would
    // naturally span. Without this, early streaming frames where the
    // tail is a short inline-only paragraph (one wrapped row or less)
    // produce a vstack whose natural width = tail length = short, and
    // any parent header doing justify-content: space-between against
    // that width packs its children to the left. Setting Stretch makes
    // the streaming widget's width invariant across the
    // short-tail → multi-row-tail → committed-blocks transitions —
    // the parent's flex math sees the same number every frame, so the
    // header layout doesn't flicker between content-sized and
    // terminal-sized.
    cached_build_ = (
        detail::vstack().gap(1).padding(0, 0, 0, 2)
            .align_self(Align::Stretch)(std::move(outer_children))
    ).build();
    cached_tail_size_     = tail.size();
    cached_prefix_gen_    = prefix_->generation;
    cached_has_tail_      = has_tail;
    cached_has_prefix_    = has_prefix;
    cached_tail_in_fence_ = in_code_fence_;
    if (has_tail) {
        cached_tail_hash_ = fnv1a64(tail);
        cached_tail_len_  = tail.size();
    } else {
        cached_tail_hash_ = 0;
        cached_tail_len_  = 0;
    }
    build_dirty_          = false;
    return cached_build_;
}

// ── Cross-TU bridge ──────────────────────────────────────────────────
namespace md_detail {
Element assemble_markdown(md::Document&& doc) {
    return ::maya::assemble_markdown(std::move(doc));
}
} // namespace md_detail

} // namespace maya
