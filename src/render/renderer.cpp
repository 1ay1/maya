#include "maya/render/renderer.hpp"

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <string>
#include <vector>

#include "maya/core/overload.hpp"
#include "maya/core/render_context.hpp"

namespace maya {

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
            // For stack boxes, only the first child participates in layout;
            // overlay children are painted on top during the paint phase.
            std::size_t child_limit = node.is_stack && !node.children.empty()
                                        ? 1 : node.children.size();
            for (std::size_t ci = 0; ci < child_limit; ++ci) {
                std::size_t child_idx = build_layout_tree(node.children[ci], nodes, /*theme=*/{});
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

            if (node.measure) {
                // Custom measure: lets the component report its content
                // size so it sizes correctly in auto_height layouts.
                ln.measure = layout::MeasureFn{
                    [](const void* ctx, int max_width) -> Size {
                        auto& fn = *static_cast<const std::function<Size(int)>*>(ctx);
                        return fn(max_width);
                    },
                    &node.measure
                };
            } else {
                // Default: return available width × 1 row minimum.
                // flex-grow can expand from there.
                ln.measure = layout::MeasureFn{
                    [](const void*, int max_width) -> Size {
                        return {Columns{max_width}, Rows{1}};
                    },
                    nullptr
                };
            }

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

        int text_x = [&] {
            switch (bt.align) {
                case BorderTextAlign::Start:
                    return x0 + 1 + bt.offset;
                case BorderTextAlign::Center:
                    return x0 + 1 + (avail - display_width) / 2 + bt.offset;
                case BorderTextAlign::End:
                    return x1 - display_width + bt.offset;
            }
            std::unreachable();
        }();

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

            bool has_b = node.has_border();
            int content_x = ax + (has_b && node.border.sides.left ? 1 : 0) + node.layout.padding.left;
            int content_y = ay + (has_b && node.border.sides.top ? 1 : 0) + node.layout.padding.top;
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

            // 5. Stack overlays: children beyond index 0 are painted on top
            //    using independent sub-layout trees, clipped to the same region.
            if (node.is_stack && node.children.size() > 1) {
                for (std::size_t oi = 1; oi < node.children.size(); ++oi) {
                    const auto& overlay = node.children[oi];

                    thread_local std::vector<layout::LayoutNode> overlay_nodes;
                    thread_local bool overlay_in_use = false;
                    auto was_in_use = overlay_in_use;
                    std::vector<layout::LayoutNode> local_overlay;
                    auto& sub = was_in_use ? local_overlay : overlay_nodes;
                    sub.clear();
                    overlay_in_use = true;

                    std::size_t sub_root = build_layout_tree(overlay, sub, {});
                    sub[sub_root].style.width  = Dimension::fixed(content_w);
                    sub[sub_root].style.height = Dimension::fixed(content_h);
                    layout::compute(sub, sub_root, content_w, content_h);

                    canvas.push_clip(Rect{
                        {Columns{content_x}, Rows{content_y}},
                        {Columns{content_w}, Rows{content_h}}
                    });
                    paint_element(overlay, canvas, pool, sub, sub_root,
                                  content_x, content_y);
                    canvas.pop_clip();

                    if (!was_in_use) overlay_in_use = false;
                }
            }

            if (clipping) {
                canvas.pop_clip();
            }
        },

