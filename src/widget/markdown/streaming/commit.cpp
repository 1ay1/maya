// commit.cpp — StreamingMarkdown commit + input plumbing.
//
// commit_range parses [committed_, boundary) into finalized prefix blocks,
// derives per-block source ranges for the fold/lookup APIs, and advances
// the fence-state tracker. append_safe / set_content / append / finish /
// clear are the input entry points that drive it (all funnel codepoint-safe
// bytes through the StreamSink first).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "maya/element/builder.hpp"
#include "maya/widget/markdown.hpp"
#include "maya/widget/markdown/internal.hpp"

namespace maya {

using ::maya::md_detail::parse_markdown_impl;
using ::maya::md_detail::collect_ref_defs;

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
    // Local helper: is the line starting at `pos` a $$-only math
    // fence opener? Same semantics as the boundary scanner's gate —
    // `$$` followed only by spaces/tabs/\r up to '\n'. The two
    // walkers MUST agree on this predicate; if commit_range here
    // ever treats a $$-followed-by-text line as a fence while the
    // scanner doesn't (or vice versa) the seg-vs-block attribution
    // drifts.
    auto is_math_fence_line = [&](size_t pos, size_t end) -> bool {
        if (pos + 2 > end) return false;
        if (source_[pos] != '$' || source_[pos+1] != '$') return false;
        size_t eol = std::string_view{source_}.find('\n', pos);
        if (eol == std::string_view::npos || eol >= end) return false;
        for (size_t q = pos + 2; q < eol; ++q) {
            char cc = source_[q];
            if (cc != ' ' && cc != '\t' && cc != '\r') return false;
        }
        return true;
    };
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
            bool is_code_open = at_ls && k + 3 <= boundary &&
                ((source_[k] == '`' && source_[k+1] == '`' && source_[k+2] == '`') ||
                 (source_[k] == '~' && source_[k+1] == '~' && source_[k+2] == '~'));
            bool is_math_open = !is_code_open && at_ls &&
                                is_math_fence_line(k, boundary);
            if (is_code_open || is_math_open) {
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
    // NO exact-size reserve here. commit_range runs once per block boundary
    // for the whole turn, appending a handful of blocks each time; a
    // reserve(size + new) fills capacity exactly, so the NEXT commit's
    // reserve always exceeds capacity and reallocates — moving every
    // committed block/meta, O(N) per commit, O(N²) per long turn (the same
    // exact-reserve trap fixed in build()'s pure-growth arm). push_back's
    // geometric doubling keeps these appends amortised O(1).

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
        // Math: $$-alone-on-line (gated identically to the boundary
        // scanner and the seg walker above). Treating `$$foo` as a
        // fence here would flip in_code_fence_ without the scanner
        // agreeing — a divergence that buries subsequent paragraphs
        // in an opaque fence forever.
        bool is_math = !is_code && is_math_fence_line(j, boundary);
        if (is_code || is_math) in_code_fence_ = !in_code_fence_;
    }

    committed_ = boundary;
    build_dirty_ = true;
    ++source_version_;

    // Prune consumed entries from the pending-boundary ledger — anything
    // ≤ the new committed_ has been committed (this call may have jumped
    // past several intermediate boundaries, e.g. finish()).
    if (!scan_boundaries_.empty()) {
        std::size_t keep = 0;
        while (keep < scan_boundaries_.size()
               && scan_boundaries_[keep] <= committed_)
            ++keep;
        if (keep > 0)
            scan_boundaries_.erase(scan_boundaries_.begin(),
                                   scan_boundaries_.begin()
                                       + static_cast<std::ptrdiff_t>(keep));
    }

