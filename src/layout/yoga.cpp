#include <maya/layout/yoga.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace maya::layout {

namespace detail {

// Per-recursion-depth scratch buffers for flex layout. compute_node()
// allocated a fresh `std::vector<FlexItem>` and `std::vector<FlexLine>`
// (the latter a vector-of-vectors) on EVERY container node EVERY frame —
// for a deep UI that's hundreds of heap allocations per layout pass and
// was the dominant cost (~75% of render time). These pools hand each
// recursion level a buffer that retains capacity across nodes and frames,
// so after warmup the layout pass performs ZERO heap allocation. Keyed by
// recursion depth so a parent's buffers are never clobbered by a child's
// compute_node call. Thread-local: each thread gets its own pool.
struct FlexScratch {
    std::vector<FlexItem> items;       // reused for `all_items`
    std::vector<FlexLine> lines;       // reused for `lines`
};

inline FlexScratch& flex_scratch_at(std::size_t depth) {
    thread_local std::vector<std::unique_ptr<FlexScratch>> pool;
    if (depth >= pool.size()) pool.resize(depth + 1);
    auto& slot = pool[depth];
    if (!slot) slot = std::make_unique<FlexScratch>();
    return *slot;
}

// RAII depth counter for the layout recursion.
struct LayoutDepthGuard {
    std::size_t* d;
    explicit LayoutDepthGuard(std::size_t* p) : d(p) { ++*d; }
    ~LayoutDepthGuard() { --*d; }
};

inline std::size_t& layout_depth() {
    thread_local std::size_t depth = 0;
    return depth;
}

// "Unconstrained" sentinel for available_w / available_h when we want a
// child to compute its natural size — used when a parent has overflow ≠
// Visible (the child will be clipped or scrolled, so it should keep its
// intrinsic dimensions instead of being squeezed to fit the viewport).
// Big enough to never trigger shrink in practice, small enough to leave
// integer arithmetic headroom (no overflow when multiplied by typical
// flex weights).
inline constexpr int kUnconstrained = 1 << 24;

std::pair<int, int> resolve_definite_size(
    const FlexStyle& style,
    int parent_width,
    int parent_height) noexcept
{
    int w = style.width.is_auto() ? -1 : style.width.resolve(parent_width);
    int h = style.height.is_auto() ? -1 : style.height.resolve(parent_height);

    // Apply min/max constraints when definite
    if (w >= 0) w = clamp_dim(w, style.min_width, style.max_width, parent_width);
    if (h >= 0) h = clamp_dim(h, style.min_height, style.max_height, parent_height);

    return {w, h};
}

void compute_node(
    std::vector<LayoutNode>& nodes,
    std::size_t idx,
    int available_width,
    int available_height,
    int parent_width,
    int parent_height)
{
    auto& node  = nodes[idx];
    auto& style = node.style;

    // Display::None -- produce a zero-size rect and bail out.
    if (style.display == Display::None) [[unlikely]] {
        node.computed = {};
        return;
    }

    // ------------------------------------------------------------------
    // 1. Resolve this node's own width and height
    // ------------------------------------------------------------------
    auto [def_w, def_h] = resolve_definite_size(style, parent_width, parent_height);

    // When a dimension is definite, use it. When auto, use available_* only
    // for the width (content always needs a column budget for text measurement).
    // For auto height, start at 0 and size to content — using available_height
    // would inflate auto-height row containers, making their children expand
    // to the parent's full height instead of sizing to their own content.
    int node_outer_w = (def_w >= 0) ? def_w : available_width;
    int node_outer_h = (def_h >= 0) ? def_h : 0;

    // Content box: subtract border + padding
    int content_w = std::max(0, node_outer_w - inner_horizontal(style));
    int content_h = std::max(0, node_outer_h - inner_vertical(style));

    // ------------------------------------------------------------------
    // 2. Leaf node with measure function
    // ------------------------------------------------------------------
    if (node.children.empty()) [[likely]] {
        if (node.measure) [[likely]] {
            Size measured = node.measure(content_w);
            int mw = measured.width.value;
            int mh = measured.height.value;

            if (def_w < 0) {
                mw = clamp_dim(mw, style.min_width, style.max_width, parent_width);
                node_outer_w = mw + inner_horizontal(style);
            }
            if (def_h < 0) {
                mh = clamp_dim(mh, style.min_height, style.max_height, parent_height);
                node_outer_h = mh + inner_vertical(style);
            }
        } else {
            // No children, no measure: use definite sizes or zero
            if (def_w < 0) node_outer_w = inner_horizontal(style);
            if (def_h < 0) node_outer_h = inner_vertical(style);
        }

        node.computed.size = {Columns{node_outer_w}, Rows{node_outer_h}};
        return;
    }

    // ------------------------------------------------------------------
    // 3. Container node -- perform flex layout on children
    // ------------------------------------------------------------------
    bool row     = is_row(style.direction);
    bool reverse = is_reverse(style.direction);

    // Acquire this recursion level's reusable scratch (zero per-node heap
    // allocation after warmup). Depth-keyed so a child compute_node() call
    // below gets a DIFFERENT slot and never clobbers our items/lines.
    auto& depth = layout_depth();
    LayoutDepthGuard depth_guard{&depth};
    FlexScratch& scratch = flex_scratch_at(depth);

    // Available space on main and cross axes for children
    int main_avail  = row ? content_w : content_h;
    int cross_avail = row ? content_h : content_w;

    // Whether the main dimension is definite (explicit or from flex-grow override).
    bool main_definite = row ? (def_w >= 0) : (def_h >= 0);

    // ------------------------------------------------------------------
    // 3a. Collect visible children and compute their hypothetical main size
    // ------------------------------------------------------------------
    std::vector<FlexItem>& all_items = scratch.items;
    all_items.clear();
    all_items.reserve(node.children.size());


    for (std::size_t ci : node.children) {
        auto& child = nodes[ci];
        if (child.style.display == Display::None) continue;

        // Determine the child's hypothetical main size.
        // flex_basis takes priority, then width/height, then measure/auto.
        const auto& cs = child.style;

        int child_outer_main_extra  = row ? outer_horizontal(cs) : outer_vertical(cs);
        int child_outer_cross_extra = row ? outer_vertical(cs)   : outer_horizontal(cs);

        // Resolve flex basis
        Dimension basis = cs.flex_basis;
        if (basis.is_auto()) {
            basis = row ? cs.width : cs.height;
        }

        int hypo_main;
        bool laid_out_in_3a = false;
        int avail_w_3a = 0, avail_h_3a = 0;
        if (!basis.is_auto()) {
            hypo_main = basis.resolve(main_avail);
        } else if (child.measure) {
            // Measure leaf to get intrinsic size. Only scroll viewports
            // measure unconstrained — we want long text inside a scroll
            // viewport to keep its full width so the user can scroll to
            // see it. Hidden containers (plain clip without scroll
            // state) should pass the constrained width so children wrap
            // to fit, matching CSS overflow:hidden semantics.
            const int measure_w = (style.overflow == Overflow::Scroll)
                ? kUnconstrained
                : (row ? main_avail : content_w);
            Size ms = child.measure(std::max(0, measure_w - inner_horizontal(cs)));
            hypo_main = row ? ms.width.value + inner_horizontal(cs) : ms.height.value + inner_vertical(cs);
        } else {
            // Recurse to determine child's natural size. Scroll viewports
            // pass infinite available so descendants compute natural
            // sizes (allowing inner content wider than the viewport to
            // be scrolled into view). Hidden containers behave like
            // Visible for layout — only paint-time clipping differs.
            const int child_avail_w = (style.overflow == Overflow::Scroll)
                ? kUnconstrained : (row ? main_avail : content_w);
            const int child_avail_h = (style.overflow == Overflow::Scroll)
                ? kUnconstrained : (row ? content_h  : main_avail);
            compute_node(nodes, ci, child_avail_w, child_avail_h, content_w, content_h);
            laid_out_in_3a = true;
            avail_w_3a = child_avail_w;
            avail_h_3a = child_avail_h;

            hypo_main = row ? nodes[ci].computed.size.width.value
                            : nodes[ci].computed.size.height.value;
        }

        // Apply min/max on the main axis
        if (row) {
            hypo_main = clamp_dim(hypo_main, cs.min_width, cs.max_width, parent_width);
        } else {
            hypo_main = clamp_dim(hypo_main, cs.min_height, cs.max_height, parent_height);
        }

        all_items.push_back(FlexItem{
            .index        = ci,
            .hypothetical = hypo_main,
            .main         = hypo_main,
            .cross        = 0,
            .main_offset  = 0,
            .cross_offset = 0,
            .laid_out     = laid_out_in_3a,
            .avail_w      = avail_w_3a,
            .avail_h      = avail_h_3a,
        });
    }

    // ------------------------------------------------------------------
    // 3b. Break items into flex lines (wrapping support)
    // ------------------------------------------------------------------
    // Reuse this depth's pooled lines vector. We keep the FlexLine slots
    // (and their inner `items` vectors' capacity) alive across frames:
    // grab the next slot, clear its items in place, fill it. `n_lines`
    // tracks how many slots are live this pass; trailing slots from a
    // previous, larger frame stay allocated but unused (capacity retained).
    std::vector<FlexLine>& lines = scratch.lines;
    std::size_t n_lines = 0;
    auto next_line = [&]() -> FlexLine& {
        if (n_lines >= lines.size()) lines.emplace_back();
        FlexLine& l = lines[n_lines++];
        l.items.clear();
        l.main_size = 0;
        l.cross_size = 0;
        return l;
    };

    if (style.wrap == FlexWrap::NoWrap || all_items.empty()) {
        // Single line containing all items
        FlexLine& line = next_line();
        line.items.assign(all_items.begin(), all_items.end());
    } else {
        // Multi-line wrapping
        FlexLine* current = &next_line();
        int line_main = 0;
        for (auto& item : all_items) {
            int needed = item.hypothetical + (current->items.empty() ? 0 : style.gap);
            if (!current->items.empty() && line_main + needed > main_avail) {
                current = &next_line();
                line_main = 0;
            }
            line_main += item.hypothetical + (current->items.empty() ? 0 : style.gap);
            current->items.push_back(item);
        }
        // A trailing empty line can occur only if all_items was empty, which
        // the NoWrap branch above already handles; here every line has items.
    }

    if (style.wrap == FlexWrap::WrapReverse) {
        std::reverse(lines.begin(), lines.begin() + n_lines);
    }

    // View over only the live line slots this pass (trailing pooled slots
    // from a previous larger frame are excluded). All loops below iterate
    // `active_lines`, never `lines` directly.
    std::span<FlexLine> active_lines{lines.data(), n_lines};


    // ------------------------------------------------------------------
    // 3c. Resolve flex-grow and flex-shrink within each line
    // ------------------------------------------------------------------
    for (auto& line : active_lines) {
        int num_gaps = static_cast<int>(line.items.size()) - 1;
        if (num_gaps < 0) num_gaps = 0;
        int total_gap = num_gaps * style.gap;

        // Sum of hypothetical main sizes
        int total_hypo = 0;
        for (auto& it : line.items) total_hypo += it.hypothetical;

        int free_space = main_avail - total_hypo - total_gap;

        if (free_space > 0 && main_definite) {
            // Distribute positive free space via flex_grow.
            // Only when the main axis is definite — auto-sized containers
            // shrink-wrap to content and have no free space to distribute.
            float total_grow = 0.0f;
            for (auto& it : line.items) {
                total_grow += nodes[it.index].style.flex_grow;
            }
            if (total_grow > 0.0f) {
                // Integer-safe distribution: hand out floor portions, then
                // distribute remainder one cell at a time to avoid rounding loss.
                int remaining = free_space;
                for (auto& it : line.items) {
                    float grow = nodes[it.index].style.flex_grow;
                    if (grow > 0.0f) {
                        int portion = static_cast<int>(
                            static_cast<float>(free_space) * grow / total_grow);
                        it.main += portion;
                        remaining -= portion;
                    }
                }
                // Distribute leftover cells (rounding residue) to items with grow > 0
                for (auto& it : line.items) {
                    if (remaining <= 0) break;
                    if (nodes[it.index].style.flex_grow > 0.0f) {
                        it.main += 1;
                        remaining -= 1;
                    }
                }
            }
        } else if (free_space < 0 && main_definite
                && style.overflow != Overflow::Scroll) {
            // Shrink overflowing items via flex_shrink — but ONLY when
            // the container won't scroll. Scroll viewports want
            // children to keep natural sizes so the user can scroll
            // through wider content; Visible and Hidden both shrink as
            // CSS specifies.
            float total_shrink_weighted = 0.0f;
            for (auto& it : line.items) {
                total_shrink_weighted +=
                    nodes[it.index].style.flex_shrink * static_cast<float>(it.hypothetical);
            }
            if (total_shrink_weighted > 0.0f) {
                // CSS flex items have an automatic minimum size
                // (min-{width,height}:auto resolves to the item's min-content
                // size) and must NOT shrink below it. We don't run a separate
                // min-content pass, but every visible item needs at least ONE
                // cell on the main axis. Flooring at 1 is the load-bearing
                // fix for the overflow smear: without it, default
                // flex_shrink=1 collapsed overflowing column rows to height 0,
                // so `main_cursor += item.main` never advanced and every row
                // piled onto the SAME y — a longer row's tail then painted
                // through the shorter row stacked on top of it
                // ("…replenishedt 2 pods"). With a 1-cell floor the rows keep
                // distinct offsets and the genuine overflow past the content
                // box is removed by the definite-size clip in the renderer.
                // An explicit min-{width,height} overrides the auto floor.
                auto floor_of = [&](const FlexItem& it) -> int {
                    const auto& cs = nodes[it.index].style;
                    const Dimension& mn = row ? cs.min_width : cs.min_height;
                    if (!mn.is_auto())
                        return mn.resolve(row ? parent_width : parent_height);
                    return it.hypothetical > 0 ? 1 : 0;
                };
                int overflow = -free_space;
                int remaining = overflow;
                for (auto& it : line.items) {
                    float shrink = nodes[it.index].style.flex_shrink;
                    float weight = shrink * static_cast<float>(it.hypothetical);
                    int reduction = static_cast<int>(
                        static_cast<float>(overflow) * weight / total_shrink_weighted);
                    int max_reduction = std::max(0, it.main - floor_of(it));
                    reduction = std::min(reduction, max_reduction);
                    it.main -= reduction;
                    remaining -= reduction;
                }
                // Distribute leftover shrink to the largest items first to
                // avoid destroying small items (e.g., a 1-row header). Same
                // floor: an item with content stops shrinking at its min so
                // it can never collapse onto its neighbour's row.
                while (remaining > 0) {
                    int best = -1;
                    int best_main = 0;
                    for (int i = 0; i < static_cast<int>(line.items.size()); ++i) {
                        auto& it = line.items[static_cast<std::size_t>(i)];
                        if (nodes[it.index].style.flex_shrink > 0.0f &&
                            it.main > floor_of(it) && it.main > best_main) {
                            best = i;
                            best_main = it.main;
                        }
                    }
                    if (best < 0 || best_main <= 0) break;
                    line.items[static_cast<std::size_t>(best)].main -= 1;
                    remaining -= 1;
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // 3d. Recursively lay out each child with its resolved main size,
    //     then determine cross sizes.
    // ------------------------------------------------------------------
    for (auto& line : active_lines) {
        int line_cross = 0;
        for (auto& item : line.items) {
            auto& child = nodes[item.index];
            const auto& cs = child.style;

            int child_w, child_h;
            // Effective cross-axis alignment for THIS child (align_self
            // overrides the container's align_items; Auto inherits). Only a
            // Stretch child should be forced to the container's cross size
            // before positioning — Center/Start/End children keep their
            // natural cross size so the alignment step below has room to
            // move them. Without this gate an auto-cross child is stretched
            // to fill the cross axis unconditionally, making align=center a
            // no-op (the item already spans the whole line, so the centering
            // offset is zero).
            const Align eff_align =
                (cs.align_self != Align::Auto) ? cs.align_self
                                               : style.align_items;
            const bool cross_stretch = (eff_align == Align::Stretch
                                     || eff_align == Align::Auto);
            if (row) {
                child_w = item.main;
                child_h = cs.height.is_auto() ? cross_avail : cs.height.resolve(content_h);
            } else {
                child_h = item.main;
                child_w = cs.width.is_auto() ? cross_avail : cs.width.resolve(content_w);
            }

            // Apply min/max on resolved sizes
            child_w = clamp_dim(child_w, cs.min_width, cs.max_width, parent_width);
            child_h = clamp_dim(child_h, cs.min_height, cs.max_height, parent_height);

            // Recursively compute children of this child
            int inner_w = std::max(0, child_w - inner_horizontal(cs));
            int inner_h = std::max(0, child_h - inner_vertical(cs));
            if (!child.children.empty()) {
                if (style.overflow == Overflow::Scroll) {
                    // Scroll viewport — children keep natural cross sizes
                    // so wider inner content can be scrolled into view.
                    // The renderer's paint-time scroll translation and
                    // clip rect handle visibility.
                    compute_node(nodes, item.index, kUnconstrained, kUnconstrained,
                                 content_w, content_h);
                    child_w = child.computed.size.width.value;
                    child_h = child.computed.size.height.value;
                } else {
                    // Visible OR Hidden: children stretch to the cross-axis
                    // size we just resolved. Hidden simply clips at paint
                    // time — it should not change layout behaviour, so
                    // bordered cards with align_items=Stretch still get
                    // their children stretched as CSS specifies.
                    bool cross_definite = row ? (def_h >= 0) : (def_w >= 0);
                    auto saved_w = cs.width;
                    auto saved_h = cs.height;
                    bool main_changed = (item.main != item.hypothetical);

                    // Determine whether 3d would force either axis to a
                    // definite size (overriding the child's intrinsic).
                    // The cross axis is only forced to the container's
                    // definite size when the child actually stretches;
                    // a Center/Start/End child keeps its natural cross
                    // size so the 3e alignment step can position it.
                    bool force_w, force_h;
                    if (row) {
                        force_w = (main_changed || !saved_w.is_auto());
                        force_h = ((cross_definite && cross_stretch) || !saved_h.is_auto());
                    } else {
                        force_h = (main_changed || !saved_h.is_auto());
                        force_w = ((cross_definite && cross_stretch) || !saved_w.is_auto());
                    }

                    // Fast path: when 3a already laid this child out at the
                    // exact same available width/height that 3d would pass,
                    // and 3d forces neither axis (so no fixed() override
                    // changes the inputs) and the main size didn't move, the
                    // 3a `computed` IS the final result. Re-running
                    // compute_node would reproduce it bit-for-bit. Skipping
                    // it removes the redundant second full layout of every
                    // nested container — the dominant render cost for
                    // table/list/card UIs. Inputs equal ⇒ outputs equal:
                    // this is exact, not an approximation.
                    if (item.laid_out && !main_changed && !force_w && !force_h
                            && child_w == item.avail_w && child_h == item.avail_h) {
                        child_w = child.computed.size.width.value;
                        child_h = child.computed.size.height.value;
                        item.main  = row ? child_w : child_h;
                        item.cross = row ? child_h : child_w;
                        line_cross = std::max(line_cross, item.cross);
                        continue;
                    }

                    if (row) {
                        // Force width (main) only when grow/shrink changed it
                        // or it was already explicit — otherwise let the child
                        // recompute its intrinsic width at the resolved cross size.
                        if (main_changed || !saved_w.is_auto())
                            child.style.width = Dimension::fixed(child_w);
                        if ((cross_definite && cross_stretch) || !saved_h.is_auto())
                            child.style.height = Dimension::fixed(child_h);
                    } else {
                        // Force height (main) only when grow/shrink changed it
                        // or it was already explicit — auto-height children must
                        // recompute after their cross (width) is resolved, since
                        // narrower widths cause text to wrap to more lines.
                        if (main_changed || !saved_h.is_auto())
                            child.style.height = Dimension::fixed(child_h);
                        if ((cross_definite && cross_stretch) || !saved_w.is_auto())
                            child.style.width = Dimension::fixed(child_w);
                    }
                    compute_node(nodes, item.index, child_w, child_h, content_w, content_h);
                    child.style.width  = saved_w;
                    child.style.height = saved_h;
                    child_w = child.computed.size.width.value;
                    child_h = child.computed.size.height.value;
                }
            } else if (child.measure) {
                // Same unconstrained-measure rule as section 3a above:
                // when the parent will clip / scroll, measure the leaf
                // at its natural size rather than wrap to viewport width.
                const int probe_w = (style.overflow != Overflow::Visible)
                    ? kUnconstrained : inner_w;
                Size ms = child.measure(probe_w);
                if (cs.width.is_auto() && !row) {
                    child_w = clamp_dim(ms.width.value + inner_horizontal(cs),
                                        cs.min_width, cs.max_width, parent_width);
                }
                if (cs.height.is_auto() && row) {
                    child_h = clamp_dim(ms.height.value + inner_vertical(cs),
                                        cs.min_height, cs.max_height, parent_height);
                }
                // For non-container measured nodes, set computed size directly.
                child.computed.size = {Columns{child_w}, Rows{child_h}};
            } else {
                child.computed.size = {Columns{child_w}, Rows{child_h}};
            }

            item.main  = row ? child_w : child_h;
            item.cross = row ? child_h : child_w;

            line_cross = std::max(line_cross, item.cross);
        }
        line.cross_size = line_cross;

        // Sum main sizes + gaps for this line
        int lm = 0;
        for (std::size_t i = 0; i < line.items.size(); ++i) {
            lm += line.items[i].main;
            if (i + 1 < line.items.size()) lm += style.gap;
        }
        line.main_size = lm;
    }

    // For single-line containers with a definite cross dimension, the
    // line's cross size spans the full available cross space (CSS §9.4).
    bool cross_definite_for_line = row ? (def_h >= 0) : (def_w >= 0);
    if (n_lines == 1 && cross_definite_for_line) {
        active_lines[0].cross_size = std::max(active_lines[0].cross_size, cross_avail);
    }

    // ------------------------------------------------------------------
    // 3e. Position children: justify-content along main, align on cross
    // ------------------------------------------------------------------
    int total_cross = 0;
    for (auto& line : active_lines) total_cross += line.cross_size;

    int cross_cursor = 0;
    for (auto& line : active_lines) {
        int n = static_cast<int>(line.items.size());
        int free_main = main_avail - line.main_size;
        if (free_main < 0) free_main = 0;

        // Determine starting offset and spacing based on justify_content
        int main_start   = 0;
        int main_between = 0;

        switch (style.justify_content) {
            case Justify::Start:
            case Justify::Auto:
            case Justify::Stretch:
                main_start = 0;
                main_between = 0;
                break;

            case Justify::End:
                main_start = free_main;
                main_between = 0;
                break;

            case Justify::Center:
                main_start = free_main / 2;
                main_between = 0;
                break;

            case Justify::SpaceBetween:
                main_start = 0;
                main_between = (n > 1) ? free_main / (n - 1) : 0;
                break;

            case Justify::SpaceAround:
                if (n > 0) {
                    int unit = free_main / (2 * n);
                    main_start = unit;
                    main_between = 2 * unit;
                }
                break;

            case Justify::SpaceEvenly:
                if (n > 0) {
                    int unit = free_main / (n + 1);
                    main_start = unit;
                    main_between = unit;
                }
                break;
        }

        // Walk items along main axis
        int main_cursor = main_start;
        for (std::size_t i = 0; i < line.items.size(); ++i) {
            auto& item = line.items[i];
            item.main_offset = main_cursor;
            main_cursor += item.main + style.gap + main_between;
        }

        // Handle reverse: mirror offsets
        if (reverse) {
            for (auto& item : line.items) {
                item.main_offset = main_avail - item.main_offset - item.main;
            }
        }

        // Cross-axis alignment per item
        for (auto& item : line.items) {
            const auto& cs = nodes[item.index].style;
            Align align = (cs.align_self != Align::Auto) ? cs.align_self : style.align_items;

            switch (align) {
                case Align::Start:
                case Align::Auto:
                    item.cross_offset = cross_cursor;
                    break;

                case Align::End:
                    item.cross_offset = cross_cursor + line.cross_size - item.cross;
                    break;

                case Align::Center:
                    item.cross_offset = cross_cursor + (line.cross_size - item.cross) / 2;
                    break;

                case Align::Stretch:
                    item.cross_offset = cross_cursor;
                    // Stretch the cross dimension to fill the line — UNLESS
                    // the container is a scroll viewport, where children must
                    // keep their natural cross size so a wider inner row can
                    // be horizontally scrolled into view. overflow=Hidden
                    // (plain clip, no scroll state) DOES want stretch so that
                    // bordered cards with align_items=Stretch behave like
                    // every other CSS flex parent.
                    if (style.overflow != Overflow::Scroll) {
                        item.cross = line.cross_size;
                        if (row) {
                            nodes[item.index].computed.size.height = Rows{item.cross};
                        } else {
                            nodes[item.index].computed.size.width = Columns{item.cross};
                        }
                    }
                    break;

                // SpaceBetween/Around/Evenly are not standard for align_items
                // per-item; treat them as Start.
                case Align::SpaceBetween:
                case Align::SpaceAround:
                case Align::SpaceEvenly:
                    item.cross_offset = cross_cursor;
                    break;
            }
        }

        cross_cursor += line.cross_size;
    }

    // ------------------------------------------------------------------
    // 3f. Write computed rects for children (position relative to parent content box)
    // ------------------------------------------------------------------
    int content_start_x = inner_left(style);
    int content_start_y = inner_top(style);

    for (auto& line : active_lines) {
        for (auto& item : line.items) {
            auto& child = nodes[item.index];
            const auto& cs = child.style;

            int cx, cy;
            if (row) {
                cx = content_start_x + item.main_offset + cs.margin.left;
                cy = content_start_y + item.cross_offset + cs.margin.top;
            } else {
                cx = content_start_x + item.cross_offset + cs.margin.left;
                cy = content_start_y + item.main_offset + cs.margin.top;
            }

            child.computed.pos = {Columns{cx}, Rows{cy}};

            // Update sizes to account for stretch or final resolved values
            if (row) {
                child.computed.size.width  = Columns{item.main};
                child.computed.size.height = Rows{item.cross};
            } else {
                child.computed.size.width  = Columns{item.cross};
                child.computed.size.height = Rows{item.main};
            }
        }
    }

    // ------------------------------------------------------------------
    // 4. Determine this node's own size
    // ------------------------------------------------------------------
    // If width or height was auto, compute from children.
    int final_w, final_h;

    if (def_w >= 0) {
        final_w = def_w;
    } else {
        if (row) {
            // Widest line determines content width
            int max_line = 0;
            for (auto& line : active_lines) max_line = std::max(max_line, line.main_size);
            final_w = max_line + inner_horizontal(style);
        } else {
            int max_cross = 0;
            for (auto& line : active_lines) max_cross = std::max(max_cross, line.cross_size);
            final_w = max_cross + inner_horizontal(style);
        }
        final_w = clamp_dim(final_w, style.min_width, style.max_width, parent_width);
    }

    if (def_h >= 0) {
        final_h = def_h;
    } else {
        if (row) {
            int total_c = 0;
            for (auto& line : active_lines) total_c += line.cross_size;
            final_h = total_c + inner_vertical(style);
        } else {
            int max_main = 0;
            for (auto& line : active_lines) max_main = std::max(max_main, line.main_size);
            final_h = max_main + inner_vertical(style);
        }
        final_h = clamp_dim(final_h, style.min_height, style.max_height, parent_height);
    }

    node.computed.size = {Columns{final_w}, Rows{final_h}};
}

} // namespace detail

// ============================================================================
// Public API
// ============================================================================

void compute(
    std::vector<LayoutNode>& nodes,
    std::size_t root,
    int available_width,
    int available_height)
{
    if (root >= nodes.size()) return;

    detail::compute_node(nodes, root, available_width, available_height,
                         available_width, available_height);

    // The root node's position is always at origin.
    auto& rn = nodes[root];
    int mx = rn.style.margin.left;
    int my = rn.style.margin.top;
    rn.computed.pos = {Columns{mx}, Rows{my}};
}

void resolve_absolute(
    std::vector<LayoutNode>& nodes,
    std::size_t idx,
    int offset_x,
    int offset_y)
{
    if (idx >= nodes.size()) return;
    auto& node = nodes[idx];

    node.computed.pos.x = Columns{node.computed.pos.x.value + offset_x};
    node.computed.pos.y = Rows{node.computed.pos.y.value + offset_y};

    for (std::size_t ci : node.children) {
        resolve_absolute(nodes, ci, node.computed.pos.x.value, node.computed.pos.y.value);
    }
}

} // namespace maya::layout
