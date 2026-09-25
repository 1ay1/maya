// src/render/paint_component.cpp — a component: direct paint, or render()
// through the cross-frame cache (blit on a hit).
#include "render_internal.hpp"

namespace maya {
namespace render_detail {

void paint_component(const PaintCtx& c, const ComponentElement& node) {
    Canvas& canvas = c.canvas;
    StylePool& pool = c.pool;
    const int ax = c.ax;
    const int ay = c.ay;
    const int aw = c.aw;
    const int ah = c.ah;
    // Direct paint (a canvas animation): the component owns its
    // cells. No render(), no sub-tree, no cache: it's redrawn every
    // frame by definition, and the frame diff is what keeps the
    // wire small. Clipped to its rectangle so it can't paint over
    // a neighbour.
    if (node.draw) {
        const int cx = ax + node.layout.padding.left;
        const int cy = ay + node.layout.padding.top;
        const int cw = std::max(0, aw - static_cast<int>(node.layout.padding.horizontal()));
        const int ch = std::max(0, ah - static_cast<int>(node.layout.padding.vertical()));
        if (cw > 0 && ch > 0) {
            Canvas::ClipScope clip{canvas, Rect{{Columns{cx}, Rows{cy}},
                                                {Columns{cw}, Rows{ch}}}};
            node.draw(canvas, cx, cy, cw, ch);
        }
        return;
    }
    // Lazy component: call the render callback with the allocated size,
    // then render the resulting element tree into this region.
    if (!node.render) return;

    int content_w = std::max(0, aw - static_cast<int>(node.layout.padding.horizontal()));
    int content_h = std::max(0, ah - static_cast<int>(node.layout.padding.vertical()));
    int content_x = ax + node.layout.padding.left;
    int content_y = ay + node.layout.padding.top;

    // Clamp to the canvas content width — the SAME value the
    // measure pass clamps to (build_layout_tree's ComponentElement
    // measure uses available_width()). Measure and paint key the
    // same hash slot, and store_component_cache only preserves a
    // paint-captured cell block across the measure re-store when
    // the widths MATCH, so they must compute an identical width or
    // the cache never blits. A component handed the layout
    // engine's kUnconstrained width (1<<24 — the auto-height /
    // stretch case) is otherwise un-cacheable.
    if (const int aw_cap = available_width();
        aw_cap > 0 && content_w > aw_cap)
        content_w = aw_cap;

    auto& cache = component_cache();

    // ── Off-screen skip: nothing visible to paint ──────────────
    // A hash-keyed component whose entire row span sits ABOVE the
    // viewport top (content_y + ah <= 0) or BELOW the bottom
    // (content_y >= canvas_h) contributes zero visible cells this
    // frame — the clip scope would discard every write anyway.
    // Bail before the slow render/paint/capture pipeline.
    //
    // This is the load-bearing case for the windowed streaming
    // markdown head wrapper (and any tall frozen scrollback
    // prefix): it is laid out every frame by the outer flex pass
    // but scrolls fully off-screen the instant the live tail grows
    // past the viewport. Its cells can NEVER be captured (the
    // capture rect gates on content_y >= 0 && content_y+rows <=
    // canvas_h, both false off-screen), so without this bail the
    // paint slow-path re-render()ed it EVERY frame — O(prefix) per
    // frame, exactly the cost the windowing was meant to remove.
    // Its height is already known to the outer layout from the
    // measure cache, so skipping paint changes nothing visible.
    //
    // Gated on hash_id: a pointer-keyed component has no stable
    // cross-frame identity and its measure may depend on this
    // paint running, so leave that path untouched. Only skip when
    // the component genuinely has no on-canvas rows.
    //
    // Unconditional (cells or not): off-screen content contributes
    // zero visible cells this frame regardless of cache state, and
    // its height is already known to the outer layout from the
    // measure cache (which trusts the stored height even with empty
    // cells). Rendering it would only capture cells we can't use
    // (the capture rect requires on-canvas rows and would clear
    // them anyway), so skip the whole pipeline.
    if (!node.hash_id.empty()) {
        const int canvas_h_now = canvas.height();
        const bool fully_above = content_y + ah <= 0;
        const bool fully_below = content_y >= canvas_h_now;
        if (fully_above || fully_below) {
            // Keep the entry warm so the top-of-frame LRU sweep
            // doesn't evict a component merely scrolled out of view.
            if (auto* entry =
                    find_component_cache(cache, node, content_w)) {
                entry->last_frame = cache.current_frame;
                touch_hash_cache(cache, *entry);
            }
            return;
        }
    }

    // ── Fast path: cached cells exist for this entry ────────────
    // The first paint of an entry captures its painted cells
    // (see the miss path below). Every subsequent frame reaches
    // this branch and copies the cells straight onto the
    // canvas — a memcpy per row, no build_layout_tree, no
    // layout::compute, no recursive paint_element walk over
    // the cached Element. This is what makes per-cached-turn
    // cost O(width) rather than O(child_tree_size).
    //
    // Width tolerance (hash-keyed only). find_component_cache
    // demands an EXACT width match, which a genuine resize needs
    // (reflowed text). But a hash-keyed component can jitter by a
    // COLUMN or two frame-to-frame WITHOUT any real reflow when it
    // self-sizes against a parent whose resolved width wobbles ±1
    // (a streaming-markdown widget rendered as a flex root instead
    // of a stretched child; a live-reveal tail nudging the outer
    // vstack's natural width). Re-rendering the whole subtree for a
    // 1-column trailing-padding difference is pure waste — and on a
    // long committed prefix it re-renders every collapsed segment
    // (measured: 188 inner renders on a stray reveal frame at
    // width 98↔99). So: accept a hash-keyed entry whose stored
    // width differs from content_w AS LONG AS every cached row's
    // real content fits within BOTH widths (cells_max_col ≤ the
    // smaller width). Then the differing columns are provably blank
    // padding and the cached cells are visually exact; we blit the
    // overlapping min-width and the clip scope leaves the rest
    // (already blank) untouched. A real reflow moves content into
    // the wider column, cells_max_col exceeds the min width, and
    // this guard correctly falls through to a re-render.
    auto* fast_entry = find_component_cache(cache, node, content_w);
    if (!fast_entry && !node.hash_id.empty()) {
        auto it = cache.entries_by_hash.find(node.hash_id);
        if (it != cache.entries_by_hash.end()
            && !it->second.cells.empty()
            && it->second.cells_rows > 0) {
            // Only absorb SMALL sub-cell jitter, never a real
            // resize. A genuine terminal/pane resize moves the
            // width by many columns and MUST re-render so content
            // re-wraps + re-aligns to the new width (test_render_
            // scaling's resize gate depends on this). The layout
            // wobble we want to swallow is ±1-2 columns from a
            // self-sizing widget's natural-width flicker.
            constexpr int kWidthJitterEps = 2;
            const int dw = it->second.width - content_w;
            if (dw >= -kWidthJitterEps && dw <= kWidthJitterEps) {
                // Rightmost non-blank column across all cached rows.
                int max_col = -1;
                for (int c : it->second.cells_row_last_col)
                    if (c > max_col) max_col = c;
                const int min_w = std::min(it->second.width, content_w);
                // Content fits within both widths → the differing
                // columns are blank padding; cells are safe to blit.
                if (max_col < min_w)
                    fast_entry = &it->second;
            }
        }
    }
    if (auto* entry = fast_entry;
        entry && !entry->cells.empty() && entry->cells_rows > 0
        && entry->cells_ambient == ambient_bg())
    {
        entry->last_frame = cache.current_frame;
        if (!node.hash_id.empty())
            touch_hash_cache(cache, *entry);

        auto _ = canvas.clip_scope(Rect{
            {Columns{content_x}, Rows{content_y}},
            {Columns{content_w}, Rows{content_h}}
        });
        const int rows = std::min(entry->cells_rows, content_h);
        const int max_y = entry->cells_max_y;
        // Blit only the columns that exist in BOTH the cached row
        // and the paint region — a width-tolerant hit blits the
        // overlap; an exact hit blits the full stored width (they
        // are equal). The clip scope bounds writes to content_w so
        // a wider cached row can never overrun.
        const int blit_w = std::min(entry->width, content_w);
        const bool have_per_row =
            static_cast<int>(entry->cells_row_last_col.size()) == entry->cells_rows;
        // Hash-keyed entries are immutable content re-blitted to
        // the same rows every frame. Use the skip-if-identical
        // fast blit so an unchanged frozen prefix that survived
        // the frame's clear_below() (its rows were preserved,
        // not re-cleared) costs a read-only compare instead of a
        // full memcpy. Pointer-keyed entries (ephemeral, always
        // freshly cleared) can't benefit — keep the plain blit.
        const bool use_cached_blit = !node.hash_id.empty();
        // Bound the blit to rows that actually land ON the canvas.
        // A cached segment straddling the viewport top has a very
        // negative content_y (thousands of its rows are above row 0);
        // a segment whose bottom hangs below the viewport has rows
        // past canvas height. blit_packed_row_cached clips each of
        // those internally with a `y<0 || y>=height_` early-return —
        // but the LOOP still called it once per off-canvas row,
        // O(entry->cells_rows) wasted calls every frame on a tall
        // committed segment (the residual `cf` creep on a long turn).
        // The canvas row is content_y + y, so on-canvas rows are
        // exactly y in [max(0, -content_y), canvas_h - content_y).
        // The clip scope is [content_y, content_y+content_h) and
        // rows <= content_h, so the canvas clamp is the only binding
        // constraint. Skipped iterations were pure no-ops (an
        // early-returned blit updates no max_y_/last_col_ state), so
        // this is behaviour-neutral, just cheaper.
        const int canvas_h_blit = canvas.height();
        const int y_lo = std::max(0, -content_y);
        const int y_hi = std::min(rows, canvas_h_blit - content_y);

        // ── Per-row epoch skip ───────────────────────
        // If we blitted this exact entry to this exact place
        // last time (same canvas, position, span, clip), then
        // any row whose write-epoch hasn't advanced past our
        // recorded snapshot provably still holds our bytes AND
        // its last_col_ bookkeeping (every clear/write stamps).
        // Those rows are skipped with one integer compare each;
        // only rows someone touched since (the per-frame cleared
        // viewport band, an overlapping paint) fall through to
        // the bulk_eq/memcpy blit. Per-row — NOT all-or-nothing —
        // because a tall entry (a live turn spanning thousands of
        // rows) always has its bottom rows inside the cleared
        // band; disarming the whole entry for that would keep the
        // O(rows × width) compare cost this exists to kill. See
        // the record fields on ComponentCacheEntry for the proof.
        const bool skip_armed =
            use_cached_blit
            && entry->blit_skip_uid == canvas.uid()
            && entry->blit_skip_x    == content_x
            && entry->blit_skip_y    == content_y
            && entry->blit_skip_w    == blit_w
            && entry->blit_skip_y_lo == y_lo
            && entry->blit_skip_y_hi == y_hi
            && entry->blit_skip_clip == canvas.effective_clip();
        const std::uint64_t skip_snap =
            skip_armed ? entry->blit_skip_epoch : 0;

        std::uint64_t rows_skipped = 0, rows_compared = 0;
        g_blit_entries.fetch_add(1, std::memory_order_relaxed);
        for (int y = y_lo; y < y_hi; ++y) {
            if (skip_armed
                && canvas.row_write_epoch(content_y + y) <= skip_snap) {
                ++rows_skipped;
                continue;   // untouched since our last blit
            }
            ++rows_compared;
            const bool row_has_content = (y <= max_y);
            const int hint = have_per_row
                ? entry->cells_row_last_col[static_cast<std::size_t>(y)]
                : INT_MIN;
            if (use_cached_blit) {
                canvas.blit_packed_row_cached(
                    content_x, content_y + y,
                    entry->cells.data() + static_cast<std::size_t>(y) * entry->width,
                    blit_w,
                    row_has_content,
                    hint);
            } else {
                canvas.blit_packed_row(
                    content_x, content_y + y,
                    entry->cells.data() + static_cast<std::size_t>(y) * entry->width,
                    blit_w,
                    row_has_content,
                    hint);
            }
        }
        // Record the blit for next frame's whole-entry skip.
        // Snapshot AFTER the loop: every row we stamped is ≤ the
        // counter now; any later stamp (another component, a
        // clear) is strictly greater and disarms the record.
        g_blit_rows_epoch_skip.fetch_add(rows_skipped,
                                         std::memory_order_relaxed);
        g_blit_rows_compared.fetch_add(rows_compared,
                                       std::memory_order_relaxed);
        if (use_cached_blit) {
            entry->blit_skip_uid   = canvas.uid();
            entry->blit_skip_epoch = canvas.write_epoch_counter();
            entry->blit_skip_x     = content_x;
            entry->blit_skip_y     = content_y;
            entry->blit_skip_w     = blit_w;
            entry->blit_skip_y_lo  = y_lo;
            entry->blit_skip_y_hi  = y_hi;
            entry->blit_skip_clip  = canvas.effective_clip();
        }
        return;
    }

    // ── Slow path: render + lay out + paint, then capture cells ─
    // Either no entry, no cells yet (just-populated by the
    // measure miss path), or width mismatch (entry stale from
    // a prior terminal width). In all three cases we have to
    // run the recursive paint pipeline; we then snapshot the
    // resulting cells into the entry so every subsequent
    // frame takes the fast path above.
    //
    // Honor a cached `entry->result` when:
    //  - hash_id is set (host opted into cross-frame identity), OR
    //  - the entry was stored this frame (within-frame reuse:
    //    the measure pass just populated `result`, so we can
    //    skip a redundant render() call here).
    // Pointer-keyed entries CAN hold a stale Element tree from
    // a prior frame's render() that closed over mutable state
    // (StreamingMarkdown's prefix is the canonical case), so we
    // reject cross-frame pointer-keyed result reuse.
    const Element* child_ptr = nullptr;
    Element        fresh_render;
    ComponentCacheEntry* reuse_entry = [&]() -> ComponentCacheEntry* {
        if (auto* e = find_component_cache(cache, node, content_w)) {
            if (!node.hash_id.empty()
                || e->last_frame == cache.current_frame) {
                return e;
            }
        }
        return nullptr;
    }();
    if (reuse_entry) {
        child_ptr = &reuse_entry->result;
        reuse_entry->last_frame = cache.current_frame;
        if (!node.hash_id.empty())
            touch_hash_cache(cache, *reuse_entry);
    } else {
        // Render fresh. For pointer-keyed entries we still
        // store the result so this frame's measure-then-paint
        // pair can share a single render() call — but we
        // don't trust it across frames. store_component_cache
        // returns the inserted entry pointer so we don't pay
        // a second hashmap probe to get back at it.
        g_component_render_calls.fetch_add(
            1, std::memory_order_relaxed);
        fresh_render = node.render(content_w, content_h);
        auto* stored = store_component_cache(cache, node, {
            content_w,
            /*height=*/content_h,
            fresh_render,
            cache.current_frame,
            node.generation
        });
        child_ptr = stored ? &stored->result : &fresh_render;
    }
    const Element& child = *child_ptr;

    // Reuse a thread-local scratch buffer for the sub-layout tree.
    thread_local std::vector<layout::LayoutNode> tl_sub_nodes;
    thread_local bool tl_in_use = false;

    auto use_local = tl_in_use;
    std::vector<layout::LayoutNode> local_nodes;
    auto& sub_nodes = use_local ? local_nodes : tl_sub_nodes;
    sub_nodes.clear();
    tl_in_use = true;

    std::size_t sub_root = build_layout_tree(child, sub_nodes, {});

    sub_nodes[sub_root].style.width = Dimension::fixed(content_w);
    sub_nodes[sub_root].style.height = Dimension::fixed(content_h);
    layout::compute(sub_nodes, sub_root, content_w, content_h);

    const int max_y_before = canvas.max_content_row();

    {
        auto _ = canvas.clip_scope(Rect{
            {Columns{content_x}, Rows{content_y}},
            {Columns{content_w}, Rows{content_h}}
        });
        // Blank our region before paint so the captured cells
        // (below) contain ONLY this component's output, not
        // stale content from earlier frames that the
        // clear_rows() bound at the top of render_live may not
        // have covered (it only clears up to prev_rows + 4;
        // content that's grown past that horizon leaves
        // residue). Cost: content_w * content_h cell writes,
        // paid once per (hash_id, width) and amortized across
        // every subsequent fast-path blit. We only run this
        // when the entry is going to be cells-cached.
        const bool will_cache =
            !node.hash_id.empty() && content_w > 0 && content_h > 0;
        if (will_cache) {
            // Blank with the AMBIENT bg, not the default style:
            // this component may sit on a bg strip (ambient-bg
            // inheritance), and a style-0 fill would bake
            // terminal-default holes into the captured cells —
            // the blit would then punch through the strip on
            // every subsequent frame. The ambient is recorded
            // on the entry below so the fast path only blits
            // when the same strip is (or isn't) behind it.
            const auto& amb = ambient_bg();
            const uint16_t fill_sid = amb.has_value()
                ? pool.intern(Style{}.with_bg(*amb))
                : 0;
            canvas.fill(
                Rect{{Columns{content_x}, Rows{content_y}},
                     {Columns{content_w}, Rows{content_h}}},
                U' ', fill_sid);
        }
        paint_element(child, canvas, pool, sub_nodes, sub_root,
                      content_x, content_y);
    }
    if (!use_local) tl_in_use = false;

    // ── Capture the painted region into the cache entry ─────────
    // Re-lookup the entry AFTER paint_element — recursive
    // cache inserts during the walk above can rehash the map
    // and invalidate any pointer we held from earlier. Same
    // map operation pattern as the find before; the lookup is
    // a single hashmap probe.
    //
    // Capture is gated on hash_id being set: pointer-keyed
    // entries are by definition ephemeral (the wrapper has a
    // fresh address each frame), and caching cells for them
    // would burn memory that's evicted next frame anyway.
    if (!node.hash_id.empty()) {
        if (auto* entry = find_component_cache(cache, node, content_w)) {
            const int max_y_after = canvas.max_content_row();
            int captured_rows = content_h;
            if (entry->height > 0 && entry->height < captured_rows)
                captured_rows = entry->height;
            // Skip cache capture when the read rect overruns
            // the canvas — happens on the first pass of the
            // inline grow-and-retry loop in src/app/render_inline.cpp before
            // the canvas is resized to fit. Capturing
            // partial-blank rows from OOB reads poisons the
            // cache: garbage style IDs in the cached cells
            // are blitted into the next frame's canvas, and
            // emit_cell_run then dereferences a bad StylePool
            // entry. Invalidate any stale entry from an
            // earlier smaller-layout frame so the fast path
            // can't serve undersized cells.
            const int canvas_w = canvas.width();
            const int canvas_h = canvas.height();
            // Columns that actually exist on the canvas from this
            // component's x origin. content_w may exceed this when
            // the component sits at content_x>0 (panel border +
            // padding) and its width was clamped to the full
            // canvas width: the rightmost (content_w - cap_w)
            // columns fall past the edge. We capture the visible
            // columns and zero-fill the overrun tail rather than
            // bailing on the whole capture (the old behaviour,
            // which left the cache permanently empty for any
            // bordered panel whose body spanned the full width →
            // every frame re-rendered). The stored entry width
            // stays content_w so blit + lookup remain consistent.
            const int cap_w = std::min(content_w,
                                       std::max(0, canvas_w - content_x));
            const bool fits_rows =
                content_x >= 0 && content_y >= 0 &&
                content_y + captured_rows <= canvas_h;
            // The cells (and thus the bytes any prior blit left on
            // a canvas) are being replaced — a stale skip record
            // would vouch for the OLD bytes. Disarm it; the next
            // blit re-records against the new cells.
            entry->blit_skip_uid = 0;
            if (!fits_rows || cap_w <= 0) {
                entry->cells.clear();
                entry->cells_rows = 0;
                entry->cells_max_y = -1;
                entry->cells_ambient.reset();
                entry->cells_row_last_col.clear();
            } else if (captured_rows > 0) {
                // Fast capture: per-row memcpy of the visible
                // columns direct from the canvas backing store,
                // zero-filling any tail past the canvas edge.
                entry->cells_rows = captured_rows;
                const std::size_t cap_bytes =
                    static_cast<std::size_t>(cap_w) * sizeof(uint64_t);
                const int canvas_w_eff = canvas.width();
                const uint64_t* cbase = canvas.cells();
                entry->cells.assign(
                    static_cast<std::size_t>(captured_rows)
                        * static_cast<std::size_t>(content_w),
                    Cell{}.pack());
                entry->cells_row_last_col.assign(
                    static_cast<std::size_t>(captured_rows), -1);
                constexpr uint64_t kBlank = Cell{}.pack();
                int last_nonblank = -1;
                for (int y = 0; y < captured_rows; ++y) {
                    uint64_t* dst =
                        entry->cells.data()
                        + static_cast<std::size_t>(y)
                          * static_cast<std::size_t>(content_w);
                    const uint64_t* src =
                        cbase
                        + static_cast<std::size_t>(content_y + y)
                          * static_cast<std::size_t>(canvas_w_eff)
                        + static_cast<std::size_t>(content_x);
                    std::memcpy(dst, src, cap_bytes);
                    int row_last = -1;
                    for (int x = cap_w - 1; x >= 0; --x) {
                        if (dst[static_cast<std::size_t>(x)] != kBlank) {
                            row_last = x;
                            break;
                        }
                    }
                    entry->cells_row_last_col[
                        static_cast<std::size_t>(y)] = row_last;
                    if (row_last >= 0) last_nonblank = y;
                }
                entry->cells_max_y = last_nonblank;
                // Stamp the ambient the cells were captured under;
                // the fast path refuses to blit under a different
                // ambient and re-renders + recaptures instead.
                entry->cells_ambient = ambient_bg();
                (void)max_y_before;
                (void)max_y_after;
            }
        }
    }
}

} // namespace render_detail
} // namespace maya