        [&](const TextElement& node) {
            const auto& lines = node.format(aw);

            if (node.runs.empty()) {
                // Fast path: single style for the whole element.
                // Hand-rolled index instead of `std::views::enumerate` —
                // libc++ on Android NDK / Termux doesn't ship the C++23
                // `enumerate` view yet (clang accepts the syntax but the
                // headers don't define the symbol). The manual loop is
                // identical in behaviour and runs everywhere our other
                // C++23 features compile.
                uint16_t style_id = pool.intern(node.style);
                const std::size_t cap =
                    std::min<std::size_t>(static_cast<std::size_t>(ah), lines.size());
                for (std::size_t row = 0; row < cap; ++row) {
                    canvas.write_text(ax, ay + static_cast<int>(row),
                                      lines[row], style_id);
                }
            } else {
                // Styled runs: paint each character segment with its own style.
                // Track byte offset into `content` as we walk wrapped lines.
                std::size_t content_byte = 0;
                std::size_t run_idx = 0;

                const std::size_t cap =
                    std::min<std::size_t>(static_cast<std::size_t>(ah), lines.size());
                for (std::size_t row = 0; row < cap; ++row) {
                    const auto& line = lines[row];
                    int y = ay + static_cast<int>(row);

                    // Skip whitespace that word_wrap consumed between lines.
                    // word_wrap may skip leading spaces on continuation lines,
                    // and the source line has a newline we need to skip.
                    while (content_byte < node.content.size() &&
                           node.content.substr(content_byte, line.size()) != line) {
                        ++content_byte;
                    }

                    // Paint this line character by character with correct styles
                    int x_cursor = ax;
                    std::size_t line_byte = 0;
                    while (line_byte < line.size()) {
                        // Find which run covers content_byte + line_byte
                        std::size_t abs_byte = content_byte + line_byte;
                        while (run_idx + 1 < node.runs.size() &&
                               abs_byte >= node.runs[run_idx].byte_offset +
                                           node.runs[run_idx].byte_length) {
                            ++run_idx;
                        }

                        // Determine how many bytes of this run remain on this line
                        const auto& run = node.runs[std::min(run_idx, node.runs.size() - 1)];
                        std::size_t run_end = run.byte_offset + run.byte_length;
                        std::size_t chunk_end;
                        if (run_end > abs_byte) {
                            chunk_end = std::min(line.size(), line_byte + (run_end - abs_byte));
                        } else {
                            chunk_end = line_byte + 1;
                        }
                        if (chunk_end <= line_byte) chunk_end = line_byte + 1;

                        auto chunk = line.substr(line_byte, chunk_end - line_byte);
                        uint16_t sid = pool.intern(run.style);
                        canvas.write_text(x_cursor, y, chunk, sid);
                        x_cursor += string_width(chunk);
                        line_byte = chunk_end;
                    }

                    content_byte += line.size();
                }
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

            // Reuse a thread-local scratch buffer for the sub-layout tree.
            // This avoids a heap allocation per component per frame — typical
            // apps have 10-50 components, so this saves 10-50 allocations/frame.
            // Guard against recursion: if a component renders another component,
            // the inner call gets its own local vector to avoid clobbering.
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

            canvas.push_clip(Rect{
                {Columns{content_x}, Rows{content_y}},
                {Columns{content_w}, Rows{content_h}}
            });
            paint_element(child, canvas, pool, sub_nodes, sub_root,
                          content_x, content_y);
            canvas.pop_clip();
            if (!use_local) tl_in_use = false;
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
    // Set the render context so widgets can query available_width() etc.
    // If a parent context exists (e.g. from App::render_frame), this is
    // a no-op override with the same values; if called standalone (tests,
    // one-shot prints), this provides the correct canvas dimensions.
    RenderContext ctx{canvas.width(), canvas.height(), render_generation(), auto_height};
    RenderContextGuard guard(ctx);

    std::vector<layout::LayoutNode> layout_nodes;
    layout_nodes.reserve(128);
    render_tree(root, canvas, pool, theme, layout_nodes, auto_height);
}

void render_tree(
    const Element& root,
    Canvas& canvas,
    StylePool& pool,
    [[maybe_unused]] const Theme& theme,
    std::vector<layout::LayoutNode>& layout_nodes,
    bool auto_height)
{
    // Set the render context if not already set by the parent overload or App.
    RenderContext ctx{canvas.width(), canvas.height(), render_generation(), auto_height};
    RenderContextGuard guard(ctx);

    // Phase 1: Build the layout tree (reusing the caller's vector).
    layout_nodes.clear();

    std::size_t root_idx = render_detail::build_layout_tree(root, layout_nodes, theme);

    // Phase 2: Constrain root to terminal dimensions.
    layout_nodes[root_idx].style.width  = Dimension::fixed(canvas.width());
    if (!auto_height)
        layout_nodes[root_idx].style.height = Dimension::fixed(canvas.height());

    // Phase 3: Run layout. Positions are parent-relative after this.
    layout::compute(layout_nodes, root_idx, canvas.width(), canvas.height());

    // Phase 4: Paint to canvas.
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

    // Phase 1: Build the layout tree (reuse thread-local to avoid per-call alloc).
    thread_local std::vector<layout::LayoutNode> layout_nodes;
    layout_nodes.clear();
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
