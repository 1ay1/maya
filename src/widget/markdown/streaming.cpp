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
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
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
#include "maya/app/app.hpp"   // request_animation_frame()

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
//     which auto-derives a stable hash_id from the control block.
//     The renderer's hash-keyed component_cache then short-circuits
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
    // would let the renderer's hash-keyed component_cache cells-blit
    // future paints, BUT it forces the auto-measure path to run an
    // extra inner layout::compute on every cache miss — exactly the
    // regression that broke test_markdown's deep_blockquote stress
    // case. Apps that want cells-cache acceleration should opt in at
    // the WIDGET level (Turn::hash_id) where the outer wrapper makes
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
        if (off >= line.size() || line[off] != '|') {
            // Finality boundary. Either a blank line (off >= size) or
            // a non-pipe line: both end the table. The table's header
            // + delimiter row are already verified above, so the
            // bytes [line_start, pos) are PROVABLY a complete table
            // and can commit now — even if N more tables (each with
            // their own trailing blanks) follow, they will each
            // commit at their own blank-line boundary instead of
            // queueing behind the first non-pipe line in the buffer.
            //
            // Previously the blank-line branch returned Incomplete
            // "so the default blank-line rule fires" — but the outer
            // scanner sees Incomplete and breaks BEFORE reaching its
            // own blank-line check, so the table (and every
            // subsequent block separated only by blank lines) sat
            // uncommitted until a non-pipe line finally landed.
            // Symptom: multiple tables stream in invisibly, then
            // burst-render together when the next paragraph arrives.
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
                // the heading; then, once the heading line itself is
                // fully terminated by '\n', advance the boundary past
                // it so the heading is rendered in its committed shape
                // immediately — without waiting for a trailing blank
                // line that may not arrive until the next paragraph
                // lands (and during long streamed answers, may take
                // seconds to do so).
                //
                // The eager-commit mirrors the table / closing-fence
                // behaviour: a structural block that's *provably*
                // complete commits at the start of the following
                // line; a still-arriving line stays in the tail where
                // render_tail's ATX special case renders it in the
                // committed style so the user sees no styling pop.
                //
                // Why this isn't extended to list / blockquote rows:
                // a list/blockquote is a single cohesive block whose
                // shape is only knowable once the block ENDS
                // (intra-list blanks vs. terminating blanks).
                // Committing each row separately would stretch a
                // tight list into singletons separated by the inter-
                // block gap — same trade we make for tables until
                // find_table_end proves them complete.
                if (source_[i] == '#') {
                    // Verify it's an ATX heading shape (1–6 '#' then
                    // a space) before treating it as a boundary —
                    // bare '#' / '#tag' at line start is a plain
                    // paragraph in CommonMark, not a heading.
                    std::size_t hashes = 0;
                    while (hashes < 6 && i + hashes < source_.size()
                           && source_[i + hashes] == '#') ++hashes;
                    bool is_atx = hashes >= 1 && hashes <= 6
                        && i + hashes < source_.size()
                        && (source_[i + hashes] == ' '
                            || source_[i + hashes] == '\t');
                    if (is_atx) {
                        last_boundary = i;
                        // If the heading line has fully arrived,
                        // anchor the boundary past it so the next
                        // paragraph/heading/etc. starts a fresh
                        // commit. If it hasn't (`eol == npos`), fall
                        // through to the line-step `break` below;
                        // the resumable scan will retry once more
                        // bytes land.
                        std::size_t heading_eol = source_.find('\n', i);
                        if (heading_eol != std::string::npos) {
                            i = heading_eol + 1;
                            last_boundary = i;
                            continue;
                        }
                    }
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

                // Horizontal rule at line start. CommonMark HR: a
                // line containing ONLY repeated `*`, `_`, or `-`
                // (≥3 of the same char), with optional interior
                // spaces, optionally indented ≤3 spaces. Like ATX
                // headings, an HR is a single fully-proven block as
                // soon as the line terminates by '\n' — we can
                // commit the prose before it AND the HR itself in
                // one transition.
                //
                // Setext ambiguity: `---` (and `===`, which we never
                // treat as HR) on a line directly under a non-blank
                // text line is a setext h2 underline, not an HR.
                // The blank-line rule above commits prose at every
                // `\n\n`, so by the time we get HERE inspecting a
                // line at `i`, the previous line is EITHER part of
                // an uncommitted run (could be a setext target) OR
                // already committed (separated by `\n\n`). Only the
                // FORMER case poses setext risk.
                //
                // Conservative gate: only eager-commit `*`/`_` HRs
                // unconditionally (setext doesn't apply — it only
                // uses `=` and `-`). For `-`-style HRs, require the
                // PREVIOUS line to be empty (blank-line separator
                // already in place, so no setext attachment is
                // possible). The fall-through for `-` after text
                // preserves the existing commit-on-blank-line
                // behaviour, which correctly resolves to either HR
                // or setext at commit time via parse_markdown_impl.
                {
                    char c0 = source_[i];
                    if (c0 == '*' || c0 == '_' || c0 == '-') {
                        std::size_t hr_eol = source_.find('\n', i);
                        if (hr_eol != std::string::npos) {
                            // Inspect the line bytes (excluding the
                            // trailing '\n').
                            std::string_view ln{source_.data() + i,
                                                hr_eol - i};
                            // Allow ≤3 leading spaces (GFM).
                            std::size_t k = 0;
                            while (k < 4 && k < ln.size() && ln[k] == ' ') ++k;
                            // Must be the same marker char throughout
                            // the line, with only spaces / '\r'
                            // allowed between markers, and ≥3 marker
                            // chars total.
                            char marker = (k < ln.size()) ? ln[k] : '\0';
                            bool is_hr_shape = (marker == '*'
                                             || marker == '_'
                                             || marker == '-');
                            std::size_t markers = 0;
                            if (is_hr_shape) {
                                for (std::size_t q = k; q < ln.size(); ++q) {
                                    char cc = ln[q];
                                    if (cc == marker) { ++markers; continue; }
                                    if (cc == ' ' || cc == '\t' || cc == '\r')
                                        continue;
                                    is_hr_shape = false;
                                    break;
                                }
                            }
                            if (is_hr_shape && markers >= 3) {
                                // Setext-safety gate for `-`: only
                                // eager-commit when the previous
                                // line is blank (so this can't be
                                // an h2 underline of the line
                                // above). `*` and `_` are always
                                // safe.
                                bool safe = (marker != '-');
                                if (!safe) {
                                    // i is at a line start; previous
                                    // byte is the '\n' that ended
                                    // the prior line. Walk back one
                                    // more byte: if it's also '\n'
                                    // (or i == 1, i.e. the prior
                                    // line was the empty line at
                                    // source start), the prior line
                                    // is blank.
                                    if (i == 0) {
                                        safe = true;
                                    } else if (i >= 2 && source_[i - 2] == '\n') {
                                        safe = true;
                                    } else if (i == 1) {
                                        // source_ == "\n---\n": the
                                        // line above is empty.
                                        safe = true;
                                    }
                                }
                                if (safe) {
                                    last_boundary = i;
                                    i = hr_eol + 1;
                                    last_boundary = i;
                                    continue;
                                }
                            }
                        }
                        // No '\n' yet — the line is still arriving,
                        // fall through to the normal line-step.
                        // Next call resumes here.
                    }
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

    // ── Derive per-block source ranges over [committed_, boundary). ──
    // Walk blank-line separators outside ``` / ~~~ / $$ fences; the
    // resulting segment list mirrors the parser's top-level block
    // segmentation closely (paragraphs / headings / lists / tables /
    // code blocks each occupy one segment). When the segment count
    // matches the parser's block count we attribute 1:1; otherwise we
    // fall back to a monotone synthetic offset so the fold/lookup APIs
    // still have a unique stable key per block.
    //
    // Approximation accepted: the offsets are correct for the common
    // case (one segment == one block), and remain UNIQUE + MONOTONE in
    // every case, which is all the fold map + binary-search reverse
    // lookup require for correctness. Refinement (parser emitting
    // exact source ranges per block) can drop in without changing the
    // public API.
    const size_t base = committed_;
    std::vector<std::pair<size_t, size_t>> seg_ranges;  // [start, end) in source_
    seg_ranges.reserve(parsed.blocks.size() + 1);
    {
        bool seg_in_fence = false;
        size_t seg_start = base;
        size_t k = base;
        // Skip leading blank lines so the first segment starts at real
        // content (matches the parser's behaviour of trimming leading
        // whitespace between blocks).
        while (k < boundary && source_[k] == '\n') ++k;
        seg_start = k;
        while (k < boundary) {
            bool at_ls = (k == base || source_[k - 1] == '\n');
            if (at_ls && k + 3 <= boundary &&
                ((source_[k] == '`' && source_[k+1] == '`' && source_[k+2] == '`') ||
                 (source_[k] == '~' && source_[k+1] == '~' && source_[k+2] == '~'))) {
                seg_in_fence = !seg_in_fence;
                // advance to end of fence line
                size_t eol = std::string_view{source_}.find('\n', k);
                if (eol == std::string_view::npos || eol >= boundary) { k = boundary; break; }
                k = eol + 1;
                // A closing fence ends the segment at its line.
                if (!seg_in_fence) {
                    if (k > seg_start) seg_ranges.emplace_back(seg_start, k);
                    while (k < boundary && source_[k] == '\n') ++k;
                    seg_start = k;
                }
                continue;
            }
            if (!seg_in_fence && at_ls && source_[k] == '\n') {
                // blank line separator
                if (k > seg_start) seg_ranges.emplace_back(seg_start, k);
                while (k < boundary && source_[k] == '\n') ++k;
                seg_start = k;
                continue;
            }
            ++k;
        }
        if (seg_start < boundary)
            seg_ranges.emplace_back(seg_start, boundary);
    }

    auto kind_of = [](const md::Block& b) noexcept -> StreamingMarkdown::BlockKind {
        using K = StreamingMarkdown::BlockKind;
        return std::visit([](const auto& x) noexcept -> K {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, md::Paragraph>)       return K::Paragraph;
            else if constexpr (std::is_same_v<T, md::Heading>)    return K::Heading;
            else if constexpr (std::is_same_v<T, md::CodeBlock>)  return K::CodeBlock;
            else if constexpr (std::is_same_v<T, md::Blockquote>) return K::Blockquote;
            else if constexpr (std::is_same_v<T, md::List>)       return K::List;
            else if constexpr (std::is_same_v<T, md::HRule>)      return K::HRule;
            else if constexpr (std::is_same_v<T, md::Table>)      return K::Table;
            else if constexpr (std::is_same_v<T, md::FootnoteDef>) return K::FootnoteDef;
            else if constexpr (std::is_same_v<T, md::Alert>)      return K::Alert;
            else if constexpr (std::is_same_v<T, md::DefList>)    return K::DefList;
            else if constexpr (std::is_same_v<T, md::Details>)    return K::Details;
            else if constexpr (std::is_same_v<T, md::HtmlBlock>)  return K::HtmlBlock;
            else return K::Other;
        }, b.inner);
    };
    auto lang_of = [](const md::Block& b) noexcept -> std::string {
        if (auto* c = std::get_if<md::CodeBlock>(&b.inner)) return c->lang;
        return {};
    };

    auto& prefix_blocks = prefix_->blocks;
    auto& prefix_metas  = prefix_->metas;
    prefix_blocks.reserve(prefix_blocks.size() + parsed.blocks.size());
    prefix_metas.reserve (prefix_metas.size()  + parsed.blocks.size());

    const bool seg_match = seg_ranges.size() == parsed.blocks.size();
    // Synthetic monotone offset used when segment count diverges from
    // block count. Starts at `base` so it preserves the "newer commit
    // == larger offset" ordering binary search relies on.
    size_t synth = base;
    for (std::size_t bi = 0; bi < parsed.blocks.size(); ++bi) {
        auto& block = parsed.blocks[bi];

        BlockMeta meta;
        if (seg_match) {
            meta.source_offset = seg_ranges[bi].first;
            meta.source_end    = seg_ranges[bi].second;
        } else {
            meta.source_offset = synth;
            // End at the next synth or `boundary` so ranges remain
            // non-overlapping and ordered.
            meta.source_end    = (bi + 1 < parsed.blocks.size())
                ? (synth + 1) : boundary;
            synth += 1;
        }
        meta.kind = kind_of(block);
        meta.lang = lang_of(block);
        // line_count: count '\n' inside the attributed source slice,
        // clamped to uint16 max so a 65k-line code block doesn't wrap.
        std::size_t lc = 0;
        if (meta.source_end > meta.source_offset && meta.source_end <= source_.size()) {
            for (std::size_t q = meta.source_offset; q < meta.source_end; ++q)
                if (source_[q] == '\n') ++lc;
        }
        meta.line_count = static_cast<std::uint16_t>(std::min<std::size_t>(lc, 0xFFFFu));

        prefix_blocks.push_back(
            std::make_shared<const Element>(md_block_to_element(block)));
        prefix_metas.push_back(std::move(meta));
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
    ++source_version_;

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
    ++source_version_;

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

    // Stream is over: drop the blinking cursor so the settled
    // message doesn't keep requesting animation frames forever.
    live_ = false;
    build_dirty_ = true;
}

void StreamingMarkdown::clear() {
    source_.clear();
    committed_ = 0;
    ++source_version_;
    // Replace the prefix snapshot rather than mutating the existing one
    // — any ComponentElement still capturing the old shared_ptr (held
    // inside cached_build_ until that's reassigned below) sees the
    // pre-clear content, and the next build() will allocate a fresh
    // prefix_ + ComponentElement so the renderer's component_cache
    // misses cleanly on the new instance instead of holding stale
    // entries against an unrelated subsequent stream.
    prefix_ = std::make_shared<CommittedPrefix>();
    // Rotate the per-instance discriminator mixed into the prefix
    // ComponentElement's hash_id. Without this rotation the first
    // post-clear commit produces generation=1 and a CacheId built
    // from ("strmd-prefix", instance_id_, 1) — hash-identical to the
    // pre-clear stream's first commit. That pre-clear entry is
    // retained in the renderer's hash-keyed component cache
    // (entries_by_hash, ~2-second wallclock TTL), so the post-clear
    // commit hits it and blits the pre-clear cells at the post-clear
    // region. Bumping instance_id_ on every clear() makes the
    // hash_id strictly monotonic across logical resets.
    instance_id_ = detail::next_component_generation();
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
    tail_inline_cache_version_     = 0;
    // Reset the build-cache shape flags so the next build() falls into
    // the full-rebuild path rather than trying to mutate a stale
    // structural template.
    cached_prefix_gen_    = 0;
    cached_fold_gen_      = 0;
    cached_has_tail_      = false;
    cached_has_prefix_    = false;
    cached_tail_in_fence_ = false;
    cached_tail_hash_     = 0;
    cached_tail_len_      = 0;
    cached_build_         = Element{TextElement{""}};
    cached_live_          = Element{TextElement{""}};
    live_                 = false;

    // Fold map keys at byte offsets in the prior source_; that source
    // is gone, so the keys are meaningless. Bumping fold_generation_
    // is paired with the new prefix_ here so any stub Elements still
    // capturing the previous fold_generation_ via their hash_id miss
    // cleanly on the renderer's component cache.
    folds_.clear();
    ++fold_generation_;

    // Drop any in-flight async parse: its result will be stale once
    // it lands (source it parsed no longer matches us) and
    // maybe_apply_async_ will discard it on the version mismatch.
    // Clearing the latest-source sentinel ensures we don't spawn a
    // follow-up worker for the discarded result.
    {
        std::lock_guard<std::mutex> lk(async_mu_);
        async_slot_.reset();
        async_latest_source_.reset();
    }
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
    // ── Eager list / blockquote rendering for completed rows ──
    //
    // Exception to the rule above: when the tail STARTS with a list
    // marker (`- `, `* `, `+ `, `\d+. `, `\d+) `) or a blockquote
    // marker (`> `), we can render each FULLY-TERMINATED row in its
    // committed shape immediately. Long lists are the worst offender
    // for the perceived "wait for the page" lag: a 12-item bullet list
    // stays in classify_blank_line's intra-blank limbo for the entire
    // duration of the stream, rendered as raw `* item` text in body
    // colour, and snaps to a styled list only when the model emits
    // its first non-list line. Same for blockquotes — `> ` markers
    // sit literal until end-of-quote.
    //
    // Monotonicity argument: each fully-terminated row has a height
    // determined by its bytes alone (text wrap inside its column).
    // The LIVE row (last line, no trailing `\n`) stays literal so
    // further bytes can extend it without snapping shape. Once a `\n`
    // lands on the live row, it joins the rendered rows above on the
    // next frame — a one-row monotonic add, identical in cost to
    // committing one extra line of an inline paragraph.
    //
    // We feed the rows through parse_markdown_impl so the rendered
    // Element is byte-identical to what commit_range would eventually
    // produce — no snap when commit finally fires. The live row falls
    // through to the inline-only path below as a separate Element
    // sitting under the rendered list.
    auto is_list_row_start = [](std::string_view line) noexcept -> bool {
        return ul_marker_len(line) > 0 || ol_marker_len(line) > 0;
    };
    auto is_quote_row_start = [](std::string_view line) noexcept -> bool {
        // Skip up to 3 leading spaces (GFM tolerance), then `> ` or `>`
        // at end-of-line.
        std::size_t k = 0;
        while (k < 4 && k < line.size() && line[k] == ' ') ++k;
        if (k >= line.size() || line[k] != '>') return false;
        // Bare `>` or `> ...` both qualify.
        return k + 1 == line.size() || line[k + 1] == ' ' || line[k + 1] == '\t';
    };

    // Probe the first line of the tail to decide whether to engage the
    // eager renderer. We deliberately don't engage on a tail whose
    // first line is plain prose followed by list rows — the prose
    // already prints inline and treating mid-buffer list rows as a
    // separate list would split the tail into two visually distinct
    // sections that the eventual commit would re-merge differently.
    auto first_line_end = body.find('\n');
    std::string_view first_line = (first_line_end == std::string_view::npos)
        ? body
        : body.substr(0, first_line_end);
    const bool tail_is_list  = is_list_row_start(first_line);
    const bool tail_is_quote = !tail_is_list && is_quote_row_start(first_line);

    if ((tail_is_list || tail_is_quote) && first_line_end != std::string_view::npos) {
        // Walk lines forward, greedily consuming everything that still
        // belongs to the same block (list rows + their continuations,
        // or `> `-prefixed rows). Stop at the first line that breaks
        // the pattern OR at the live row (the trailing partial line
        // with no `\n`).
        std::size_t cursor = 0;
        std::size_t last_committed_end = 0;   // end of last fully-terminated in-block row
        while (cursor < body.size()) {
            auto eol = body.find('\n', cursor);
            if (eol == std::string_view::npos) {
                // Live row — leave for the inline fallback.
                break;
            }
            std::string_view line = body.substr(cursor, eol - cursor);
            bool in_block;
            if (tail_is_list) {
                // A list "row" is either a new item marker OR a
                // continuation (indented or blank, where the blank's
                // intra-list semantics already kept us cohesive).
                if (is_list_row_start(line)) {
                    in_block = true;
                } else if (line.empty()) {
                    // Blank inside the list (loose-list separator).
                    // Keep going — classify_blank_line's verdict at
                    // commit time will decide whether this terminates;
                    // for rendering we just need bytes that
                    // parse_markdown_impl will recognise.
                    in_block = true;
                } else if (count_indent(line) >= 2) {
                    // Indented continuation of the previous item.
                    in_block = true;
                } else {
                    in_block = false;
                }
            } else {
                // Blockquote: lines are `> ...` or `>` (lazy
                // continuations are CommonMark legal but we conservatively
                // require the marker to keep the eager path simple).
                in_block = is_quote_row_start(line);
            }
            if (!in_block) break;
            cursor = eol + 1;
            last_committed_end = cursor;
        }

        if (last_committed_end > 0) {
            // Parse the proven-complete rows as a real markdown
            // document. parse_markdown_impl on a slice ending in `\n`
            // produces a single List or Blockquote block (plus possibly
            // a trailing empty paragraph for the loose blank, which
            // assemble_markdown's vstack handles).
            std::string_view rendered_slice = body.substr(0, last_committed_end);
            ::maya::md_detail::RefDefsScope guard(
                const_cast<std::unordered_map<std::string, md::LinkRef>*>(&ref_defs_));
            auto parsed = parse_markdown_impl(std::string{rendered_slice}, 0);

            std::vector<Element> kids;
            kids.reserve(parsed.blocks.size() + 1);
            for (auto& block : parsed.blocks) {
                kids.push_back(md_block_to_element(block));
            }

            // Live row (post-last-`\n` partial) renders as inline text,
            // same monotonicity rules as the paragraph fallback below.
            // We don't fold it into the parsed list because doing so
            // would change the parsed list's shape on the next frame
            // when the row terminates — the snap we're trying to
            // avoid.
            std::string_view live_row = body.substr(last_committed_end);
            // Trim leading blank lines so a separator between the
            // rendered list and the live row doesn't paint a stray
            // empty row.
            while (!live_row.empty() && live_row.front() == '\n')
                live_row.remove_prefix(1);
            if (!live_row.empty()) {
                auto live_spans = parse_inlines(live_row);
                std::string content;
                std::vector<StyledRun> runs;
                const Style base = Style{}.with_fg(colors::text);
                for (const auto& s : live_spans) {
                    flatten_inline(s, base, content, runs);
                }
                Element live_el;
                if (runs.empty()) {
                    live_el = Element{TextElement{}};
                } else if (runs.size() == 1) {
                    live_el = Element{TextElement{
                        .content = std::move(content),
                        .style   = runs[0].style,
                    }};
                } else {
                    live_el = Element{TextElement{
                        .content = std::move(content),
                        .style   = Style{}.with_fg(colors::text),
                        .runs    = std::move(runs),
                    }};
                }
                kids.push_back(std::move(live_el));
            }

            if (kids.size() == 1) return std::move(kids.front());
            return detail::vstack().gap(1)(std::move(kids)).build();
        }
        // Fell through (no fully-terminated rows yet) — the inline
        // fallback below renders the partial markers as literal text,
        // same as the historical behaviour. Once the first row gets
        // its `\n` we'll take the eager path on the next frame.
    }

    // ── Eager table rendering ─────────────────────────────────────────
    //
    // Tables are the worst offender for "wait for the page to format".
    // Without this branch the user sees raw `|` / `---` text from the
    // moment the model emits the header until either (a) a blank line
    // lands below the table (the boundary scanner commits) or (b) a
    // non-`|` line lands below (find_table_end commits). On a long
    // table that's many seconds of unstyled pipes scrolling past.
    //
    // Eager rule:
    //   * tail starts with `|` (after ≤3 spaces tolerance — GFM)
    //   * the header line is fully terminated by `\n`
    //   * the delimiter line is fully terminated AND well-formed
    //     (only `|`, `-`, `:`, space, `\r`, with at least one `-`)
    //
    // When all three hold we feed [header, delim, body-rows…] through
    // parse_markdown_impl exactly the way commit_range would, then
    // render via md_block_to_element. Any partial trailing row (no
    // closing `\n`) is held back and rendered as inline literal text
    // BELOW the formatted table, same monotonicity trick the eager
    // list path uses.
    //
    // Monotonicity: each fully-terminated body row contributes a
    // fixed number of visual rows to the rendered table (column-
    // wrapped). New `\n`-terminated rows can only ADD rows. The live
    // partial row stays inline below until its `\n` lands, at which
    // point it folds into the table on the next frame (one-row add,
    // identical Element identity for the rows already shown — the
    // table's ComponentElement keys on `data` shared_ptr identity,
    // not on this temporary fresh build, so the inner cells cache
    // misses cleanly on the new row count without ghosting).
    if (body.size() >= 1 && first_line_end != std::string_view::npos) {
        auto first_non_space_off = [](std::string_view s) -> std::size_t {
            std::size_t k = 0;
            while (k < 4 && k < s.size() && s[k] == ' ') ++k;
            return k;
        };
        std::size_t hoff = first_non_space_off(first_line);
        const bool header_pipe = hoff < first_line.size()
                                 && first_line[hoff] == '|';
        if (header_pipe) {
            // Need a fully-terminated delimiter line.
            std::size_t delim_start = first_line_end + 1;
            std::size_t delim_end = body.find('\n', delim_start);
            if (delim_end != std::string_view::npos) {
                std::string_view delim_line =
                    body.substr(delim_start, delim_end - delim_start);
                std::size_t doff = first_non_space_off(delim_line);
                bool delim_pipe = doff < delim_line.size()
                                  && delim_line[doff] == '|';
                bool delim_has_dash = false;
                bool delim_ok = delim_pipe;
                if (delim_ok) {
                    for (std::size_t k = doff;
                         k < delim_line.size(); ++k) {
                        char c = delim_line[k];
                        if (c == '-') { delim_has_dash = true; continue; }
                        if (c == '|' || c == ':' || c == ' '
                            || c == '\t' || c == '\r') continue;
                        delim_ok = false; break;
                    }
                }
                if (delim_ok && delim_has_dash) {
                    // Walk body rows: every fully-terminated `|`-prefixed
                    // line after the delimiter. Stop at the first non-
                    // `|` line OR at the live (unterminated) row.
                    std::size_t cursor = delim_end + 1;
                    std::size_t last_row_end = cursor; // end of header+delim+complete rows
                    while (cursor < body.size()) {
                        std::size_t eol = body.find('\n', cursor);
                        if (eol == std::string_view::npos) break;
                        std::string_view line =
                            body.substr(cursor, eol - cursor);
                        std::size_t off = first_non_space_off(line);
                        if (off >= line.size() || line[off] != '|')
                            break;
                        cursor = eol + 1;
                        last_row_end = cursor;
                    }

                    // Parse the proven-complete slice as a real markdown
                    // document — yields exactly one md::Table block
                    // matching what commit_range would eventually emit.
                    std::string_view table_slice =
                        body.substr(0, last_row_end);
                    ::maya::md_detail::RefDefsScope guard(
                        const_cast<std::unordered_map<
                            std::string, md::LinkRef>*>(&ref_defs_));
                    auto parsed = parse_markdown_impl(
                        std::string{table_slice}, 0);

                    std::vector<Element> kids;
                    kids.reserve(parsed.blocks.size() + 1);
                    for (auto& block : parsed.blocks) {
                        kids.push_back(md_block_to_element(block));
                    }

                    // Live trailing row (partial, no `\n`). Render as
                    // inline literal text so further bytes extend it
                    // without snapping the table's shape.
                    std::string_view live_row =
                        body.substr(last_row_end);
                    while (!live_row.empty() && live_row.front() == '\n')
                        live_row.remove_prefix(1);
                    if (!live_row.empty()) {
                        auto live_spans = parse_inlines(live_row);
                        std::string content;
                        std::vector<StyledRun> runs;
                        const Style base =
                            Style{}.with_fg(colors::text);
                        for (const auto& s : live_spans) {
                            flatten_inline(s, base, content, runs);
                        }
                        Element live_el;
                        if (runs.empty()) {
                            live_el = Element{TextElement{}};
                        } else if (runs.size() == 1) {
                            live_el = Element{TextElement{
                                .content = std::move(content),
                                .style   = runs[0].style,
                            }};
                        } else {
                            live_el = Element{TextElement{
                                .content = std::move(content),
                                .style   = Style{}.with_fg(colors::text),
                                .runs    = std::move(runs),
                            }};
                        }
                        kids.push_back(std::move(live_el));
                    }

                    if (kids.size() == 1) return std::move(kids.front());
                    return detail::vstack().gap(1)(std::move(kids)).build();
                }
            }
        }
        // Header+delim not both ready yet → fall through to inline
        // path; the literal `|` text stays visible until the next
        // frame lands more bytes and we can try the eager path again.
    }

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
    // Fast path: source_version_ matches what populated the cache —
    // bytes are guaranteed unchanged, no hash needed.  Slow path:
    // version moved, fall back to the (hash, length) key to catch the
    // case where commit_range shifted the tail base but the stable-
    // prefix slice happened to round-trip to the same bytes.
    if (tail_inline_cache_version_ != source_version_) {
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
        tail_inline_cache_version_ = source_version_;
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
    // Live-mode wrapper helper. When live_ is false this is a single
    // ref-return; when true it rebuilds cached_live_ as a vstack of
    // [cached_build_, blinking_cursor_row] and requests an animation
    // frame so the next paint advances the blink phase. Defined as a
    // lambda over `this` so each return-site in build() collapses to
    // `return finalize();`.
    auto finalize = [this]() -> const Element& {
        if (!live_) return cached_build_;

        // ── Geeky-as-fuck live animation ──
        //
        // Three layered effects on the trailing edge of the live tail:
        //
        //   (1) SCRAMBLE → RESOLVE
        //       Newly-arrived chars start as random ASCII/box glyphs
        //       and "decrypt" into the real char over ~180ms. Drives
        //       a strong "text materialising in real time" feel.
        //
        //   (2) HOT → COOL GRADIENT TRAIL
        //       The last ~24 codepoints follow a per-char color
        //       gradient indexed by age:
        //           freshest  →  hot magenta + bold
        //           young     →  bright cyan
        //           settling  →  ice blue, dim
        //           locked    →  base fg
        //       Each char migrates leftward through the gradient as
        //       new ones arrive on the right.
        //
        //   (3) PULSING BLOCK CARET with TRAILING TRACER
        //       Wide block (▊) cycling magenta→cyan, plus a thin
        //       trailing dim half-block to the right that fades over
        //       300ms — makes the caret feel like it's leaving a
        //       trail as it moves forward.
        //
        // All three driven by request_animation_frame so they
        // continue ticking at ~60 fps. Pure visual layer: cached_build_
        // is not touched; every frame builds cached_live_ fresh from
        // a shallow copy + mutated tail-leaf TextElement.

        const auto now = std::chrono::steady_clock::now();
        const std::int64_t ms_total = std::chrono::duration_cast<
            std::chrono::milliseconds>(now.time_since_epoch()).count();

        // Update grow-time stamp: if source has grown since last
        // build(), record this frame's wall time as the moment the
        // trailing edge moved. last_seen_size_ tracks the size we
        // observed last frame.
        if (source_.size() > last_seen_size_) {
            last_grow_ms_   = ms_total;
            last_seen_size_ = source_.size();
        } else if (source_.size() < last_seen_size_) {
            // Source shrank (clear / set_content rollback) — reset.
            last_seen_size_ = source_.size();
            last_grow_ms_   = ms_total;
        }
        const std::int64_t age_at_tail_ms = ms_total - last_grow_ms_;

        // Walk a copy of cached_build_ down its rightmost spine to
        // the last leaf TextElement.
        auto find_last_text = [](Element& root) -> TextElement* {
            Element* cur = &root;
            for (;;) {
                if (auto* t = std::get_if<TextElement>(&cur->inner)) {
                    return t->content.empty() ? nullptr : t;
                }
                if (auto* b = std::get_if<BoxElement>(&cur->inner)) {
                    if (b->children.empty()) return nullptr;
                    cur = &b->children.back();
                    continue;
                }
                return nullptr;
            }
        };

        // Step back N UTF-8 codepoints from end of `s`.
        auto utf8_step_back = [](std::string_view s,
                                 std::size_t n) -> std::size_t {
            std::size_t i = s.size();
            while (n > 0 && i > 0) {
                --i;
                while (i > 0 &&
                       (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) {
                    --i;
                }
                --n;
            }
            return i;
        };

        // Tunables.
        constexpr std::size_t kTrailLen    = 24;   // codepoints in gradient
        constexpr std::size_t kScrambleLen = 4;    // codepoints scrambled
        constexpr std::int64_t kScrambleMs = 180;  // scramble → resolve
        constexpr std::int64_t kCharStepMs = 28;   // per-char age delta

        // Effective age for codepoint at position `i_from_tail`
        // (0 = newest, increasing leftward). Newest char's age is
        // the actual wall-clock since source grew; each earlier char
        // is kCharStepMs older. Clamped so even the oldest in the
        // trail has a non-trivial age — prevents the whole trail
        // from snapping color when bytes arrive in a burst.
        auto age_for = [&](std::size_t i_from_tail) -> std::int64_t {
            return age_at_tail_ms +
                static_cast<std::int64_t>(i_from_tail) * kCharStepMs;
        };

        // Trail color: lerp through a 4-stop palette by age.
        //   age 0       → hot magenta (255, 90, 200) + bold
        //   age 120ms   → bright cyan (120, 230, 255) + bold
        //   age 320ms   → ice blue    (140, 180, 220) dim
        //   age 700ms+  → base fg (no override)
        // Returns nullopt for ages past the trail — caller leaves
        // those chars' original style alone.
        auto trail_style = [&](std::int64_t age_ms)
            -> std::optional<Style>
        {
            if (age_ms >= 700) return std::nullopt;
            // Lerp helpers.
            auto lerp = [](double a, double b, double t) {
                return a + (b - a) * t;
            };
            double r, g, b;
            bool bold = false;
            bool dim  = false;
            if (age_ms < 120) {
                const double t = age_ms / 120.0;
                r = lerp(255, 120, t);
                g = lerp( 90, 230, t);
                b = lerp(200, 255, t);
                bold = true;
            } else if (age_ms < 320) {
                const double t = (age_ms - 120) / 200.0;
                r = lerp(120, 140, t);
                g = lerp(230, 180, t);
                b = lerp(255, 220, t);
                bold = t < 0.5;
            } else {
                // 320 → 700: fade from ice blue to invisible-on-base
                // by reducing saturation. We approximate by lerping
                // toward a neutral light gray.
                const double t = (age_ms - 320) / 380.0;
                r = lerp(140, 200, t);
                g = lerp(180, 200, t);
                b = lerp(220, 200, t);
                dim = true;
            }
            Style s = Style{}.with_fg(Color::rgb(
                static_cast<std::uint8_t>(r),
                static_cast<std::uint8_t>(g),
                static_cast<std::uint8_t>(b)));
            if (bold) s = s.with_bold();
            if (dim)  s = s.with_dim();
            return s;
        };

        // Scramble glyphs: a curated set of ASCII / box-drawing /
        // dingbat chars that read as "digital noise". We pick one
        // deterministically from (i_from_tail, ms_total) so a given
        // char's scramble glyph CHANGES across frames — the
        // characteristic Matrix "churning" effect.
        static constexpr const char* kScrambleGlyphs[] = {
            "#", "$", "%", "&", "*", "+", "=", "?", "@",
            "!", "~", "^", "<", ">", "|", "/", "\\",
            "\xe2\x96\x91",  // ░
            "\xe2\x96\x92",  // ▒
            "\xe2\x96\x93",  // ▓
            "\xe2\x96\xa0",  // ■
            "\xe2\x96\xa1",  // □
            "\xe2\x97\x86",  // ◆
            "\xe2\x97\x87",  // ◇
            "\xe2\x97\x8f",  // ●
            "\xe2\x97\x8b",  // ○
            "\xe2\x9c\xa6",  // ✦
            "\xe2\x9c\xa8",  // ✨ (sparkle — occasional accent)
            "\xce\xb1", "\xce\xb2", "\xce\xb3", "\xce\xb4",  // αβγδ
            "\xce\xbb", "\xcf\x80", "\xcf\x83", "\xcf\x86",  // λπσφ
            "0", "1", "7", "8", "X", "Z",
        };
        constexpr std::size_t kScrambleN =
            sizeof(kScrambleGlyphs) / sizeof(kScrambleGlyphs[0]);

        // Cheap deterministic hash for picking a scramble glyph.
        auto scramble_pick = [&](std::size_t cp_idx,
                                 std::int64_t age_ms) -> std::string_view {
            // Quantise time so we don't change glyph EVERY frame —
            // we churn at ~45ms intervals so individual frames don't
            // look like static. (Faster = noisier, slower = sleepier.)
            const std::uint64_t time_bucket =
                static_cast<std::uint64_t>(ms_total / 45);
            std::uint64_t h = 0x9e3779b97f4a7c15ull;
            h ^= cp_idx + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= time_bucket + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= static_cast<std::uint64_t>(age_ms) + 0x9e3779b9
                 + (h << 6) + (h >> 2);
            return std::string_view{kScrambleGlyphs[h % kScrambleN]};
        };

        // Shallow copy of cached_build_; we'll splice a freshly-built
        // tail TextElement onto its rightmost leaf.
        //
        // Note: this copies the outer vstack BoxElement and its
        // children vector. Children are:
        //   [prefix_component, tail_text]  (or one of those alone)
        // The prefix is a ComponentElement — its copy is O(1) (one
        // std::function copy + shared_ptr refcount bump). The actual
        // committed-prefix content lives inside the lambda's captured
        // shared_ptr<CommittedPrefix>; copying the ComponentElement
        // does NOT touch the prefix block trees, and the renderer's
        // component_cache (keyed on hash_id) keeps the painted cells
        // warm across frames. So this copy is O(1) regardless of
        // transcript length — long sessions stay cheap.
        //
        // The tail TextElement copy DOES copy its content std::string
        // which is up to one paragraph / code block of bytes. We
        // mitigate below by mutating only the trailing kTrailLen
        // codepoints in place rather than rebuilding the whole string.
        Element animated_body = cached_build_;
        if (TextElement* tail = find_last_text(animated_body)) {
            const std::string_view orig = tail->content;

            // Find the trail window WITHOUT scanning the entire
            // content. Walk backward from the end byte-by-byte until
            // we've passed kTrailLen codepoints (a UTF-8 codepoint
            // start is any byte whose top 2 bits are not 10). Then
            // index codepoints only within [trail_byte_start, end).
            // O(kTrailLen) per frame, independent of tail length.
            const std::size_t trail_byte_start =
                utf8_step_back(orig, kTrailLen);
            const std::string_view trail_slice =
                orig.substr(trail_byte_start);

            // Codepoint boundaries WITHIN the trail slice only.
            std::vector<std::size_t> trail_cp_offs;
            trail_cp_offs.reserve(kTrailLen + 1);
            for (std::size_t i = 0; i < trail_slice.size();) {
                trail_cp_offs.push_back(i);
                ++i;
                while (i < trail_slice.size() &&
                       (static_cast<unsigned char>(trail_slice[i]) & 0xC0) == 0x80)
                    ++i;
            }
            trail_cp_offs.push_back(trail_slice.size());
            const std::size_t trail_n =
                trail_cp_offs.empty() ? 0 : (trail_cp_offs.size() - 1);

            if (trail_n == 0) {
                // Empty trail (shouldn't happen given we checked
                // tail non-empty above, but defensive). Skip
                // mutation; caret row below still paints.
                goto trail_done;
            }

            // Scramble window is the rightmost min(kScrambleLen,
            // trail_n) codepoints.
            const std::size_t scramble_n =
                std::min(trail_n, kScrambleLen);
            const std::size_t scramble_cp_start_local =
                trail_n - scramble_n;

            // Build the new content in-place: prefix bytes are
            // unchanged (we just reference them via the existing
            // string), then we splice trail bytes that may have a
            // scramble glyph substituted for the real cp.
            //
            // Strategy: reserve a tail-sized buffer (small — bounded
            // by trail_slice.size() + ~kScrambleLen * worst-case-utf8
            // width). Append-build the trail. Then assign:
            //   tail->content = prefix_bytes + new_trail_bytes
            //
            // To avoid a full-content copy we use std::string's
            // resize+memcpy idiom: keep the original prefix by
            // truncating to trail_byte_start, then append the new
            // trail.
            std::string new_trail;
            new_trail.reserve(trail_slice.size() + scramble_n * 3);

            // Carry-forward original runs that fall before the trail.
            std::vector<StyledRun> new_runs;
            new_runs.reserve(
                (tail->runs.empty() ? 1 : tail->runs.size())
                + trail_n + 2);

            if (trail_byte_start > 0) {
                if (tail->runs.empty()) {
                    new_runs.push_back(StyledRun{
                        .byte_offset = 0,
                        .byte_length = trail_byte_start,
                        .style       = tail->style,
                    });
                } else {
                    for (const auto& r : tail->runs) {
                        if (r.byte_offset >= trail_byte_start) break;
                        const std::size_t end =
                            r.byte_offset + r.byte_length;
                        const std::size_t clipped =
                            std::min(end, trail_byte_start);
                        new_runs.push_back(StyledRun{
                            .byte_offset = r.byte_offset,
                            .byte_length = clipped - r.byte_offset,
                            .style       = r.style,
                        });
                        if (end > trail_byte_start) break;
                    }
                }
            }

            // Walk the trail codepoint-by-codepoint, emitting either
            // the real char (with a trail color overlay) or a scramble
            // glyph.
            for (std::size_t k = 0; k < trail_n; ++k) {
                const std::size_t i_from_tail = trail_n - 1 - k;
                const std::int64_t age = age_for(i_from_tail);

                std::string_view real_cp{
                    trail_slice.data() + trail_cp_offs[k],
                    trail_cp_offs[k + 1] - trail_cp_offs[k]};

                const bool in_scramble_window =
                    k >= scramble_cp_start_local;
                const bool scrambling =
                    in_scramble_window && age < kScrambleMs;

                std::string_view emitted;
                std::string scramble_owned;
                if (scrambling) {
                    scramble_owned = std::string{
                        scramble_pick(trail_byte_start + trail_cp_offs[k],
                                      age)};
                    emitted = scramble_owned;
                } else {
                    emitted = real_cp;
                }

                const std::size_t out_byte_off =
                    trail_byte_start + new_trail.size();
                new_trail.append(emitted.data(), emitted.size());
                const std::size_t out_byte_len =
                    (trail_byte_start + new_trail.size()) - out_byte_off;

                // Pick the style.
                Style s;
                if (scrambling) {
                    const bool flick =
                        ((ms_total / 60 + k) & 1) == 0;
                    s = Style{}
                        .with_fg(flick
                            ? Color::rgb(255,  80, 180)
                            : Color::rgb(255, 160,  60))
                        .with_bold();
                } else if (auto ts = trail_style(age)) {
                    s = *ts;
                } else {
                    s = tail->style;
                }
                new_runs.push_back(StyledRun{
                    .byte_offset = out_byte_off,
                    .byte_length = out_byte_len,
                    .style       = s,
                });
            }

            // Truncate to the prefix and append the rebuilt trail.
            // resize(trail_byte_start) does NOT reallocate; the
            // existing capacity is preserved.
            tail->content.resize(trail_byte_start);
            tail->content.append(new_trail);
            tail->runs = std::move(new_runs);
            // Force re-wrap on the copy ONLY when scramble is active.
            // Scramble may substitute a 1-byte ASCII for a 3-byte box
            // glyph, changing content byte length and invalidating the
            // wrap cache's content_size key. When no scramble is
            // active (age_at_tail >= kScrambleMs across the entire
            // window) the trail bytes are byte-identical to the
            // original tail — the wrap cache from the source render is
            // still valid and re-wrapping would be wasted O(content)
            // work on every animation frame. On a multi-KB code-block
            // tail this avoidance is the difference between smooth
            // and chokes-the-renderer.
            const bool scramble_active =
                age_at_tail_ms < kScrambleMs +
                static_cast<std::int64_t>(scramble_n) * kCharStepMs;
            if (scramble_active) {
                tail->cached_width = -1;
            }
        }
        trail_done:;

        // ── Caret row with trailing tracer ──
        //
        // Main caret: wide block, color pulses magenta→cyan via a
        // triangular wave on ~650ms period.
        // Trailing tracer: a dim half-block to the right of the
        // caret that's only visible during the "forward stroke" of
        // the pulse (gives the impression the caret just moved).
        constexpr std::int64_t kCaretPeriodMs = 650;
        const double caret_phase =
            static_cast<double>(ms_total % kCaretPeriodMs)
            / static_cast<double>(kCaretPeriodMs);
        const double tri = (caret_phase < 0.5)
            ? (caret_phase * 2.0)
            : (2.0 - caret_phase * 2.0);
        // Caret: lerp magenta (220, 80, 200) → cyan (100, 230, 255).
        const std::uint8_t cr = static_cast<std::uint8_t>(
            220.0 + (100.0 - 220.0) * tri);
        const std::uint8_t cg = static_cast<std::uint8_t>(
             80.0 + (230.0 -  80.0) * tri);
        const std::uint8_t cb = static_cast<std::uint8_t>(
            200.0 + (255.0 - 200.0) * tri);
        // Tracer brightness follows the caret's forward stroke
        // (first half of the cycle) and fades on the return.
        const double tracer_intensity = (caret_phase < 0.5)
            ? (1.0 - caret_phase * 2.0)   // 1 → 0 as phase 0 → 0.5
            : 0.0;
        const std::uint8_t tr = static_cast<std::uint8_t>(
            60.0 + (140.0 - 60.0) * tracer_intensity);
        const std::uint8_t tg = static_cast<std::uint8_t>(
            60.0 + (160.0 - 60.0) * tracer_intensity);
        const std::uint8_t tb = static_cast<std::uint8_t>(
            80.0 + (200.0 - 80.0) * tracer_intensity);
        Element caret_row = (
            detail::hstack().padding(0, 0, 0, 2)(
                Element{TextElement{
                    .content = std::string{"\xe2\x96\x88"}, // █ full block
                    .style   = Style{}
                        .with_fg(Color::rgb(cr, cg, cb))
                        .with_bold(),
                }},
                Element{TextElement{
                    .content = std::string{"\xe2\x96\x8c"}, // ▌ half block
                    .style   = Style{}
                        .with_fg(Color::rgb(tr, tg, tb))
                        .with_dim(),
                }}
            )
        ).build();

        cached_live_ = (
            detail::vstack()(
                std::move(animated_body),
                std::move(caret_row)
            )
        ).build();

        ::maya::request_animation_frame();
        return cached_live_;
    };

    // Poll any landed async parse — may mutate prefix_/source_ and
    // flip build_dirty_, so it must run BEFORE the early-out.
    maybe_apply_async_();

    // Untouched-since-last-build: return cached. Dominant case when the
    // widget is idle (no streaming).
    if (!build_dirty_) return finalize();

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
        cached_tail_version_ = source_version_;
        build_dirty_       = false;
        return finalize();
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
    // commit-fast-path below. Hoisted into a lambda so the hash_id
    // construction and lambda-capture wiring don't drift between the
    // two call sites (a previous bug where they did caused the prefix
    // ComponentElement to skip its cache invalidation on commit and
    // ghost the previous frame's prefix bitmap into the new prefix's
    // layout slot).
    auto make_prefix_component = [&]() -> Element {
        auto p = prefix_;
        // Snapshot the fold map so the render lambda's behaviour is
        // stable per ComponentElement instance — unrelated edits
        // (typing into the composer) that don't change the prefix
        // won't reshape this component, but a fold toggle bumps
        // fold_generation_ which changes hash_id and forces a
        // re-render with the updated map.
        std::map<std::size_t, bool> fold_snapshot = folds_;
        ComponentElement comp;
        comp.hash_id = CacheIdBuilder{}
            .add(std::string_view{"strmd-prefix"})
            .add(static_cast<std::uint64_t>(instance_id_))
            .add(static_cast<std::uint64_t>(p->generation))
            .add(static_cast<std::uint64_t>(fold_generation_))
            .build();
        comp.render = [p, folds = std::move(fold_snapshot)](int /*w*/, int /*h*/) -> Element {
            std::vector<Element> kids;
            kids.reserve(p->blocks.size());
            for (std::size_t i = 0; i < p->blocks.size(); ++i) {
                const auto& meta = (i < p->metas.size())
                    ? p->metas[i] : BlockMeta{};
                auto fit = folds.find(meta.source_offset);
                const bool folded = fit != folds.end() && fit->second;
                if (folded) {
                    // Render a single-line stub. Use a styled
                    // dim summary derived from BlockMeta so a
                    // long code block collapses to one row
                    // labelled with its language and line
                    // count.
                    const char* kind_label = "block";
                    switch (meta.kind) {
                        case BlockKind::CodeBlock:  kind_label = "code";       break;
                        case BlockKind::Table:      kind_label = "table";      break;
                        case BlockKind::List:       kind_label = "list";       break;
                        case BlockKind::Blockquote: kind_label = "blockquote"; break;
                        case BlockKind::Paragraph:  kind_label = "paragraph";  break;
                        case BlockKind::Heading:    kind_label = "heading";    break;
                        case BlockKind::HtmlBlock:  kind_label = "html";       break;
                        default: break;
                    }
                    char buf[160];
                    if (!meta.lang.empty()) {
                        std::snprintf(buf, sizeof(buf),
                            "\xe2\x96\xb8 %u line%s of %s (%s) hidden \xe2\x80\x94 unfold",
                            static_cast<unsigned>(meta.line_count),
                            meta.line_count == 1 ? "" : "s",
                            kind_label, meta.lang.c_str());
                    } else {
                        std::snprintf(buf, sizeof(buf),
                            "\xe2\x96\xb8 %u line%s of %s hidden \xe2\x80\x94 unfold",
                            static_cast<unsigned>(meta.line_count),
                            meta.line_count == 1 ? "" : "s",
                            kind_label);
                    }
                    kids.push_back(Element{TextElement{
                        .content = std::string{buf},
                        .style   = Style{}.with_fg(colors::strike_fg).with_dim(),
                    }});
                } else {
                    kids.push_back(*p->blocks[i]);
                }
            }
            return detail::vstack().gap(1)(std::move(kids)).build();
        };
        return Element{std::move(comp)};
    };

    if (cached_prefix_gen_ == prefix_->generation
        && cached_fold_gen_  == fold_generation_
        && cached_has_prefix_ == has_prefix
        && cached_has_tail_   == has_tail
        && std::holds_alternative<BoxElement>(cached_build_.inner)) {
        auto& box = std::get<BoxElement>(cached_build_.inner);
        const std::size_t prefix_n =
            has_prefix ? prefix_->blocks.size() : 0u;
        const std::size_t expected = prefix_n + (has_tail ? 1u : 0u);
        if (box.children.size() == expected) {
            if (has_tail) {
                // Fast path: source_version_ matches what produced the
                // current cached tail — the tail bytes are provably
                // byte-identical to last frame (every mutator that
                // touches source_/committed_ also bumps the version).
                // O(1) compare, no hash walk.
                //
                // Slow path: version moved, fall back to the (fence,
                // length, FNV-1a) digest. This catches the rare case
                // where commit_range fired but the tail happened to
                // land at the same bytes — lets us still skip
                // render_tail when the body round-tripped.
                bool tail_unchanged;
                std::uint64_t tail_hash = 0;
                if (cached_tail_version_ == source_version_) {
                    tail_unchanged = true;
                } else {
                    tail_hash = fnv1a64(tail);
                    tail_unchanged =
                        cached_tail_in_fence_ == in_code_fence_
                        && cached_tail_len_   == tail.size()
                        && cached_tail_hash_  == tail_hash;
                }
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
                cached_tail_version_ = source_version_;
            }
            cached_tail_size_ = tail.size();
            build_dirty_      = false;
            return finalize();
        }
    }

    // ── Commit fast path ──
    //
    // Generation moved (commit_range fired), but the outer vstack's
    // shape (has_prefix / has_tail) is unchanged. We can mutate the
    // existing BoxElement in place: swap children[0] (the prefix
    // ComponentElement) for a fresh one carrying the new generation in
    // its hash_id, and refresh the tail child if needed. The outer
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
    if ((cached_prefix_gen_ != prefix_->generation
         || cached_fold_gen_  != fold_generation_)
        && cached_has_prefix_ == has_prefix && has_prefix
        && cached_has_tail_   == has_tail
        && std::holds_alternative<BoxElement>(cached_build_.inner)) {
        auto& box = std::get<BoxElement>(cached_build_.inner);
        const std::size_t expected = 1u + (has_tail ? 1u : 0u);
        if (box.children.size() == expected) {
            box.children[0] = make_prefix_component();
            if (has_tail) {
                bool tail_unchanged;
                std::uint64_t tail_hash = 0;
                if (cached_tail_version_ == source_version_) {
                    tail_unchanged = true;
                } else {
                    tail_hash = fnv1a64(tail);
                    tail_unchanged =
                        cached_tail_in_fence_ == in_code_fence_
                        && cached_tail_len_   == tail.size()
                        && cached_tail_hash_  == tail_hash;
                }
                if (!tail_unchanged) {
                    box.children.back() = render_tail(tail);
                    cached_tail_hash_     = tail_hash;
                    cached_tail_len_      = tail.size();
                    cached_tail_in_fence_ = in_code_fence_;
                }
                cached_tail_version_ = source_version_;
            }
            cached_prefix_gen_ = prefix_->generation;
            cached_fold_gen_   = fold_generation_;
            cached_tail_size_  = tail.size();
            build_dirty_       = false;
            return finalize();
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
    // ComponentElement per block whose hash_id was derived from sp
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
        // generation-derived hash_id. The renderer's component_cache
        // then stores the rendered cells against this key; between
        // commits (generation stable) every render hits the cache and
        // blits cells directly, skipping the deep-copy lambda below
        // entirely. On commit the generation bumps, the hash_id
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
    cached_fold_gen_      = fold_generation_;
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
    cached_tail_version_ = source_version_;
    build_dirty_          = false;
    return finalize();
}

// ── Block introspection / folding ────────────────────────────────────

std::optional<std::size_t>
StreamingMarkdown::block_for_offset(std::size_t off) const noexcept {
    const auto& metas = prefix_->metas;
    if (metas.empty()) return std::nullopt;
    // Binary search for the greatest i with metas[i].source_offset <= off.
    auto it = std::upper_bound(metas.begin(), metas.end(), off,
        [](std::size_t v, const BlockMeta& m) noexcept {
            return v < m.source_offset;
        });
    if (it == metas.begin()) return std::nullopt;
    --it;
    const std::size_t idx = static_cast<std::size_t>(it - metas.begin());
    // off must lie within the block's attributed range. Beyond
    // source_end (e.g. into the tail) returns nullopt.
    if (off >= it->source_end) return std::nullopt;
    return idx;
}

bool StreamingMarkdown::is_folded(std::size_t source_offset) const noexcept {
    auto it = folds_.find(source_offset);
    return it != folds_.end() && it->second;
}

void StreamingMarkdown::set_fold(std::size_t source_offset, bool folded) {
    // Validate against the current block set so we don't accumulate
    // map entries for stale offsets (an offset that doesn't match any
    // known block has no rendering meaning).
    bool known = false;
    for (const auto& m : prefix_->metas) {
        if (m.source_offset == source_offset) { known = true; break; }
    }
    if (!known) return;
    bool& slot = folds_[source_offset];
    if (slot == folded) return;
    slot = folded;
    ++fold_generation_;
    build_dirty_ = true;
}

bool StreamingMarkdown::toggle_fold(std::size_t source_offset) {
    const bool now = !is_folded(source_offset);
    set_fold(source_offset, now);
    // set_fold no-ops on an unknown offset; reflect the actual state.
    return is_folded(source_offset);
}

void StreamingMarkdown::unfold_all() {
    if (folds_.empty()) return;
    folds_.clear();
    ++fold_generation_;
    build_dirty_ = true;
}

void StreamingMarkdown::auto_fold_long_blocks(std::uint16_t threshold_lines,
                                              std::uint32_t kinds_mask) {
    bool any = false;
    for (const auto& m : prefix_->metas) {
        const std::uint32_t bit = 1u << static_cast<unsigned>(m.kind);
        if (!(bit & kinds_mask)) continue;
        if (m.line_count <= threshold_lines) continue;
        // Only fold blocks the user hasn't already explicitly
        // unfolded — if they typed the unfold key, respect it. A
        // missing entry (the dominant case on the first auto-fold
        // pass) gets the auto-fold; an entry set to `false` is the
        // user's explicit "keep open" and stays unfolded.
        if (folds_.contains(m.source_offset)) continue;
        folds_[m.source_offset] = true;
        any = true;
    }
    if (any) {
        ++fold_generation_;
        build_dirty_ = true;
    }
}

// ── Async parse ──────────────────────────────────────────────────────

bool StreamingMarkdown::is_parsing() const noexcept {
    // No need to lock for a coarse "in flight?" probe — the slot
    // pointer is set under the mutex and read here without; the
    // worker only flips `ready` and then the foreground destroys
    // the slot. A torn read of the shared_ptr is benign (returns
    // false negative for one frame at worst).
    return static_cast<bool>(async_slot_);
}

void StreamingMarkdown::set_content_async(std::string_view content) {
    // Same fast-path as set_content: no-op on unchanged.
    if (content.size() == source_.size() &&
        (content.empty() || std::memcmp(content.data(), source_.data(),
                                        content.size()) == 0)) {
        return;
    }
    // Pure-append growth → cheap incremental sync path is strictly
    // better than handing the delta off to a worker (the steady-state
    // streaming case). The expensive case async exists for is the
    // "divergent prefix" / large initial set path below.
    if (content.size() > source_.size() &&
        (source_.empty() || std::memcmp(content.data(), source_.data(),
                                        source_.size()) == 0)) {
        set_content(content);
        return;
    }

    // Below a threshold the synchronous reset-and-parse path is faster
    // than thread handoff; pay the cost inline. 16 KB is a generous
    // ceiling — pasting a long markdown blob comfortably exceeds it
    // while typical short composer edits stay below.
    constexpr std::size_t kAsyncThreshold = 16 * 1024;
    if (content.size() < kAsyncThreshold) {
        set_content(content);
        return;
    }

    // Divergent and large → schedule a background parse. Keep the
    // current cached_build_ visible until the worker lands so the UI
    // doesn't blank.
    std::string requested{content};
    {
        std::lock_guard<std::mutex> lk(async_mu_);
        async_latest_source_ = requested;
        // If a worker is already in flight, just update the latest
        // request — maybe_apply_async_ will spawn a follow-up on
        // arrival of the in-flight result. This is the Zed-style
        // single-flight coalescer.
        if (async_slot_) return;
    }
    spawn_async_worker_(std::move(requested));
}

void StreamingMarkdown::spawn_async_worker_(std::string source) const {
    auto slot = std::make_shared<AsyncResult>();
    slot->source = source;
    {
        std::lock_guard<std::mutex> lk(async_mu_);
        async_slot_ = slot;
    }

    // Detached worker: result lifetime is tied to the shared_ptr in
    // the slot, which the foreground holds the only other copy of.
    // If the StreamingMarkdown is destroyed while a worker is alive,
    // the foreground's slot copy is dropped on destruction; the
    // worker writes into its own slot copy (still alive), and the
    // result is then silently discarded when the worker's local
    // shared_ptr falls out of scope.
    std::thread([slot, src = std::move(source)]() mutable {
        // Re-parse from scratch. We can't reuse the host's incremental
        // state because that lives on the foreground thread; instead
        // we run the full top-level parse on the worker. Output is
        // the rendered Element list + per-block metadata + the
        // ref-defs map a fresh parse produces.
        std::unordered_map<std::string, md::LinkRef> defs;
        std::string cleaned = collect_ref_defs(std::string_view{src}, defs);
        ::maya::md_detail::RefDefsScope guard(&defs);
        auto parsed = parse_markdown_impl(cleaned, 0);

        // Compute segment ranges over the full src — same algorithm
        // as the foreground commit_range above, just bounded by the
        // whole buffer.
        std::vector<std::pair<std::size_t, std::size_t>> seg_ranges;
        seg_ranges.reserve(parsed.blocks.size() + 1);
        {
            bool seg_in_fence = false;
            std::size_t k = 0;
            while (k < src.size() && src[k] == '\n') ++k;
            std::size_t seg_start = k;
            while (k < src.size()) {
                bool at_ls = (k == 0 || src[k - 1] == '\n');
                if (at_ls && k + 3 <= src.size() &&
                    ((src[k] == '`' && src[k+1] == '`' && src[k+2] == '`') ||
                     (src[k] == '~' && src[k+1] == '~' && src[k+2] == '~'))) {
                    seg_in_fence = !seg_in_fence;
                    std::size_t eol = src.find('\n', k);
                    if (eol == std::string::npos) { k = src.size(); break; }
                    k = eol + 1;
                    if (!seg_in_fence) {
                        if (k > seg_start) seg_ranges.emplace_back(seg_start, k);
                        while (k < src.size() && src[k] == '\n') ++k;
                        seg_start = k;
                    }
                    continue;
                }
                if (!seg_in_fence && at_ls && src[k] == '\n') {
                    if (k > seg_start) seg_ranges.emplace_back(seg_start, k);
                    while (k < src.size() && src[k] == '\n') ++k;
                    seg_start = k;
                    continue;
                }
                ++k;
            }
            if (seg_start < src.size())
                seg_ranges.emplace_back(seg_start, src.size());
        }
        const bool seg_match = seg_ranges.size() == parsed.blocks.size();

        slot->blocks.reserve(parsed.blocks.size());
        slot->metas.reserve (parsed.blocks.size());
        std::size_t synth = 0;
        for (std::size_t bi = 0; bi < parsed.blocks.size(); ++bi) {
            auto& block = parsed.blocks[bi];
            BlockMeta meta;
            if (seg_match) {
                meta.source_offset = seg_ranges[bi].first;
                meta.source_end    = seg_ranges[bi].second;
            } else {
                meta.source_offset = synth;
                meta.source_end = (bi + 1 < parsed.blocks.size())
                    ? (synth + 1) : src.size();
                synth += 1;
            }
            meta.kind = std::visit([](const auto& x) noexcept -> BlockKind {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, md::Paragraph>)       return BlockKind::Paragraph;
                else if constexpr (std::is_same_v<T, md::Heading>)    return BlockKind::Heading;
                else if constexpr (std::is_same_v<T, md::CodeBlock>)  return BlockKind::CodeBlock;
                else if constexpr (std::is_same_v<T, md::Blockquote>) return BlockKind::Blockquote;
                else if constexpr (std::is_same_v<T, md::List>)       return BlockKind::List;
                else if constexpr (std::is_same_v<T, md::HRule>)      return BlockKind::HRule;
                else if constexpr (std::is_same_v<T, md::Table>)      return BlockKind::Table;
                else if constexpr (std::is_same_v<T, md::FootnoteDef>) return BlockKind::FootnoteDef;
                else if constexpr (std::is_same_v<T, md::Alert>)      return BlockKind::Alert;
                else if constexpr (std::is_same_v<T, md::DefList>)    return BlockKind::DefList;
                else if constexpr (std::is_same_v<T, md::Details>)    return BlockKind::Details;
                else if constexpr (std::is_same_v<T, md::HtmlBlock>)  return BlockKind::HtmlBlock;
                else return BlockKind::Other;
            }, block.inner);
            if (auto* c = std::get_if<md::CodeBlock>(&block.inner)) meta.lang = c->lang;
            std::size_t lc = 0;
            for (std::size_t q = meta.source_offset; q < meta.source_end && q < src.size(); ++q)
                if (src[q] == '\n') ++lc;
            meta.line_count = static_cast<std::uint16_t>(std::min<std::size_t>(lc, 0xFFFFu));
            slot->blocks.push_back(
                std::make_shared<const Element>(md_block_to_element(block)));
            slot->metas.push_back(std::move(meta));
        }
        slot->ref_defs = std::move(defs);
        // in_code_fence at end-of-source — count ``` / ~~~ fence
        // toggles at line starts.
        bool fence = false;
        for (std::size_t j = 0; j < src.size(); ++j) {
            bool at_ls = (j == 0 || src[j - 1] == '\n');
            if (!at_ls) continue;
            if (j + 3 <= src.size() &&
                ((src[j] == '`' && src[j+1] == '`' && src[j+2] == '`') ||
                 (src[j] == '~' && src[j+1] == '~' && src[j+2] == '~')))
                fence = !fence;
        }
        slot->in_code_fence = fence;
        // Publish: release-store on `ready` so the foreground's
        // acquire-load sees the populated vectors above.
        slot->ready.store(true, std::memory_order_release);
    }).detach();
}

void StreamingMarkdown::maybe_apply_async_() const {
    std::shared_ptr<AsyncResult> slot;
    std::optional<std::string>   latest_after;
    {
        std::lock_guard<std::mutex> lk(async_mu_);
        if (!async_slot_) return;
        if (!async_slot_->ready.load(std::memory_order_acquire)) return;
        slot = std::move(async_slot_);  // detach from member
        latest_after = async_latest_source_;
    }

    // Decide what to do with the landed result:
    //   A. The caller's latest request matches what this worker
    //      parsed → adopt the result; clear latest_source.
    //   B. The caller has since asked for a different source →
    //      discard this result, spawn a follow-up on the new
    //      request.
    const bool current = latest_after && *latest_after == slot->source;
    if (current) {
        // Adopt. This is the foreground thread (build() calls us),
        // so mutating self's state is safe with no extra locks.
        auto self = const_cast<StreamingMarkdown*>(this);
        self->source_ = slot->source;
        self->committed_ = slot->source.size();
        self->in_code_fence_ = slot->in_code_fence;
        self->ref_defs_ = std::move(slot->ref_defs);
        self->sink_.reset();
        auto fresh = std::make_shared<CommittedPrefix>();
        fresh->blocks = std::move(slot->blocks);
        fresh->metas  = std::move(slot->metas);
        // Generation must STRICTLY exceed the prior gen so the
        // prefix ComponentElement's hash_id changes and the
        // renderer's cache misses cleanly.
        fresh->generation = self->prefix_->generation + 1;
        self->prefix_ = std::move(fresh);
        self->build_dirty_ = true;
        ++self->source_version_;
        // Reset scanner / sink state to the new committed end.
        self->scan_cursor_ = self->committed_;
        self->scan_in_fence_ = self->in_code_fence_;
        self->scan_last_boundary_ = self->committed_;
        // Drop fold map entries that no longer correspond to any
        // block in the new prefix (offsets shifted under us).
        std::vector<std::size_t> drop;
        for (auto& kv : self->folds_) {
            bool known = false;
            for (const auto& m : self->prefix_->metas)
                if (m.source_offset == kv.first) { known = true; break; }
            if (!known) drop.push_back(kv.first);
        }
        for (auto k : drop) self->folds_.erase(k);
        {
            std::lock_guard<std::mutex> lk(self->async_mu_);
            self->async_latest_source_.reset();
        }
    } else if (latest_after) {
        // Stale: spawn a follow-up for the most recent request.
        spawn_async_worker_(std::move(*latest_after));
    }
}

// ── Cross-TU bridge ──────────────────────────────────────────────────
namespace md_detail {
Element assemble_markdown(md::Document&& doc) {
    return ::maya::assemble_markdown(std::move(doc));
}
} // namespace md_detail

} // namespace maya
