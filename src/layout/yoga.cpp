#include <maya/layout/yoga.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace maya::layout {

namespace detail {

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

    // Available space on main and cross axes for children
    int main_avail  = row ? content_w : content_h;
    int cross_avail = row ? content_h : content_w;

    // Whether the main dimension is definite (explicit or from flex-grow override).
    bool main_definite = row ? (def_w >= 0) : (def_h >= 0);

    // ------------------------------------------------------------------
    // 3a. Collect visible children and compute their hypothetical main size
    // ------------------------------------------------------------------
    std::vector<FlexItem> all_items;
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
        if (!basis.is_auto()) {
            hypo_main = basis.resolve(main_avail);
        } else if (child.measure) {
            // Measure leaf to get intrinsic size
            int measure_w = row ? main_avail : content_w;
            Size ms = child.measure(std::max(0, measure_w - (row ? inner_horizontal(cs) : inner_horizontal(cs))));
            hypo_main = row ? ms.width.value + inner_horizontal(cs) : ms.height.value + inner_vertical(cs);
        } else {
            // Recurse to determine child's natural size.
            int child_avail_w = row ? main_avail : content_w;
            int child_avail_h = row ? content_h  : main_avail;
            compute_node(nodes, ci, child_avail_w, child_avail_h, content_w, content_h);

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
        });
    }

    // ------------------------------------------------------------------
    // 3b. Break items into flex lines (wrapping support)
    // ------------------------------------------------------------------
    std::vector<FlexLine> lines;

    if (style.wrap == FlexWrap::NoWrap || all_items.empty()) {
        // Single line containing all items
        FlexLine line;
        line.items = std::move(all_items);
        lines.push_back(std::move(line));
    } else {
        // Multi-line wrapping
        FlexLine current;
        int line_main = 0;
        for (auto& item : all_items) {
            int needed = item.hypothetical + (current.items.empty() ? 0 : style.gap);
            if (!current.items.empty() && line_main + needed > main_avail) {
                lines.push_back(std::move(current));
                current = FlexLine{};
                line_main = 0;
            }
            line_main += item.hypothetical + (current.items.empty() ? 0 : style.gap);
            current.items.push_back(item);
        }
        if (!current.items.empty()) {
            lines.push_back(std::move(current));
        }
    }

    if (style.wrap == FlexWrap::WrapReverse) {
        std::ranges::reverse(lines);
    }

    // ------------------------------------------------------------------
    // 3c. Resolve flex-grow and flex-shrink within each line
    // ------------------------------------------------------------------
    for (auto& line : lines) {
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
        } else if (free_space < 0 && main_definite) {
            // Shrink overflowing items via flex_shrink
            float total_shrink_weighted = 0.0f;
            for (auto& it : line.items) {
                total_shrink_weighted +=
                    nodes[it.index].style.flex_shrink * static_cast<float>(it.hypothetical);
            }
            if (total_shrink_weighted > 0.0f) {
                int overflow = -free_space;
                int remaining = overflow;
                for (auto& it : line.items) {
                    float shrink = nodes[it.index].style.flex_shrink;
                    float weight = shrink * static_cast<float>(it.hypothetical);
                    int reduction = static_cast<int>(
                        static_cast<float>(overflow) * weight / total_shrink_weighted);
                    reduction = std::min(reduction, it.main); // don't go negative
                    it.main -= reduction;
                    remaining -= reduction;
                }
                // Distribute leftover shrink to the largest items first
                // to avoid destroying small items (e.g., a 1-row header).
                while (remaining > 0) {
                    int best = -1;
                    int best_main = 0;
                    for (int i = 0; i < static_cast<int>(line.items.size()); ++i) {
                        auto& it = line.items[static_cast<std::size_t>(i)];
                        if (nodes[it.index].style.flex_shrink > 0.0f && it.main > best_main) {
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
    for (auto& line : lines) {
        int line_cross = 0;
        for (auto& item : line.items) {
            auto& child = nodes[item.index];
            const auto& cs = child.style;

            int child_w, child_h;
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
                bool cross_definite = row ? (def_h >= 0) : (def_w >= 0);
                auto saved_w = cs.width;
                auto saved_h = cs.height;
                bool main_changed = (item.main != item.hypothetical);
                if (row) {
                    // Force width (main) only when grow/shrink changed it
                    // or it was already explicit — otherwise let the child
                    // recompute its intrinsic width at the resolved cross size.
                    if (main_changed || !saved_w.is_auto())
                        child.style.width = Dimension::fixed(child_w);
                    if (cross_definite || !saved_h.is_auto())
                        child.style.height = Dimension::fixed(child_h);
                } else {
                    // Force height (main) only when grow/shrink changed it
                    // or it was already explicit — auto-height children must
                    // recompute after their cross (width) is resolved, since
                    // narrower widths cause text to wrap to more lines.
                    if (main_changed || !saved_h.is_auto())
                        child.style.height = Dimension::fixed(child_h);
                    if (cross_definite || !saved_w.is_auto())
                        child.style.width = Dimension::fixed(child_w);
                }
                compute_node(nodes, item.index, child_w, child_h, content_w, content_h);
                child.style.width  = saved_w;
                child.style.height = saved_h;
                child_w = child.computed.size.width.value;
                child_h = child.computed.size.height.value;
            } else if (child.measure) {
                Size ms = child.measure(inner_w);
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
    if (lines.size() == 1 && cross_definite_for_line) {
        lines[0].cross_size = std::max(lines[0].cross_size, cross_avail);
    }

    // ------------------------------------------------------------------
    // 3e. Position children: justify-content along main, align on cross
    // ------------------------------------------------------------------
    int total_cross = 0;
    for (auto& line : lines) total_cross += line.cross_size;

    int cross_cursor = 0;
    for (auto& line : lines) {
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
                    // Stretch the cross dimension to fill the line
                    item.cross = line.cross_size;
                    if (row) {
                        nodes[item.index].computed.size.height = Rows{item.cross};
                    } else {
                        nodes[item.index].computed.size.width = Columns{item.cross};
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

    for (auto& line : lines) {
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
            for (auto& line : lines) max_line = std::max(max_line, line.main_size);
            final_w = max_line + inner_horizontal(style);
        } else {
            int max_cross = 0;
            for (auto& line : lines) max_cross = std::max(max_cross, line.cross_size);
            final_w = max_cross + inner_horizontal(style);
        }
        final_w = clamp_dim(final_w, style.min_width, style.max_width, parent_width);
    }

    if (def_h >= 0) {
        final_h = def_h;
    } else {
        if (row) {
            int total_c = 0;
            for (auto& line : lines) total_c += line.cross_size;
            final_h = total_c + inner_vertical(style);
        } else {
            int max_main = 0;
            for (auto& line : lines) max_main = std::max(max_main, line.main_size);
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
