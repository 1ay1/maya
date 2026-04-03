#pragma once
// maya::render::renderer - Tree walker that renders an element tree to a canvas
//
// The renderer bridges the element/layout world and the canvas/cell world.
// It walks the element tree, runs layout to compute positions and sizes,
// then paints each element's visual representation onto the canvas:
//   - BoxElement: border lines, background fill, recursive children
//   - TextElement: styled text with word-wrap/truncation
//   - ElementList: transparent recursion into fragment children
//
// Clip regions are pushed for overflow:hidden/scroll containers so that
// children that extend beyond the box boundary are silently discarded.

#include <cstddef>
#include <string>
#include <vector>

#include "canvas.hpp"
#include "../core/types.hpp"
#include "../element/element.hpp"
#include "../layout/yoga.hpp"
#include "../style/border.hpp"
#include "../style/style.hpp"
#include "../style/theme.hpp"

namespace maya {

namespace render_detail {

// ============================================================================
// Layout tree builder
// ============================================================================
// Recursively converts an Element tree into a flat vector of LayoutNodes
// suitable for the layout::compute() algorithm. Returns the index of the
// root node it created.

inline std::size_t build_layout_tree(
    const Element& elem,
    std::vector<layout::LayoutNode>& nodes,
    const Theme& /*theme*/)
{
    return visit_element(elem, [&](const auto& node) -> std::size_t {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::same_as<T, BoxElement>) {
            std::size_t idx = nodes.size();
            nodes.emplace_back();
            auto& ln = nodes[idx];

            // Map BoxElement's FlexStyle to layout::FlexStyle.
            auto& ls = ln.style;

            // Explicit mapping -- enum orderings differ between element and layout.
            auto map_dir = [](FlexDirection d) -> layout::FlexDirection {
                switch (d) {
                    case FlexDirection::Row:           return layout::FlexDirection::Row;
                    case FlexDirection::Column:        return layout::FlexDirection::Column;
                    case FlexDirection::RowReverse:    return layout::FlexDirection::RowReverse;
                    case FlexDirection::ColumnReverse: return layout::FlexDirection::ColumnReverse;
                }
                return layout::FlexDirection::Column;
            };
            auto map_wrap = [](FlexWrap w) -> layout::FlexWrap {
                switch (w) {
                    case FlexWrap::NoWrap:      return layout::FlexWrap::NoWrap;
                    case FlexWrap::Wrap:        return layout::FlexWrap::Wrap;
                    case FlexWrap::WrapReverse: return layout::FlexWrap::WrapReverse;
                }
                return layout::FlexWrap::NoWrap;
            };

            ls.direction = map_dir(node.layout.direction);
            ls.wrap      = map_wrap(node.layout.wrap);

            // Map alignment enums (the element enums may differ from layout enums).
            auto map_align = [](Align a) -> layout::Align {
                switch (a) {
                    case Align::Start:    return layout::Align::Start;
                    case Align::Center:   return layout::Align::Center;
                    case Align::End:      return layout::Align::End;
                    case Align::Stretch:  return layout::Align::Stretch;
                    case Align::Baseline: return layout::Align::Start; // fallback
                }
                return layout::Align::Start;
            };

            auto map_justify = [](Justify j) -> layout::Justify {
                switch (j) {
                    case Justify::Start:        return layout::Justify::Start;
                    case Justify::Center:       return layout::Justify::Center;
                    case Justify::End:          return layout::Justify::End;
                    case Justify::SpaceBetween: return layout::Justify::SpaceBetween;
                    case Justify::SpaceAround:  return layout::Justify::SpaceAround;
                    case Justify::SpaceEvenly:  return layout::Justify::SpaceEvenly;
                }
                return layout::Justify::Start;
            };

            ls.align_items     = map_align(node.layout.align_items);
            ls.align_self      = map_align(node.layout.align_self);
            ls.justify_content = map_justify(node.layout.justify);
            ls.flex_grow       = node.layout.grow;
            ls.flex_shrink     = node.layout.shrink;
            ls.flex_basis      = node.layout.basis;
            ls.width           = node.layout.width;
            ls.height          = node.layout.height;
            ls.min_width       = node.layout.min_width;
            ls.min_height      = node.layout.min_height;
            ls.max_width       = node.layout.max_width;
            ls.max_height      = node.layout.max_height;
            ls.gap             = node.layout.gap;
            ls.padding         = node.layout.padding;
            ls.margin          = node.layout.margin;

            // Border consumes 1 cell per visible side.
            if (node.has_border()) {
                ls.border = Edges<int>{
                    node.border.sides.top    ? 1 : 0,
                    node.border.sides.right  ? 1 : 0,
                    node.border.sides.bottom ? 1 : 0,
                    node.border.sides.left   ? 1 : 0,
                };
            }

            ls.overflow = static_cast<layout::Overflow>(
                static_cast<uint8_t>(node.overflow));

            // Recursively build children.
            for (const auto& child : node.children) {
                std::size_t child_idx = build_layout_tree(child, nodes, /*theme=*/{});
                nodes[idx].children.push_back(child_idx);
            }

            return idx;
        }
        else if constexpr (std::same_as<T, TextElement>) {
            std::size_t idx = nodes.size();
            nodes.emplace_back();
            auto& ln = nodes[idx];

            // Text is a leaf node with a measure function.
            // Use a lightweight function pointer + context instead of std::function.
            ln.measure = layout::MeasureFn{
                [](const void* ctx, int max_width) -> Size {
                    return static_cast<const TextElement*>(ctx)->measure(max_width);
                },
                &node
            };

            return idx;
        }
        else if constexpr (std::same_as<T, ElementList>) {
            // Fragments are transparent: wrap in an anonymous flex container
            // so that layout has a single root node.
            std::size_t idx = nodes.size();
            nodes.emplace_back();

            for (const auto& child : node.items) {
                std::size_t child_idx = build_layout_tree(child, nodes, /*theme=*/{});
                nodes[idx].children.push_back(child_idx);
            }

            return idx;
        }
        else {
            // Unknown element type -- create a zero-size placeholder.
            std::size_t idx = nodes.size();
            nodes.emplace_back();
            return idx;
        }
    });
}

// ============================================================================
// Painting helpers
// ============================================================================

/// Render a box's border onto the canvas.
/// Uses canvas.set() with char32_t codepoints directly instead of write_text()
/// to avoid per-character UTF-8 decode overhead on every border cell.
inline void paint_border(
    Canvas& canvas,
    const BorderConfig& border,
    const Rect& rect,
    uint16_t style_id)
{
    if (border.empty()) return;

    auto cp = get_border_codepoints(border.style);
    int x0 = rect.left().value;
    int y0 = rect.top().value;
    int x1 = rect.right().value - 1;  // inclusive right
    int y1 = rect.bottom().value - 1; // inclusive bottom

    if (x0 > x1 || y0 > y1) return;

    // Corners — direct codepoint write, no UTF-8 round-trip.
    if (border.sides.top && border.sides.left)
        canvas.set(x0, y0, cp.top_left, style_id);
    if (border.sides.top && border.sides.right)
        canvas.set(x1, y0, cp.top_right, style_id);
    if (border.sides.bottom && border.sides.left)
        canvas.set(x0, y1, cp.bottom_left, style_id);
    if (border.sides.bottom && border.sides.right)
        canvas.set(x1, y1, cp.bottom_right, style_id);

    // Top edge.
    if (border.sides.top) {
        for (int x = x0 + 1; x < x1; ++x)
            canvas.set(x, y0, cp.top, style_id);
    }

    // Bottom edge.
    if (border.sides.bottom) {
        for (int x = x0 + 1; x < x1; ++x)
            canvas.set(x, y1, cp.bottom, style_id);
    }

    // Left edge.
    if (border.sides.left) {
        for (int y = y0 + 1; y < y1; ++y)
            canvas.set(x0, y, cp.left, style_id);
    }

    // Right edge.
    if (border.sides.right) {
        for (int y = y0 + 1; y < y1; ++y)
            canvas.set(x1, y, cp.right, style_id);
    }

    // Border title text (still uses write_text — rare path, variable-length string).
    if (border.text.has_value() && !border.text->content.empty()) {
        const auto& bt = *border.text;
        int edge_y = (bt.position == BorderTextPos::Top) ? y0 : y1;
        int avail = x1 - x0 - 1;
        if (avail <= 0) return;

        int text_width = string_width(bt.content);
        int display_width = std::min(text_width, avail);

        int text_x;
        switch (bt.align) {
            case BorderTextAlign::Start:
                text_x = x0 + 1 + bt.offset;
                break;
            case BorderTextAlign::Center:
                text_x = x0 + 1 + (avail - display_width) / 2 + bt.offset;
                break;
            case BorderTextAlign::End:
                text_x = x1 - display_width + bt.offset;
                break;
        }

        text_x = std::clamp(text_x, x0 + 1, x1 - 1);
        canvas.write_text(text_x, edge_y, bt.content, style_id);
    }
}

// ============================================================================
// Recursive element painter
// ============================================================================

inline void paint_element(
    const Element& elem,
    Canvas& canvas,
    StylePool& pool,
    const std::vector<layout::LayoutNode>& layout_nodes,
    std::size_t layout_idx,
    int offset_x,
    int offset_y)
{
    const auto& ln = layout_nodes[layout_idx];
    const Rect& computed = ln.computed;

    // Absolute position on canvas.
    int ax = offset_x + computed.pos.x.value;
    int ay = offset_y + computed.pos.y.value;
    int aw = computed.size.width.value;
    int ah = computed.size.height.value;

    Rect abs_rect{{Columns{ax}, Rows{ay}}, {Columns{aw}, Rows{ah}}};

    visit_element(elem, [&](const auto& node) {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::same_as<T, BoxElement>) {
            // 1. Fill background if the box has a bg color.
            if (node.style.bg.has_value()) {
                uint16_t bg_style_id = pool.intern(
                    Style{}.with_bg(*node.style.bg));
                canvas.fill(abs_rect, U' ', bg_style_id);
            }

            // 2. Draw border.
            if (node.has_border()) {
                Style border_style = node.style;
                // If border has a color override, apply it.
                if (node.border.colors.top.has_value()) {
                    border_style = border_style.with_fg(*node.border.colors.top);
                }
                uint16_t border_style_id = pool.intern(border_style);
                paint_border(canvas, node.border, abs_rect, border_style_id);
            }

            // 3. Push clip for overflow:hidden/scroll.
            bool clipping = (node.overflow == Overflow::Hidden ||
                             node.overflow == Overflow::Scroll);

            int content_x = ax + (node.border.sides.left ? 1 : 0) + node.layout.padding.left;
            int content_y = ay + (node.border.sides.top ? 1 : 0) + node.layout.padding.top;
            int content_w = std::max(0, aw - node.inner_horizontal());
            int content_h = std::max(0, ah - node.inner_vertical());

            if (clipping) {
                canvas.push_clip(Rect{
                    {Columns{content_x}, Rows{content_y}},
                    {Columns{content_w}, Rows{content_h}}
                });
            }

            // 4. Recurse into children.
            std::size_t child_layout = 0;
            for (std::size_t i = 0; i < node.children.size(); ++i) {
                if (child_layout < ln.children.size()) {
                    paint_element(
                        node.children[i],
                        canvas,
                        pool,
                        layout_nodes,
                        ln.children[child_layout],
                        ax,
                        ay);
                    ++child_layout;
                }
            }

            if (clipping) {
                canvas.pop_clip();
            }
        }
        else if constexpr (std::same_as<T, TextElement>) {
            // Render formatted text lines at the computed position.
            uint16_t style_id = pool.intern(node.style);
            auto lines = node.format(aw);

            for (std::size_t i = 0; i < lines.size() && static_cast<int>(i) < ah; ++i) {
                canvas.write_text(ax, ay + static_cast<int>(i), lines[i], style_id);
            }
        }
        else if constexpr (std::same_as<T, ElementList>) {
            // Fragment: recurse into each child with its own layout node.
            for (std::size_t i = 0; i < node.items.size() && i < ln.children.size(); ++i) {
                paint_element(
                    node.items[i],
                    canvas,
                    pool,
                    layout_nodes,
                    ln.children[i],
                    ax,
                    ay);
            }
        }
    });
}

} // namespace render_detail

