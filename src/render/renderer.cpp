#include "maya/render/renderer.hpp"

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <string>
#include <vector>

namespace maya {

// ============================================================================
// Overload set helper for std::visit dispatch
// ============================================================================

template <typename... Fs>
struct overload : Fs... { using Fs::operator()...; };

namespace render_detail {

// ============================================================================
// Stateless enum mappers (anonymous namespace — internal linkage)
// ============================================================================

namespace {

constexpr layout::FlexDirection map_dir(FlexDirection d) noexcept {
    switch (d) {
        case FlexDirection::Row:           return layout::FlexDirection::Row;
        case FlexDirection::Column:        return layout::FlexDirection::Column;
        case FlexDirection::RowReverse:    return layout::FlexDirection::RowReverse;
        case FlexDirection::ColumnReverse: return layout::FlexDirection::ColumnReverse;
    }
    return layout::FlexDirection::Column;
}

constexpr layout::FlexWrap map_wrap(FlexWrap w) noexcept {
    switch (w) {
        case FlexWrap::NoWrap:      return layout::FlexWrap::NoWrap;
        case FlexWrap::Wrap:        return layout::FlexWrap::Wrap;
        case FlexWrap::WrapReverse: return layout::FlexWrap::WrapReverse;
    }
    return layout::FlexWrap::NoWrap;
}

constexpr layout::Align map_align(Align a) noexcept {
    switch (a) {
        case Align::Start:    return layout::Align::Start;
        case Align::Center:   return layout::Align::Center;
        case Align::End:      return layout::Align::End;
        case Align::Stretch:  return layout::Align::Stretch;
        case Align::Baseline: return layout::Align::Start; // fallback
    }
    return layout::Align::Start;
}

constexpr layout::Justify map_justify(Justify j) noexcept {
    switch (j) {
        case Justify::Start:        return layout::Justify::Start;
        case Justify::Center:       return layout::Justify::Center;
        case Justify::End:          return layout::Justify::End;
        case Justify::SpaceBetween: return layout::Justify::SpaceBetween;
        case Justify::SpaceAround:  return layout::Justify::SpaceAround;
        case Justify::SpaceEvenly:  return layout::Justify::SpaceEvenly;
    }
    return layout::Justify::Start;
}

} // anonymous namespace

// ============================================================================
// Layout tree builder
// ============================================================================

std::size_t build_layout_tree(
    const Element& elem,
    std::vector<layout::LayoutNode>& nodes,
    const Theme& /*theme*/)
{
    return visit_element(elem, overload{
        [&](const BoxElement& node) -> std::size_t {
            std::size_t idx = nodes.size();
            nodes.emplace_back();
            auto& ls = nodes[idx].style;

            ls.direction       = map_dir(node.layout.direction);
            ls.wrap            = map_wrap(node.layout.wrap);
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
        },

        [&](const TextElement& node) -> std::size_t {
            std::size_t idx = nodes.size();
            nodes.emplace_back();
            auto& ln = nodes[idx];

            // Text is a leaf node with a measure function.
            ln.measure = layout::MeasureFn{
                [](const void* ctx, int max_width) -> Size {
                    return static_cast<const TextElement*>(ctx)->measure(max_width);
                },
                &node
            };

            return idx;
        },

        [&](const ElementList& node) -> std::size_t {
            // Fragments are transparent: wrap in an anonymous flex container
            // so that layout has a single root node.
            std::size_t idx = nodes.size();
            nodes.emplace_back();

            for (const auto& child : node.items) {
                std::size_t child_idx = build_layout_tree(child, nodes, /*theme=*/{});
                nodes[idx].children.push_back(child_idx);
            }

            return idx;
        },

        [&](const ComponentElement& node) -> std::size_t {
            // Component: a leaf that defers rendering to paint time.
            // Participates in flex layout via its FlexStyle properties.
            // Has a measure function returning 1×1 so the layout engine
            // gives it nonzero size even without explicit dimensions.
            std::size_t idx = nodes.size();
            nodes.emplace_back();
            auto& ln = nodes[idx];
            auto& ls = ln.style;

            ls.flex_grow   = node.layout.grow;
            ls.flex_shrink = node.layout.shrink;
            ls.flex_basis  = node.layout.basis;
            ls.width       = node.layout.width;
            ls.height      = node.layout.height;
            ls.min_width   = node.layout.min_width;
            ls.min_height  = node.layout.min_height;
            ls.max_width   = node.layout.max_width;
            ls.max_height  = node.layout.max_height;
            ls.padding     = node.layout.padding;
            ls.margin      = node.layout.margin;
            ls.align_self  = map_align(node.layout.align_self);

            // Measure: return available width × 1 row minimum.
            // This ensures the component gets at least 1 row and the
            // full cross-axis width, so flex-grow can expand from there.
            ln.measure = layout::MeasureFn{
                [](const void*, int max_width) -> Size {
                    return {Columns{max_width}, Rows{1}};
                },
                nullptr
            };

            return idx;
        }
    });
}

// ============================================================================
// Border painting
// ============================================================================

void paint_border(
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

void paint_element(
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

    visit_element(elem, overload{
        [&](const BoxElement& node) {
            // 1. Fill background if the box has a bg color.
            if (node.style.bg.has_value()) {
                uint16_t bg_style_id = pool.intern(
                    Style{}.with_bg(*node.style.bg));
                canvas.fill(abs_rect, U' ', bg_style_id);
            }

            // 2. Draw border.
            if (node.has_border()) {
                Style border_style = node.style;
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

            // 4. Recurse into children, pairing each child element with its layout index.
            for (const auto& [child, child_layout_idx] :
                     std::views::zip(node.children, ln.children)) {
                paint_element(
                    child,
                    canvas,
                    pool,
                    layout_nodes,
                    child_layout_idx,
                    ax,
                    ay);
            }

            if (clipping) {
                canvas.pop_clip();
            }
        },

        [&](const TextElement& node) {
            // Render formatted text lines at the computed position.
            uint16_t style_id = pool.intern(node.style);
            auto lines = node.format(aw);

            for (const auto& [row, line] :
                     lines | std::views::enumerate | std::views::take(ah)) {
                canvas.write_text(ax, ay + static_cast<int>(row), line, style_id);
            }
        },

        [&](const ElementList& node) {
            // Fragment: recurse into each child with its own layout node.
            for (const auto& [child, child_layout_idx] :
                     std::views::zip(node.items, ln.children)) {
                paint_element(
                    child,
                    canvas,
                    pool,
                    layout_nodes,
                    child_layout_idx,
                    ax,
                    ay);
            }
        },

        [&](const ComponentElement& node) {
            // Lazy component: call the render callback with the allocated size,
            // then render the resulting element tree into this region.
            if (!node.render) return;

            int content_w = std::max(0, aw - static_cast<int>(node.layout.padding.horizontal()));
            int content_h = std::max(0, ah - static_cast<int>(node.layout.padding.vertical()));
            int content_x = ax + node.layout.padding.left;
            int content_y = ay + node.layout.padding.top;

            Element child = node.render(content_w, content_h);

            // Build a sub-layout tree for the generated element.
            std::vector<layout::LayoutNode> sub_nodes;
            sub_nodes.reserve(64);
            std::size_t sub_root = build_layout_tree(child, sub_nodes, {});

            sub_nodes[sub_root].style.width = Dimension::fixed(content_w);
            sub_nodes[sub_root].style.height = Dimension::fixed(content_h);
            layout::compute(sub_nodes, sub_root, content_w, content_h);

            canvas.push_clip(Rect{
                {Columns{content_x}, Rows{content_y}},
                {Columns{content_w}, Rows{content_h}}
            });
            paint_element(child, canvas, pool, sub_nodes, sub_root,
                          content_x, content_y);
            canvas.pop_clip();
        }
    });
}

} // namespace render_detail

void render_tree(
    const Element& root,
    Canvas& canvas,
    StylePool& pool,
    [[maybe_unused]] const Theme& theme,
    bool auto_height)
{
    // Phase 1: Build the layout tree.
    std::vector<layout::LayoutNode> layout_nodes;
    layout_nodes.reserve(128);

    std::size_t root_idx = render_detail::build_layout_tree(root, layout_nodes, theme);

    // Phase 2: Constrain root to terminal dimensions.
    // In auto_height mode (inline rendering), only constrain width so the
    // layout sizes to content height — preserving terminal scrollback.
    layout_nodes[root_idx].style.width  = Dimension::fixed(canvas.width());
    if (!auto_height)
        layout_nodes[root_idx].style.height = Dimension::fixed(canvas.height());

    // Phase 3: Run layout. Positions are parent-relative after this.
    layout::compute(layout_nodes, root_idx, canvas.width(), canvas.height());

    // NOTE: We do NOT call resolve_absolute() because paint_element
    // propagates offsets during the recursive walk. Calling both would
    // double-count parent positions.

    // Phase 4: Paint to canvas.
    // NOTE: caller is responsible for clearing the canvas before calling
    // render_tree(). The pipeline's clear() step and App::render_frame()
    // both do this. We do NOT clear here to avoid a redundant SIMD fill.
    render_detail::paint_element(
        root, canvas, pool, layout_nodes, root_idx,
        /*offset_x=*/0, /*offset_y=*/0);
}

void render_tree_at(
    const Element& root,
    Canvas& canvas,
    StylePool& pool,
    const Theme& theme,
    int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;

    // Phase 1: Build the layout tree.
    std::vector<layout::LayoutNode> layout_nodes;
    layout_nodes.reserve(128);
    std::size_t root_idx = render_detail::build_layout_tree(root, layout_nodes, theme);

    // Phase 2: Constrain root to the sub-region dimensions.
    layout_nodes[root_idx].style.width  = Dimension::fixed(w);
    layout_nodes[root_idx].style.height = Dimension::fixed(h);

    // Phase 3: Run layout within the sub-region bounds.
    layout::compute(layout_nodes, root_idx, w, h);

    // Phase 4: Clip to the sub-region and paint with offset — no clear.
    canvas.push_clip(Rect{
        {Columns{x}, Rows{y}},
        {Columns{w}, Rows{h}}
    });
    render_detail::paint_element(
        root, canvas, pool, layout_nodes, root_idx,
        /*offset_x=*/x, /*offset_y=*/y);
    canvas.pop_clip();
}

} // namespace maya
