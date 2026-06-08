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

    // Keep scanner state coherent with the new committed_. The
    // earlier conditional resync only fired when commit_range jumped
    // past scan_cursor_ (the finish() path); for the normal
    // append_safe path scan_in_fence_ was left untouched on the
    // assumption it already equalled in_code_fence_. That invariant
    // is fragile: any future eager-commit branch that advances
    // last_boundary past lines without re-evaluating fence parity
    // can let the two drift. Unconditional resync removes the
    // dependency on that invariant — cheap (two stores) and makes
    // the post-commit state a proven function of in_code_fence_.
    scan_last_boundary_ = committed_;
    if (scan_cursor_ < committed_) scan_cursor_ = committed_;
    scan_in_fence_ = in_code_fence_;
}

// Internal: append codepoint-clean bytes that have already passed through
// the StreamSink.  Public entry points all funnel here.
void StreamingMarkdown::append_safe(std::string_view safe_bytes) {
    if (safe_bytes.empty()) return;
    source_.append(safe_bytes.data(), safe_bytes.size());
    build_dirty_ = true;
    ++source_version_;

    size_t boundary = find_block_boundary();
    if (boundary > committed_) {
        // Gate eager block commit on the reveal cursor when reveal_fx_
        // is on and live_. commit_range fires the parser which produces
        // styled prefix Elements (H2 underline, code-fence chrome,
        // table rules) painted at full chrome the moment they land.
        // The reveal overlay only animates the live tail; committed
        // prefix bytes BYPASS the typewriter — once a block commits,
        // its chrome appears in one frame and the user sees a burst.
        //
        // While reveal_fx_ && live_, DEFER ALL commits. The whole
        // stream stays in the tail; render_tail's eager-heading /
        // eager-list / eager-table paths still render styled inline
        // text WITHOUT the block-level underline/border chrome that
        // commit produces, and the overlay's ghost band materialises
        // each codepoint as the cursor walks. finish() flushes any
        // remaining tail through commit_range, so the final settled
        // shape (with full chrome) appears at end-of-stream when the
        // host expects a layout snap anyway. Off-path for hosts that
        // didn't opt into reveal_fx.
        //
        // PRISTINE-FIRST-FRAME extension: the host turns the widget
        // live AFTER the first set_content (turn.cpp feeds bytes, then
        // calls set_live(true)), and the reveal clip only initialises
        // on the first build()/advance_reveal pass. So on the very
        // first stream frame live_ is still false and clip is -1 — the
        // strict gate below let that first delta's complete blocks
        // commit un-paced, dumping the opening block (70+ rows) in one
        // frame: the stream-start burst. Also defer while the widget is
        // PRISTINE (nothing committed yet) and reveal_fx is on, so the
        // opening block stays in the paced tail until the host marks it
        // live and the cursor takes over. Once live_ is true the gate
        // is identical to before — commits resume at block boundaries,
        // so steady-state reveal shape (and its height monotonicity) is
        // unchanged.
        const bool pristine_reveal_start =
            reveal_fx_ && !live_ && committed_ == 0;
        const bool defer =
            pristine_reveal_start
            || (reveal_fx_ && live_
                && reveal_byte_clip_ != static_cast<std::size_t>(-1));
        if (!defer) {
            commit_range(boundary);
        }
    }
}

std::size_t StreamingMarkdown::byte_offset_for_cp(
    std::size_t n_cp) const noexcept
{
    // Cached: the last (n_cp, byte) pair walked. Cursor advance + the
    // append_safe gate in the same frame ask for the SAME n_cp twice
    // — a single source_.size() check + equal-cp short-circuit avoids
    // the second walk.
    if (cp_to_byte_cache_at_ == source_.size()
        && cp_to_byte_cache_cp_ == n_cp) {
        return cp_to_byte_cache_byte_;
    }
    // Walk from 0. Counting UTF-8 lead bytes (high bits != 10).
    // Steady-state cursor walks codepoints monotonically; the cache
    // amortises but a full walk is still O(source_size) on a cold
    // call. Acceptable: called at most twice per frame, and the
    // typewriter rate (~120 cps) means the cursor crosses ~2 cp per
    // frame at 60 fps — source_ rarely exceeds tens of KB during a
    // live stream.
    std::size_t bytes_seen = 0;
    std::size_t cps_seen   = 0;
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