// ============================================================================
// render_tree - Public API: render an element tree onto a canvas
// ============================================================================
// 1. Builds a layout tree from the element tree.
// 2. Runs the flexbox layout algorithm (layout::compute).
// 3. Resolves all positions to absolute coordinates.
// 4. Paints each element onto the canvas.

inline void render_tree(
    const Element& root,
    Canvas& canvas,
    StylePool& pool,
    [[maybe_unused]] const Theme& theme)
{
    // Phase 1: Build the layout tree.
    // Reserve 128 slots upfront — a typical TUI element tree has 20-80 nodes.
    // Over-reserving once is cheaper than repeated realloc during tree walk.
    std::vector<layout::LayoutNode> layout_nodes;
    layout_nodes.reserve(128);

    std::size_t root_idx = render_detail::build_layout_tree(root, layout_nodes, theme);

    // Phase 2: Constrain root to terminal dimensions (like Ink's setWidth).
    layout_nodes[root_idx].style.width  = Dimension::fixed(canvas.width());
    layout_nodes[root_idx].style.height = Dimension::fixed(canvas.height());

    // Phase 3: Run layout. Positions are parent-relative after this.
    layout::compute(layout_nodes, root_idx, canvas.width(), canvas.height());

    // NOTE: We do NOT call resolve_absolute() because paint_element
    // propagates offsets during the recursive walk. Calling both would
    // double-count parent positions.

    // Phase 4: Paint to canvas.
    canvas.clear();
    render_detail::paint_element(
        root, canvas, pool, layout_nodes, root_idx,
        /*offset_x=*/0, /*offset_y=*/0);
}

} // namespace maya
