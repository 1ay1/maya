// render_tail.cpp — StreamingMarkdown's opaque-tail renderer.
//
// Renders the uncommitted bytes since the last block boundary as a
// monotonic in-progress element: inside-fence literal, open-fence code
// block, ATX heading, eager list/blockquote/table for fully-terminated
// rows, and an inline-only sliding-cache paragraph fallback for the live
// line. The monotonic-height invariant lives here — see the body.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "maya/element/builder.hpp"
#include "maya/element/text.hpp"        // string_width
#include "maya/render/cache_id.hpp"
#include "maya/style/border.hpp"
#include "maya/style/style.hpp"
#include "maya/widget/markdown.hpp"
#include "maya/widget/markdown/internal.hpp"

#include "maya/widget/markdown/streaming_internal.hpp"

namespace maya {

using ::maya::md_detail::parse_markdown_impl;
using ::maya::md_detail::parse_inlines;
using ::maya::md_detail::flatten_inline;
using ::maya::md_detail::build_inline_row;
using ::maya::md_detail::highlight_code;
using ::maya::md_detail::ul_marker_len;
using ::maya::md_detail::ol_marker_len;
using ::maya::md_detail::count_indent;
using ::maya::md_detail::streaming::fnv1a64;

void StreamingMarkdown::render_eager_slice(std::string_view slice,
                                           std::vector<Element>& kids) const {
    // Memoize on (source_version_, slice length, slice hash). The slice
    // is the proven-complete prefix of the eager block; it only grows
    // when a new row terminates, so between row-commits this is a hit
    // and we re-emit the cached block Elements by shared_ptr copy —
    // skipping parse_markdown_impl + md_block_to_element entirely.
    //
    // Each cached block is also wrapped in a hash-keyed ComponentElement
    // (key = slice hash + block index) so the RENDERER's cell cache
    // blits it (memcpy/row) instead of re-laying-out the whole table or
    // list every frame. Without this the parse is cached but render_tree
    // still re-runs layout::compute over the full block each frame — the
    // dominant cost for a big streaming table. The key changes only when
    // a new row lands (slice hash moves), so a growing block re-lays-out
    // once per committed row, not once per frame.
    std::uint64_t slice_hash = eager_cache_slice_hash_;
    bool hit = false;
    if (eager_cache_version_ == source_version_
        && eager_cache_slice_len_ == slice.size()
        && !eager_cache_blocks_.empty()) {
        hit = true;
    } else {
        const std::uint64_t h = fnv1a64(slice);
        slice_hash = h;
        if (eager_cache_slice_len_ == slice.size()
            && eager_cache_slice_hash_ == h
            && !eager_cache_blocks_.empty()) {
            hit = true;
        } else {
            ::maya::md_detail::RefDefsScope guard(
                const_cast<std::unordered_map<std::string, md::LinkRef>*>(&ref_defs_));
            auto parsed = parse_markdown_impl(std::string{slice}, 0);
            eager_cache_blocks_.clear();
            eager_cache_blocks_.reserve(parsed.blocks.size());
            for (auto& block : parsed.blocks) {
                // Reserve final column widths for a live table so it does
                // not reflow as later rows reveal (no-op for non-tables).
                apply_live_table_floor_(block);
                eager_cache_blocks_.push_back(
                    std::make_shared<const Element>(md_block_to_element(block)));
            }
            eager_cache_slice_hash_ = h;
            eager_cache_slice_len_  = slice.size();
            eager_cache_version_    = source_version_;
        }
    }
    (void)hit;
    for (std::size_t i = 0; i < eager_cache_blocks_.size(); ++i) {
        auto blk = eager_cache_blocks_[i];
        ComponentElement comp;
        comp.hash_id = ::maya::CacheIdBuilder{}
            .add(std::string_view{"strmd-eager"})
            .add(static_cast<std::uint64_t>(instance_id_))
            .add(slice_hash)
            .add(static_cast<std::uint64_t>(i))
            .build();
        comp.render = [blk](int, int) -> Element { return *blk; };
        kids.push_back(Element{std::move(comp)});
    }
}

// ── Live-table column-width floor ───────────────────────────────────────────
//
// See markdown.hpp (live_table_floor_). When the uncommitted tail begins with
// a GFM table, scan source_ over EVERY already-arrived, fully-terminated row
// of that table and record each column's final width. The eager / canonical
// table renders fold it in (md::Table::min_col_widths) so the visible table
// sits at its FINAL widths from the first revealed row instead of reflowing
// horizontally as wider rows/cells stream in. Cached on (source_version_,
// committed_): recomputed only when bytes arrive or the commit base moves.
void StreamingMarkdown::refresh_live_table_floor_() const {
    if (live_table_floor_version_ == source_version_
        && live_table_floor_base_ == committed_)
        return;
    live_table_floor_version_ = source_version_;
    live_table_floor_base_    = committed_;
    live_table_floor_.clear();

    // The floor only matters when the reveal cursor can hide arrived rows
    // (reveal_fx live). Otherwise the displayed tail already holds every
    // arrived row, so its natural ideal == the floor — computing it would be
    // a pure-overhead parse. Skip.
    if (!(reveal_fx_ && live_)) return;

    // Escape hatch for A/B measurement / emergencies: AGENTTY_NO_TABLE_FLOOR
    // disables the reservation (table reverts to per-revealed-row widths).
    static const bool disabled =
        [] { const char* e = std::getenv("AGENTTY_NO_TABLE_FLOOR");
             return e && e[0] && e[0] != '0'; }();
    if (disabled) return;

    // Start of the live tail's first non-blank line.
    std::size_t base = committed_;
    while (base < source_.size()
           && (source_[base] == '\n' || source_[base] == '\r'))
        ++base;
    if (base >= source_.size()) return;

    // line [ls, le): does it start with '|' after ≤3 leading spaces (GFM)?
    auto first_pipe = [&](std::size_t ls, std::size_t le) -> bool {
        std::size_t k = ls, lim = std::min(ls + 4, le);
        while (k < lim && source_[k] == ' ') ++k;
        return k < le && source_[k] == '|';
    };

    // Header line must be terminated and pipe-led.
    std::size_t he = source_.find('\n', base);
    if (he == std::string::npos || !first_pipe(base, he)) return;
    // Delimiter line must be terminated, pipe-led, and well-formed.
    std::size_t ds = he + 1;
    std::size_t de = source_.find('\n', ds);
    if (de == std::string::npos || !first_pipe(ds, de)) return;
    {
        bool has_dash = false, ok = true;
        std::size_t k = ds, lim = std::min(ds + 4, de);
        while (k < lim && source_[k] == ' ') ++k;
        for (; k < de; ++k) {
            char c = source_[k];
            if (c == '-') { has_dash = true; continue; }
            if (c == '|' || c == ':' || c == ' ' || c == '\t' || c == '\r')
                continue;
            ok = false; break;
        }
        if (!ok || !has_dash) return;
    }

    // Walk arrived data rows: each fully-terminated '|'-led line after the
    // delimiter is in the table. Stop at the first non-pipe line OR the
    // first UNTERMINATED (live) line — the half-typed live row must NOT
    // drive the floor (its growing cell would re-introduce the very jitter
    // we remove; it reflows by at most itself when it finally terminates).
    std::size_t end = de + 1;
    for (std::size_t p = de + 1; p < source_.size();) {
        std::size_t e = source_.find('\n', p);
        if (e == std::string::npos || !first_pipe(p, e)) break;
        p = e + 1;
        end = p;
    }

    // Parse [base, end) — header + delimiter + all terminated rows — and
    // take each column's max flattened width, EXACTLY as the renderer
    // computes ideal (same flatten_inline + string_width), so the floor
    // equals the committed table's natural width: continuous at the seam.
    std::string_view arrived =
        std::string_view{source_}.substr(base, end - base);
    ::maya::md_detail::RefDefsScope guard(
        const_cast<std::unordered_map<std::string, md::LinkRef>*>(&ref_defs_));
    auto blocks = parse_markdown_impl(std::string{arrived}, 0).blocks;
    if (blocks.empty()) return;
    auto* t = std::get_if<md::Table>(&blocks.front().inner);
    if (!t) return;
    const int ncols = static_cast<int>(t->header.cells.size());
    if (ncols == 0) return;
    auto cell_w = [](const std::vector<md::Inline>& spans) -> int {
        std::string content;
        std::vector<StyledRun> runs;
        const Style base_st{};
        for (const auto& sp : spans)
            flatten_inline(sp, base_st, content, runs);
        return string_width(content);
    };
    std::vector<int> w(static_cast<std::size_t>(ncols), 0);
    for (int c = 0; c < ncols; ++c)
        w[static_cast<std::size_t>(c)] =
            std::max(1, cell_w(t->header.cells[static_cast<std::size_t>(c)].spans));
    for (auto& row : t->rows)
        for (int c = 0; c < ncols
                 && static_cast<std::size_t>(c) < row.cells.size(); ++c)
            w[static_cast<std::size_t>(c)] = std::max(
                w[static_cast<std::size_t>(c)],
                cell_w(row.cells[static_cast<std::size_t>(c)].spans));
    live_table_floor_ = std::move(w);
}

void StreamingMarkdown::apply_live_table_floor_(md::Block& block) const {
    if (live_table_floor_.empty()) return;
    if (auto* t = std::get_if<md::Table>(&block.inner))
        if (t->min_col_widths.empty())
            t->min_col_widths = live_table_floor_;
}

Element StreamingMarkdown::render_tail(std::string_view tail) const {
    // Keep the live-table column-width floor current for THIS frame before
    // any table render below reads it (cheap: cached on version+committed).
    refresh_live_table_floor_();

    // ── Canonical-render memo (the anti-"stuck" guard) ─────────────────
    // The body below full-parses the tail every call; build() calls this
    // every animation frame. Memoize on (source_version_, len, hash,
    // in_code_fence_) so a tail that hasn't changed since the last frame
    // is returned by shared_ptr copy with zero parsing. See header.
    //
    // CRITICAL — skip the hash when source_version_ matches. fnv1a64(tail)
    // is O(tail) and used to run EVERY frame just to TEST the memo, so a
    // long live tail (e.g. reveal_fx defers commits, leaving the whole
    // turn in the tail) paid O(N) per animation frame even on a frame
    // where nothing changed — the long-turn streaming stutter. Every
    // mutator of source_/committed_ bumps source_version_, so an unchanged
    // version PROVES the tail bytes are identical: short-circuit on
    // (version, len, in_fence) alone and never touch the bytes. The inner
    // term/eager caches below already use this same version gate; the
    // outer memo was the one O(N) hold-out. The hash is still computed (and
    // cached) on the cold path so a version-bumped-but-identical-bytes case
    // (rare: a mutation that round-trips the tail) still hits by hash.
    const bool version_match =
        tail_canon_cache_el_
        && tail_canon_cache_version_  == source_version_
        && tail_canon_cache_len_      == tail.size()
        && tail_canon_cache_in_fence_ == in_code_fence_;
    if (version_match) return *tail_canon_cache_el_;

    const std::uint64_t tail_hash = fnv1a64(tail);
    if (tail_canon_cache_el_
        && tail_canon_cache_len_      == tail.size()
        && tail_canon_cache_hash_     == tail_hash
        && tail_canon_cache_in_fence_ == in_code_fence_) {
        // Bytes are identical despite a version bump — refresh the version
        // stamp so subsequent frames take the cheap version_match path.
        tail_canon_cache_version_ = source_version_;
        return *tail_canon_cache_el_;
    }

    Element result = [&]() -> Element {
    // The 100% monotonicity funnel.
    //
    // render_tail_inner picks an eager/inline shape for the in-progress
    // bytes. That shape can DISAGREE with the canonical block layout the
    // bytes will commit to, and the disagreement shows up as a height
    // snap — either mid-stream (a reclassification: quote→alert,
    // paragraph→setext, blank-terminated quote collapse) or at the
    // COMMIT SEAM (the eager tail is taller than the canonical block,
    // so when commit_range moves the bytes into the prefix the total
    // height drops by the difference). A padding floor (min_height)
    // can't fix it: the host sizes the inline frame by content_height,
    // which ignores trailing blank rows.
    //
    // The fix that holds across BOTH the mid-stream snap and the commit
    // seam: render the fully-terminated portion of the tail in its
    // CANONICAL committed shape — exactly what commit_range will emit
    // (parse_markdown_impl + md_block_to_element). The live partial last
    // line (no trailing `\n`) can still misparse as a block, so it stays
    // on the eager/inline path; canonical handles only the proven-
    // complete prefix. Because the canonical render IS the committed
    // shape, height at the seam is continuous, and because canonical is
    // a stable function of terminated bytes it never shrinks as more
    // bytes arrive.
    //
    // NOTE: we do NOT early-return for in_code_fence_. When the tail
    // starts inside an open fence, render_tail_inner renders the literal
    // body — but if the CLOSING fence has arrived in the tail (it sits
    // there until the trailing blank line commits the whole block), the
    // canonical parse below balances the fence into a proper CodeBlock
    // whose height matches what commit_range will produce, eliminating
    // the 2-row collapse at the closing-fence commit seam. If the fence
    // is still open (no closing ```), the canonical parse of the
    // terminated rows yields the same in-progress CodeBlock shape.
    std::size_t s = 0;
    while (s < tail.size() && tail[s] == '\n') ++s;
    if (s >= tail.size()) return render_tail_inner(tail);
    std::string_view body = tail.substr(s);

    // Split the tail at the last newline: [terminated_prefix][live_line].
    // Only the terminated prefix is safe to render canonically — the
    // live line may still be a half-typed marker that parse_markdown_impl
    // would misclassify (and then re-classify next byte, snapping).
    auto last_nl = body.rfind('\n');
    if (last_nl == std::string_view::npos) {
        // No terminated rows yet — nothing canonical to anchor; the eager
        // inline render is the floor.
        return render_tail_inner(tail);
    }
    std::string_view terminated = body.substr(0, last_nl + 1);
    std::string_view live_line   = body.substr(last_nl + 1);

    // When the committed prefix ended INSIDE an open code fence, the
    // opening ``` lives in the already-committed bytes, so a fresh parse
    // of the tail wouldn't know it's code. Re-open the fence with a
    // synthetic opener so parse_markdown_impl reconstructs the CodeBlock
    // (and balances it if the closing ``` is present in the tail). The
    // opener is stripped from the rendered output by md_block_to_element
    // exactly as a real fence opener would be.
    std::string fence_prefix;
    if (in_code_fence_) fence_prefix = "```\n";

    // Canonical parse of a slice — byte-identical to what commit_range
    // will eventually produce for these bytes.
    auto canonical_blocks = [this](std::string_view src) {
        ::maya::md_detail::RefDefsScope guard(
            const_cast<std::unordered_map<std::string, md::LinkRef>*>(&ref_defs_));
        return parse_markdown_impl(std::string{src}, 0).blocks;
    };

    // Memoized RENDER of the terminated prefix to shared block Elements,
    // each wrapped in a hash-keyed ComponentElement so the RENDERER's
    // cell cache blits it (memcpy/row) instead of re-laying-out +
    // re-highlighting the whole block every frame. Two layers of caching
    // are needed: this memo skips the PARSE + md_block_to_element when the
    // terminated prefix is unchanged, and the ComponentElement key (stable
    // per terminated-hash + block index) makes render_tree blit the cells
    // rather than recompute layout::compute over the block. Without the
    // wrapper the parse is cached but the per-frame render_tree still
    // re-highlights a growing code fence — O(tail) per frame, the actual
    // "animation stops past N bytes" stall (the parse was never the
    // dominant cost; the layout/highlight re-run was). The key moves only
    // when a new row terminates, so a growing block re-lays-out once per
    // committed row, not once per frame. Mirrors render_eager_slice.
    std::size_t term_block_count = 0;
    std::uint64_t term_slice_hash = 0;
    auto terminated_rendered = [&]() -> std::vector<std::shared_ptr<const Element>> {
        const std::uint64_t term_hash =
            (tail_term_cache_version_ == source_version_)
                ? tail_term_cache_hash_ : fnv1a64(terminated);
        if (!tail_term_cache_blocks_.empty()
            && tail_term_cache_len_      == terminated.size()
            && tail_term_cache_in_fence_ == in_code_fence_
            && (tail_term_cache_version_ == source_version_
                || tail_term_cache_hash_ == term_hash)) {
            term_block_count = tail_term_cache_blocks_.size();
            term_slice_hash  = tail_term_cache_hash_;
            tail_term_cache_version_ = source_version_;
            return tail_term_cache_blocks_;
        }
        auto blocks = canonical_blocks(fence_prefix + std::string{terminated});
        std::vector<std::shared_ptr<const Element>> els;
        els.reserve(blocks.size());
        for (auto& blk : blocks) {
            apply_live_table_floor_(blk);   // live-table width floor (no-op otherwise)
            els.push_back(std::make_shared<const Element>(md_block_to_element(blk)));
        }
        term_block_count           = blocks.size();
        term_slice_hash            = fnv1a64(terminated);
        tail_term_cache_blocks_    = els;
        tail_term_cache_len_       = terminated.size();
        tail_term_cache_hash_      = term_slice_hash;
        tail_term_cache_in_fence_  = in_code_fence_;
        tail_term_cache_version_   = source_version_;
        return els;
    }();
    if (terminated_rendered.empty()) return render_tail_inner(tail);

    // Wrap a cached terminated block in a hash-keyed ComponentElement so
    // the renderer blits its cells. Key = (tag, instance, terminated
    // slice hash, block index): stable while the terminated prefix is
    // unchanged, moves when a new row lands.
    auto wrap_term_block = [&](const std::shared_ptr<const Element>& blk,
                               std::size_t idx) -> Element {
        ComponentElement comp;
        comp.hash_id = ::maya::CacheIdBuilder{}
            .add(std::string_view{"strmd-term"})
            .add(static_cast<std::uint64_t>(instance_id_))
            .add(term_slice_hash)
            .add(static_cast<std::uint64_t>(idx))
            .build();
        comp.render = [blk](int, int) -> Element { return *blk; };
        return Element{std::move(comp)};
    };

    // Decide how to render the live partial last line. If folding it
    // into the block above (by appending a synthetic newline) keeps the
    // SAME block count, the live line is a CONTINUATION of the last
    // terminated block (another `>` quote row, another list item, another
    // table row) — render it canonically inside that block so it doesn't
    // pop from a separate inline row into the block when its real `\n`
    // lands (the 2-row collapse that shows up at the commit seam). If it
    // STARTS A NEW block (count grows) or isn't terminable cleanly, keep
    // it as a separate inline row below — a half-typed new-block marker
    // must stay literal so it can't snap shape.
    std::vector<Element> rendered;
    {
        std::string trimmed_live{live_line};
        while (!trimmed_live.empty()
               && (trimmed_live.back() == '\n' || trimmed_live.back() == '\r'))
            trimmed_live.pop_back();

        // A live line that is only fence characters (`\`` / `~`) is a
        // half-typed CLOSING fence — rendering it as a row (either inline
        // or as a code line inside the open fence) adds a row that
        // vanishes the instant the third backtick lands and the block
        // commits. Suppress it, but ONLY when we're genuinely inside an
        // open fence (otherwise a bare `` ` `` starting inline code in
        // prose would wrongly disappear).
        {
            bool all_fence = !trimmed_live.empty();
            for (char c : trimmed_live)
                if (c != '`' && c != '~') { all_fence = false; break; }
            if (all_fence) {
                // Fence parity over committed-side state + terminated
                // rows: an opener/closer is a line whose first non-space
                // run is >=3 of the same fence char. Odd parity = the
                // last terminated row left us INSIDE a fence, so this
                // all-fence live line is the closing delimiter.
                bool inside = in_code_fence_;
                std::size_t p = 0;
                while (p < terminated.size()) {
                    std::size_t e = terminated.find('\n', p);
                    if (e == std::string_view::npos) e = terminated.size();
                    std::string_view ln = terminated.substr(p, e - p);
                    std::size_t k = 0;
                    while (k < ln.size() && ln[k] == ' ') ++k;
                    if (ln.size() - k >= 3 &&
                        (ln[k] == '`' || ln[k] == '~')) {
                        char fc = ln[k];
                        std::size_t run = 0;
                        while (k + run < ln.size() && ln[k + run] == fc) ++run;
                        if (run >= 3) inside = !inside;
                    }
                    p = e + 1;
                }
                if (inside) trimmed_live.clear();
            }
        }

        bool live_continues = false;
        if (!trimmed_live.empty()) {
            std::string merged = fence_prefix + std::string{terminated};
            merged += trimmed_live;
            merged += '\n';
            auto merged_blocks = canonical_blocks(merged);
            if (merged_blocks.size() == term_block_count) {
                // Live line merged into the last block — the live row is a
                // continuation, so blocks [0, N-1) are byte-identical to
                // the terminated render (reuse the cached Elements) and
                // only the LAST block grew. Re-render just that one.
                live_continues = true;
                rendered.reserve(merged_blocks.size() + 1);
                for (std::size_t i = 0; i + 1 < merged_blocks.size(); ++i)
                    rendered.push_back(wrap_term_block(terminated_rendered[i], i));
                if (!merged_blocks.empty()) {
                    apply_live_table_floor_(merged_blocks.back());
                    rendered.push_back(
                        md_block_to_element(merged_blocks.back()));
                }
            }
        }
        if (!live_continues) {
            rendered.reserve(terminated_rendered.size() + 1);
            for (std::size_t i = 0; i < terminated_rendered.size(); ++i)
                rendered.push_back(wrap_term_block(terminated_rendered[i], i));
        }

        // Live line that STARTS a new block stays inline below.
        if (!live_continues && !trimmed_live.empty()) {
            std::string_view ll = live_line;
            while (!ll.empty() && ll.front() == '\n') ll.remove_prefix(1);
            if (!ll.empty()) {
                auto spans = parse_inlines(ll);
                std::string content;
                std::vector<StyledRun> runs;
                const Style base = Style{}.with_fg(colors::text);
                for (const auto& sp : spans)
                    flatten_inline(sp, base, content, runs);
                if (!runs.empty()) {
                    if (runs.size() == 1)
                        rendered.push_back(Element{TextElement{
                            .content = std::move(content), .style = runs[0].style}});
                    else
                        rendered.push_back(Element{TextElement{
                            .content = std::move(content),
                            .style   = Style{}.with_fg(colors::text),
                            .runs    = std::move(runs)}});
                }
            }
        }
    }
    if (rendered.empty()) return render_tail_inner(tail);

    Element canonical = (rendered.size() == 1)
        ? std::move(rendered.front())
        : detail::vstack().gap(1)(std::move(rendered)).build();

    // Emit the canonical render. The terminated rows are byte-identical
    // to what commit_range will produce (continuous across the commit
    // seam) and the live line is either folded into its continuing block
    // (no pop when it terminates) or held as a literal inline row that
    // can only grow. Both cases keep height monotonic.
    return canonical;
    }();

    // Store the memo so an unchanged tail next frame returns by
    // shared_ptr copy with zero parsing.
    tail_canon_cache_el_       = std::make_shared<const Element>(result);
    tail_canon_cache_version_  = source_version_;
    tail_canon_cache_len_      = tail.size();
    tail_canon_cache_hash_     = tail_hash;
    tail_canon_cache_in_fence_ = in_code_fence_;
    return result;
}

Element StreamingMarkdown::render_tail_inner(std::string_view tail) const {
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
            // Mirror the strip md_block_to_element applies post-commit: drop
            // one trailing '\n' on the live body so the live tail and the
            // committed render produce the SAME row count. Without this,
            // the closing fence commits and the renderer drops one row
            // (the trailing-newline phantom row) — a 1-row shrink at the
            // commit seam that the height-monotonicity test catches.
            if (!code.empty() && code.back() == '\n') code.pop_back();
        }
        // Match md_block_to_element's CodeBlock styling exactly — same
        // builder shape, same align_self(Stretch) so the in-flight tail
        // render and the committed block render produce identical
        // borders. Without the stretch the streaming-tail border tracks
        // the longest emitted line, drifting frame-to-frame.
        //
        // Empty body (opener landed but no content yet, OR opener+closer
        // with nothing between): render a 1-row dim placeholder that
        // mirrors md_block_to_element's empty-CodeBlock rendering.
        // Constant 1 row keeps live tail and committed render in
        // agreement (no shrink at commit seam) AND avoids reading as a
        // stray empty bordered box.
        if (code.empty()) {
            std::string label = lang.empty()
                ? std::string{"\xe2\x97\x8b empty code block"}
                : std::string{"\xe2\x97\x8b "} + lang + " (empty)";
            return Element{TextElement{
                .content = std::move(label),
                .style   = Style{}.with_fg(colors::strike_fg).with_dim(),
            }};
        }
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
        // The heading line ALWAYS occupies exactly one row, even when the
        // text is still empty (`# ` typed, no glyphs yet). An empty
        // TextElement collapses to zero rows, which drops the total height
        // BELOW the just-committed prefix for one frame and then snaps back
        // when the first text byte arrives — a visible 1-row flicker on
        // every streamed heading. Reserve the row with a single space so
        // height is monotonic across `#` -> `# ` -> `# A`.
        std::string content = text.empty() ? std::string{" "}
                                           : std::string{text};
        Element heading_el = Element{TextElement{
            .content = std::move(content),
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

    // ── Thematic break (---, ***, ___) at tail start: render the
    //    committed HRule the instant the line terminates. The line must
    //    be `\n`-terminated (a complete rule) so height is fixed at one
    //    row and can't snap. At tail start there's no open paragraph in
    //    this slice, so a `-` line is unambiguously an HR, not a setext
    //    underline. Any content after the rule's newline falls through
    //    to the inline parse below.
    {
        auto eol = body.find('\n');
        if (eol != std::string_view::npos) {
            std::string_view rule_line = body.substr(0, eol);
            auto strip_ws = [](std::string_view s) {
                while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
                    s.remove_prefix(1);
                while (!s.empty() && (s.back() == ' ' || s.back() == '\t'
                                      || s.back() == '\r'))
                    s.remove_suffix(1);
                return s;
            };
            std::string_view t = strip_ws(rule_line);
            bool is_hr = !t.empty();
            if (is_hr) {
                char c = t[0];
                if (c != '-' && c != '_' && c != '*') is_hr = false;
                else {
                    int count = 0;
                    for (char ch : t) {
                        if (ch == c) ++count;
                        else if (ch == ' ' || ch == '\t') continue;
                        else { is_hr = false; break; }
                    }
                    if (count < 3) is_hr = false;
                }
            }
            if (is_hr) {
                Element rule = md_block_to_element(md::Block{md::HRule{}});
                std::string_view rest = body.substr(eol + 1);
                while (!rest.empty() && rest.front() == '\n')
                    rest.remove_prefix(1);
                if (rest.empty()) return rule;
                auto spans = parse_inlines(rest);
                return detail::vstack().gap(1)(
                    std::move(rule),
                    build_inline_row(spans)
                );
            }
        }
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
            // document (cached on the slice bytes — re-emits the block
            // Elements without re-parsing when the slice is unchanged).
            std::string_view rendered_slice = body.substr(0, last_committed_end);
            std::vector<Element> kids;
            kids.reserve(2);
            render_eager_slice(rendered_slice, kids);

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
                    // Cached on the slice bytes so an unchanged table
                    // re-emits without re-parsing every frame.
                    std::string_view table_slice =
                        body.substr(0, last_row_end);
                    std::vector<Element> kids;
                    kids.reserve(2);
                    render_eager_slice(table_slice, kids);

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

} // namespace maya