    // Keep scanner state coherent with the new committed_.
    //
    // Resyncing scan_in_fence_ to in_code_fence_ is valid ONLY when the
    // commit caught up to (or passed) the resumable scanner —
    // committed_ >= scan_cursor_. Then both describe the SAME byte
    // position, so the committed-prefix fence parity (in_code_fence_) is
    // also the parity at scan_cursor_. This covers finish() and every
    // eager-commit branch (closing-fence / table / heading / hrule),
    // which all set last_boundary == the scan position.
    //
    // When committed_ < scan_cursor_ the scanner has walked AHEAD into
    // the still-uncommitted tail — e.g. past an opening ``` whose closing
    // fence hasn't arrived yet (a chunk ended mid-code-block).
    // find_block_boundary already advanced scan_in_fence_ to the fence
    // parity AT scan_cursor_ as it walked there (true — inside the open
    // fence). in_code_fence_ is only the parity at the EARLIER committed_
    // (false — the opening fence is still in the tail), so overwriting
    // scan_in_fence_ with it DROPS the open-fence state. On the next call
    // the closing ``` is then read as an OPENING fence and every
    // following paragraph/heading/list is buried in one code block: the
    // chunk-boundary-dependent "prose rendered as code" streaming
    // corruption. Leave the scanner's own state intact in that case.
    scan_last_boundary_ = committed_;
    if (committed_ >= scan_cursor_) {
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

    // ── Commit every COMPLETED block; keep only the last block live ──────
    //
    // A markdown block is finished the instant the next block begins — i.e.
    // once a blank-line (or fence-close) boundary exists after it. Only the
    // trailing, still-growing block is "live"; everything above it is done
    // and will never change. So commit the whole completed prefix and leave
    // exactly the in-progress tail uncommitted. That keeps the live tail
    // bounded to ONE block no matter how long the turn is, so render_tail
    // and the live overlay are O(one block) per frame, never O(turn) — the
    // root of the long-reply streaming lag.
    //
    // find_block_boundary() returns the end of the last completed block, so
    // committing to it leaves precisely the in-progress block as the tail.
    //
    // Why this does NOT burst chrome ahead of the typewriter reveal: the
    // reveal animation (scramble / gradient / caret) decorates only the
    // LIVE TAIL. A block only becomes committable once the NEXT block has
    // started — by then the cursor has long since swept past it (the
    // typewriter reveals the tail as bytes arrive; a block that's now
    // "prefix" was the tail many frames ago and was revealed then). So
    // committing it changes nothing visible: it was already shown at full
    // text; commit just moves its (immutable) Element into the frozen
    // prefix the renderer blits from cache. The earlier reveal-cursor
    // gating was solving a non-problem and cost O(turn) per frame.
    //
    // PRISTINE-FIRST-FRAME: the host feeds the first delta, THEN calls
    // set_live(true). On that very first pre-live frame, hold the opening
    // block in the tail so it reveals through the typewriter from the start
    // instead of committing un-paced (the stream-start burst). Once live,
    // the rule above takes over.
    size_t boundary = find_block_boundary();
    if (boundary > committed_) {
        const bool pristine_reveal_start =
            reveal_fx_ && !live_ && committed_ == 0;
        if (pristine_reveal_start) return;
        // ── Reveal-paced commit ──
        //
        // While the animated reveal is live, commit only up to the largest
        // discovered boundary the typewriter cursor has already swept —
        // never bytes it hasn't revealed. Committed blocks render in full
        // immediately (the prefix is un-ghosted, un-clipped) and
        // advance_reveal_cursor_ snaps the cursor forward past
        // committed_cp, so an early commit TELEPORTS ~drain_secs worth of
        // text (the cursor's design lag) onto the screen in one frame.
        // That was the per-block-type burst the smoothness probe measured:
        // prose glided at ~2 content-cells/frame while code fences (+196),
        // tables (+103) and lists (+191) popped whole at their commit
        // boundary — a structured block's boundary is discovered the
        // instant it completes, when the cursor is still mid-block.
        //
        // Deferring is VISUALLY FREE: render_tail's canonical path renders
        // terminated tail bytes in their committed shape (byte-identical
        // cells — the monotonicity funnel's guarantee), with build()'s
        // line-granular clip + the eager bottom-row glide pacing rows out
        // exactly like prose. And it is BOUNDED: commit_revealed_ commits
        // each pending ledger boundary as the cursor crosses it (also
        // polled from build() between deltas), so the uncommitted tail
        // stays ≈ one block + the cursor's lag window — never O(turn).
        // finish() flushes everything at end-of-stream; the snap paths
        // (snap_reveal_to_edge + finish) move the cursor to the edge
        // first, so the gate opens by construction.
        if (reveal_fx_ && live_) {
            commit_revealed_();
            return;
        }
        commit_range(boundary);
    }
}

void StreamingMarkdown::commit_revealed_() {
    if (!(reveal_fx_ && live_)) return;
    if (scan_boundaries_.empty()) return;
    const std::size_t cursor_byte =
        byte_offset_for_cp(static_cast<std::size_t>(
            reveal_cp_ < 0.0 ? 0.0 : reveal_cp_));
    // Largest pending boundary the cursor has fully swept.
    std::size_t target = 0;
    for (std::size_t b : scan_boundaries_) {
        if (b <= cursor_byte && b > committed_) target = b;
        if (b > cursor_byte) break;
    }
    if (target > committed_) commit_range(target);
}

std::size_t StreamingMarkdown::byte_offset_for_cp(
    std::size_t n_cp) const noexcept
{
    // Exact-match short-circuit: the same (n_cp, source size) asked twice
    // (cursor advance, then the in-frame consumer) returns the cached byte
    // with zero walking.
    if (cp_to_byte_cache_at_ == source_.size()
        && cp_to_byte_cache_cp_ == n_cp) {
        return cp_to_byte_cache_byte_;
    }

    // Forward-resumable walk — flat per-frame cost regardless of turn
    // length. The reveal cursor advances MONOTONICALLY (reveal_cp_ only
    // grows, except on clear()/replace which reset the cache below), and
    // during a live stream source_ is APPEND-ONLY — so bytes
    // [0, cp_to_byte_cache_byte_) are provably unchanged and the cached
    // (cp, byte) pair is still a valid waypoint. Resume the count from
    // there instead of restarting at byte 0, so each frame walks only the
    // ~2 codepoints the cursor advanced (O(Δcp)), not the whole buffer.
    //
    // Without this the clip recompute in advance_reveal_cursor_ ran a
    // branchy UTF-8 scan from 0 to the cursor on EVERY animation frame
    // (the exact-match cache misses every frame because both n_cp and
    // source size move), so a long reply's per-frame animation cost grew
    // O(source_size) with its length — streaming visibly slowed/lagged as
    // the turn got longer. O(Δcp)/frame keeps it constant.
    //
    // The waypoint is trusted only when it can't have been invalidated:
    // the cache was taken at a size <= the current size (source grew or
    // held → the prefix is unchanged) and the request is at/ahead of the
    // waypoint cp. Any backward request or apparent shrink falls back to a
    // cold walk from 0 — correct, just not amortised.
    std::size_t bytes_seen = 0;
    std::size_t cps_seen   = 0;
    if (n_cp >= cp_to_byte_cache_cp_
        && cp_to_byte_cache_at_  <= source_.size()
        && cp_to_byte_cache_byte_ <= source_.size()) {
        bytes_seen = cp_to_byte_cache_byte_;
        cps_seen   = cp_to_byte_cache_cp_;
    }
    while (bytes_seen < source_.size() && cps_seen < n_cp) {
        ++bytes_seen;
        while (bytes_seen < source_.size()
               && (static_cast<unsigned char>(source_[bytes_seen]) & 0xC0)
                      == 0x80)
        {
            ++bytes_seen;
        }
        ++cps_seen;
    }
    cp_to_byte_cache_cp_   = n_cp;
    cp_to_byte_cache_byte_ = bytes_seen;
    cp_to_byte_cache_at_   = source_.size();
    return bytes_seen;
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
    // The Tracked<> wrapper on live_ auto-bumps build_dirty_.
    live_ = false;
    // A hard finish means there is no finalize RAMP left to glide — the
    // widget is at its terminal state NOW. finalize_deadline_ms_ is the
    // sole backing of is_finalizing(); if a ramp was armed (request_finalize)
    // but finish() lands before the line-430 reveal-tick gate clears it, the
    // deadline stays non-zero and is_finalizing() reports true forever. Any
    // host that re-arms a frame on is_finalizing() (agentty's turn.cpp does)
    // then spins ~20 fps while idle. Clearing it here — alongside live_ —
    // makes finish() the true terminal transition it advertises.
    finalize_deadline_ms_ = 0;
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
    scan_boundaries_.clear();
    // Reset the inline-tail cache. Strictly speaking the prefix-match
    // check at render_tail entry is self-correcting (any new tail will
    // mismatch and re-parse), but clearing here avoids carrying KB of
    // stale string state past a logical reset.
    tail_inline_cache_prefix_hash_ = 0;
    tail_inline_cache_prefix_len_  = 0;
    tail_inline_cache_content_.clear();
    tail_inline_cache_runs_.clear();
    tail_inline_cache_version_     = 0;
    eager_cache_version_   = 0;
    eager_cache_slice_hash_ = 0;
    eager_cache_slice_len_ = 0;
    eager_cache_blocks_.clear();
    tail_canon_cache_version_  = 0;
    tail_canon_cache_hash_     = 0;
    tail_canon_cache_len_      = 0;
    tail_canon_cache_in_fence_ = false;
    tail_canon_cache_el_.reset();
    tail_term_cache_version_   = 0;
    tail_term_cache_hash_      = 0;
    tail_term_cache_len_       = 0;
    tail_term_cache_in_fence_  = false;
    tail_term_cache_blocks_.clear();
    tail_term_cache_abs_end_   = static_cast<std::size_t>(-1);
    tail_stable_cache_version_  = 0;
    tail_stable_cache_len_      = 0;
    tail_stable_cache_in_fence_ = false;
    tail_stable_cache_blocks_.clear();
    tail_stable_cache_abs_end_  = static_cast<std::size_t>(-1);
    tail_wrap_cache_version_     = 0;
    tail_wrap_cache_len_         = 0;
    tail_wrap_cache_in_fence_    = false;
    tail_wrap_cache_block_count_ = 0;
    tail_wrap_cache_blocks_.clear();
    tail_wrap_cache_abs_end_     = static_cast<std::size_t>(-1);
    tail_merge_cache_last_blk_abs_ = static_cast<std::size_t>(-1);
    tail_merge_cache_term_abs_end_ = static_cast<std::size_t>(-1);
    tail_merge_cache_live_hash_    = 0;
    tail_merge_cache_in_fence_     = false;
    tail_merge_cache_continues_    = false;
    tail_merge_cache_el_.reset();
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
    cached_tail_clip_     = 0;
    cached_total_cp_         = 0;
    cached_total_cp_at_      = 0;
    cached_committed_cp_     = 0;
    cached_committed_cp_at_  = 0;
    reveal_byte_clip_        = static_cast<std::size_t>(-1);
    cp_to_byte_cache_cp_     = 0;
    cp_to_byte_cache_byte_   = 0;
    cp_to_byte_cache_at_     = 0;
    cached_build_         = Element{TextElement{""}};
    cached_live_          = Element{TextElement{""}};
    live_overlay_prefix_gen_ = static_cast<std::uint64_t>(-1);
    overlay_sig_valid_    = false;
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
        std::lock_guard<std::mutex> lk(async_mu_());
        async_slot_.reset();
        async_latest_source_.reset();
    }
}

} // namespace maya
