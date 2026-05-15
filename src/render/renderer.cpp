#include "maya/render/renderer.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

#include "maya/core/overload.hpp"
#include "maya/core/render_context.hpp"
#include "maya/core/scroll_state.hpp"

namespace maya {

namespace render_detail {

// ============================================================================
// Cross-frame ComponentElement render cache
// ============================================================================
// Two purposes, scaling-superlinear in long sessions:
//
//   1. Within one frame, the auto-measure path (build_layout_tree →
//      measure callback → render) produces an Element tree that the
//      subsequent paint pass also needs.  Without a cache, paint would
//      call render() again — doubling render cost per component per frame.
//
//   2. Across frames, when the application uses dsl::list_ref(stable_vec)
//      the ComponentElement instances inside have stable addresses.
//      A pointer + width key lets us recognise the SAME logical
//      component frame after frame and skip render() entirely whenever
//      neither the address nor the width changed.  For a long
//      conversation with 100 frozen turn cards and 1 streaming turn,
//      the win is enormous: the 100 stable cards render() exactly once
//      ever (until something invalidates them); only the streaming
//      card pays the per-frame cost.
//
// Eviction policy: at the start of every top-level render_tree() call we
// drop entries whose last_frame_used predates the previous frame.  Each
// hit during measure / paint stamps last_frame_used to the current
// frame, so stable entries survive indefinitely while ephemeral ones
// (new pointer each frame) age out within one frame of last access.
//
// Why cache by pointer rather than by hash: pointer comparison is one
// instruction; hashing an Element's content tree is O(content) and would
// dominate the savings.  The application opts into the cross-frame
// behaviour by holding the ComponentElement in a stable vector
// (typically through dsl::list_ref); ephemeral usage falls back to
// the within-frame cache automatically.

namespace {

struct ComponentCacheEntry {
    int      width  = -1;         // width at which the result was rendered
    int      height = 0;          // measured natural height at that width
    Element  result;              // cached render() output
    std::uint64_t last_frame = 0; // bumped on every hit
    // Generation of the ComponentElement instance whose render() output
    // produced `result`. The pointer alone (the map key) is not a
    // stable identity — the allocator can reuse a freed
    // ComponentElement's address for a fresh, unrelated instance, and
    // a pointer-only cache would alias the new instance to the old's
    // cached render. Comparing generations on lookup catches that case
    // and treats it as a miss.
    std::uint64_t generation = 0;

    // Painted-cell cache: a (height × width) grid of packed cell
    // values captured the first time this entry is painted. On every
    // subsequent paint we blit these cells into the canvas instead of
    // re-running build_layout_tree + layout::compute + paint_element
    // over the cached Element. That walk was the residual O(child_size)
    // per-cached-turn cost the test suite isolated as ~1.2 us/turn —
    // blit collapses it to a memcpy per row.
    //
    // `cells` length is `width * cells_rows`. `cells_rows` is the
    // actual height we painted (≤ `height`; auto_height layouts can
    // produce slack rows we don't want to clobber on the host
    // canvas). `cells_max_y` records the highest row index that
    // carried non-blank content at population time — the blit only
    // calls into the canvas's max_y tracker for those rows.
    //
    // Empty `cells` ⇒ not yet populated (the entry was created by the
    // measure-phase miss path; paint-phase miss will populate). Width
    // mismatch already evicts via `find_component_cache`, so the
    // (width, cells) invariant stays consistent across resize.
    std::vector<std::uint64_t> cells;
    int cells_rows  = 0;
    int cells_max_y = -1;
};

struct ComponentCache {
    // Pointer-keyed cache: address-stable ComponentElements (typically
    // those held in a stable vector slot the host doesn't reallocate)
    // hit here. Pointer compare is one instruction so this is the
    // first-class hot path.
    std::unordered_map<const ComponentElement*, ComponentCacheEntry> entries;
    // Content-keyed cache: when a ComponentElement carries a non-empty
    // `cache_id`, lookups go through this map instead. This lets the
    // content survive value-copies through containers (e.g. a settled
    // turn Element pushed into a fresh per-frame vector by a widget's
    // build() function) — every copy presents the same id and finds
    // the same cache entry, even though &comp differs each frame.
    std::unordered_map<std::string, ComponentCacheEntry>             entries_by_id;
    std::uint64_t current_frame = 0;
};

inline ComponentCache& component_cache() {
    thread_local ComponentCache c;
    return c;
}

// Look up a ComponentElement in the cross-frame cache, preferring the
// content-keyed map when the component carries a non-empty cache_id.
// Returns nullptr on miss; caller stores via store_component_cache().
//
// Generation handling diverges between the two paths:
//   - Pointer keying: generation MUST match. The pointer alone is not
//     a stable identity (the allocator can recycle a freed
//     ComponentElement's address); the generation check rejects the
//     aliased entry.
//   - Content keying: generation is IGNORED. The whole point of
//     cache_id is that the host is promising "same id ⇒ same content"
//     — and the host gets there by constructing fresh
//     ComponentElement instances every frame (each with a brand-new
//     generation), so a generation match would never happen and the
//     cache would always miss. cache_id IS the cross-frame identity.
inline ComponentCacheEntry* find_component_cache(ComponentCache& cache,
                                                 const ComponentElement& comp,
                                                 int width) noexcept {
    if (!comp.cache_id.empty()) {
        auto it = cache.entries_by_id.find(comp.cache_id);
        if (it != cache.entries_by_id.end()
            && it->second.width == width) {
            return &it->second;
        }
        return nullptr;
    }
    auto it = cache.entries.find(&comp);
    if (it != cache.entries.end()
        && it->second.width      == width
        && it->second.generation == comp.generation) {
        return &it->second;
    }
    return nullptr;
}

// Insert / overwrite a cache entry under the appropriate key.
inline void store_component_cache(ComponentCache& cache,
                                  const ComponentElement& comp,
                                  ComponentCacheEntry entry) {
    if (!comp.cache_id.empty()) {
        cache.entries_by_id[comp.cache_id] = std::move(entry);
    } else {
        cache.entries[&comp] = std::move(entry);
    }
}

inline int& render_depth() {
    thread_local int d = 0;
    return d;
}

}  // anonymous

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
            // overflow propagates to layout so the shrink loop can opt out
            // when a container will clip its children (scroll viewports
            // need children to keep their natural sizes — the renderer
            // translates by scroll_x/y during paint).
            ls.overflow        = (node.overflow == Overflow::Hidden ? layout::Overflow::Hidden
                                : node.overflow == Overflow::Scroll ? layout::Overflow::Scroll
                                : layout::Overflow::Visible);

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

        [&](const ElementListRef& node) -> std::size_t {
            // Borrowed fragment — same shape as ElementList but reads
            // through a pointer instead of owning the vector. This is
            // the zero-copy path for stable application-side data
            // (model.frozen and similar).
            std::size_t idx = nodes.size();
            nodes.emplace_back();
            if (node.items_ref) {
                for (const auto& child : *node.items_ref) {
                    std::size_t child_idx = build_layout_tree(child, nodes, /*theme=*/{});
                    nodes[idx].children.push_back(child_idx);
                }
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
                // Legacy: caller-supplied measure callback. The widget
                // is responsible for keeping it in sync with render().
                // Drift between the two silently clips rows — see the
                // auto-measure fallback below for the contract-safe path.
                ln.measure = layout::MeasureFn{
                    [](const void* ctx, int max_width) -> Size {
                        auto& fn = *static_cast<const std::function<Size(int)>*>(ctx);
                        return fn(max_width);
                    },
                    &node.measure
                };
            } else if (node.render) {
                // Auto-measure: derive the natural size by RUNNING render
                // and measuring the result.  This makes measure/render
                // disagreement structurally impossible — there's only
                // one callback to write, and the framework guarantees
                // they're consistent because measure literally invokes
                // render and counts the rows.
                //
                // Cost: render() is called twice per frame (once during
                // layout, once during paint).  For pure-data components
                // (the common case) this is cheap.  Components with
                // expensive render can opt into the legacy `measure`
                // callback above for explicit caching.
                ln.measure = layout::MeasureFn{
                    [](const void* ctx, int max_width) -> Size {
                        auto& comp = *static_cast<const ComponentElement*>(ctx);
                        if (!comp.render || max_width <= 0) {
                            return {Columns{std::max(0, max_width)}, Rows{1}};
                        }
                        constexpr int kBigH = 1 << 20;

                        auto& cache = component_cache();
                        // Trust the cached height when:
                        //  - cache_id is set (host opted into cross-frame
                        //    identity), OR
                        //  - the entry was stored this frame (pointer-keyed
                        //    within-frame reuse: same render call satisfies
                        //    both layout::compute's hypothetical (3a) and
                        //    final (3d) measure passes — without this,
                        //    every nested component renders twice per
                        //    measure pass).
                        // Pointer-keyed entries CAN hold a stale `height`
                        // across frames (the wrapper instance is reused
                        // but its closure captured mutable state — e.g.
                        // StreamingMarkdown's prefix), so we reject
                        // cross-frame pointer-keyed HITs.
                        if (auto* entry = find_component_cache(cache, comp, max_width)) {
                            const bool trust =
                                !comp.cache_id.empty()
                                || entry->last_frame == cache.current_frame;
                            if (trust) {
                                entry->last_frame = cache.current_frame;
                                return {Columns{max_width},
                                        Rows{entry->height}};
                            }
                        }

                        // Miss (or pointer-keyed cross-frame: treated as
                        // miss). Render, layout, store. Pointer-keyed
                        // entries still get an entry stored so the
                        // paint slow-path can find the same `result`
                        // and avoid a second render() call within
                        // this frame — but the entry won't be honored
                        // across frames at the measure site.
                        Element child = comp.render(max_width, kBigH);

                        std::vector<layout::LayoutNode> tmp;
                        tmp.reserve(8);
                        std::size_t root = build_layout_tree(child, tmp, {});
                        tmp[root].style.width  = Dimension::fixed(max_width);
                        tmp[root].style.height = Dimension::auto_();
                        layout::compute(tmp, root, max_width, kBigH);
                        int h = std::max(1,
                            tmp[root].computed.size.height.raw());

                        store_component_cache(cache, comp, {
                            max_width,
                            h,
                            std::move(child),
                            cache.current_frame,
                            comp.generation
                        });
                        return {Columns{max_width}, Rows{h}};
                    },
                    &node
                };
            } else {
                // No render at all — degenerate placeholder.
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

            // 3. Push clip for overflow:hidden/scroll. RAII guard ensures
            //    the pop happens even if a child's paint throws — without
            //    this, a thrown paint callback would leave a stale clip
            //    on the stack and silently mangle subsequent rendering.
            bool clipping = (node.overflow == Overflow::Hidden ||
                             node.overflow == Overflow::Scroll);

            bool has_b = node.has_border();
            int content_x = ax + (has_b && node.border.sides.left ? 1 : 0) + node.layout.padding.left;
            int content_y = ay + (has_b && node.border.sides.top ? 1 : 0) + node.layout.padding.top;
            int content_w = std::max(0, aw - node.inner_horizontal());
            int content_h = std::max(0, ah - node.inner_vertical());

            std::optional<Canvas::ClipScope> guard;
            if (clipping) {
                guard.emplace(canvas, Rect{
                    {Columns{content_x}, Rows{content_y}},
                    {Columns{content_w}, Rows{content_h}}
                });
            }

            // 3b. Writeback: record info into the attached ScrollState
            //     based on this box's role in the scroll system.
            //
            //     - Viewport: compute content extent from children, write
            //       max_x/max_y, store the viewport's painted rect.
            //     - VerticalBar / HorizontalBar: store the bar's painted
            //       rect so mouse hover hit-testing in ScrollState::handle
            //       can route wheel events to the right axis.
            //
            //     We always record bounds (even if max didn't change) so
            //     resize and scroll-position changes don't desync the
            //     hover hit-rects.
            if (node.scroll_state != nullptr &&
                node.scroll_role != ScrollRole::None) {
                auto* s = node.scroll_state;
                // First writeback for this state THIS PAINT: clear bar
                // lists so a state that previously had bars but now
                // doesn't (e.g., user removed the scrollbar widget)
                // stops claiming hit regions. Also register with the
                // live-state list for auto-dispatch.
                if (s->paint_gen_seen != detail::paint_generation) {
                    s->paint_gen_seen = detail::paint_generation;
                    s->bars_h.clear();
                    s->bars_v.clear();
                    s->bar_h_bounds = {};
                    s->bar_v_bounds = {};
                    detail::live_scroll_states.push_back(s);
                }

                if (node.scroll_role == ScrollRole::Viewport) {
                    int content_extent_w = 0;
                    int content_extent_h = 0;
                    for (auto child_layout_idx : ln.children) {
                        const auto& cn = layout_nodes[child_layout_idx];
                        const int right  = cn.computed.pos.x.value + cn.computed.size.width.value;
                        const int bottom = cn.computed.pos.y.value + cn.computed.size.height.value;
                        if (right  > content_extent_w) content_extent_w = right;
                        if (bottom > content_extent_h) content_extent_h = bottom;
                    }
                    const int inner_origin_x = (has_b && node.border.sides.left ? 1 : 0) + node.layout.padding.left;
                    const int inner_origin_y = (has_b && node.border.sides.top  ? 1 : 0) + node.layout.padding.top;
                    const int content_w_total = std::max(0, content_extent_w - inner_origin_x);
                    const int content_h_total = std::max(0, content_extent_h - inner_origin_y);
                    const int new_max_x = std::max(0, content_w_total - content_w);
                    const int new_max_y = std::max(0, content_h_total - content_h);
                    if (new_max_x != s->max_x || new_max_y != s->max_y) {
                        detail::scroll_writeback_dirty = true;
                    }
                    s->max_x = new_max_x;
                    s->max_y = new_max_y;
                    s->viewport_bounds = {content_x, content_y, content_w, content_h};
                    s->clamp();
                } else if (node.scroll_role == ScrollRole::VerticalBar ||
                           node.scroll_role == ScrollRole::HorizontalBar) {
                    // Record the bar's NATURAL extent — the sum of its
                    // children's painted sizes — not the (possibly
                    // stretched) outer box. A scrollbar widget emits
                    // exactly `viewport_w` cells of glyphs; if the
                    // container stretches the widget to a larger width
                    // (default flexbox align: Stretch), the trailing
                    // cells are empty and clicks there must NOT count
                    // toward the bar — otherwise the hit-test math
                    // diverges from the rendered thumb position.
                    int natural_w = 0;
                    int natural_h = 0;
                    for (auto child_layout_idx : ln.children) {
                        const auto& cn = layout_nodes[child_layout_idx];
                        const int right  = cn.computed.pos.x.value + cn.computed.size.width.value;
                        const int bottom = cn.computed.pos.y.value + cn.computed.size.height.value;
                        if (right  > natural_w) natural_w = right;
                        if (bottom > natural_h) natural_h = bottom;
                    }
                    const int inner_origin_x = (has_b && node.border.sides.left ? 1 : 0) + node.layout.padding.left;
                    const int inner_origin_y = (has_b && node.border.sides.top  ? 1 : 0) + node.layout.padding.top;
                    natural_w = std::max(0, natural_w - inner_origin_x);
                    natural_h = std::max(0, natural_h - inner_origin_y);
                    // If the bar has no children somehow (shouldn't
                    // happen for a real scrollbar widget) fall back to
                    // the outer size.
                    const int rect_w = natural_w > 0 ? std::min(aw, natural_w) : aw;
                    const int rect_h = natural_h > 0 ? std::min(ah, natural_h) : ah;
                    const ScrollRect r{ax, ay, rect_w, rect_h};
                    if (node.scroll_role == ScrollRole::VerticalBar) {
                        s->bars_v.push_back(r);
                        s->bar_v_bounds = r;
                    } else {
                        s->bars_h.push_back(r);
                        s->bar_h_bounds = r;
                    }
                }
            }

            // 4. Recurse into children. Apply paint-time scroll offset by
            //    shifting the origin handed to descendants — yoga laid them
            //    out at natural positions in this box's coordinate space,
            //    we translate during paint. The clip rect pushed above for
            //    overflow:Hidden/Scroll prevents anything outside the inner
            //    content rect from being painted, so descendants effectively
            //    "scroll" within the viewport. This is the same mechanism
            //    the web uses (overflow:scroll + scrollTop on the element).
            //
            // Prefer the LIVE scroll_state position over the value captured
            // into `node.layout.scroll_y` at element-build time. The
            // writeback above (3b) just ran s->clamp(), so s->y now
            // reflects the freshly-computed max_y. The captured value in
            // node.layout can be stale by an unbounded amount when the
            // app sets y past max_y to express "stick to bottom after
            // pushing new content" (a one-line idiom for chat scrollback).
            // Without this branch the first frame after such a push
            // translates by the stale (huge) value, pushing every child
            // off-screen and producing a visible blank flicker before the
            // writeback-dirty re-render corrects it.
            const int live_scroll_x = (node.scroll_state != nullptr
                                       && node.scroll_role == ScrollRole::Viewport)
                ? node.scroll_state->x : node.layout.scroll_x;
            const int live_scroll_y = (node.scroll_state != nullptr
                                       && node.scroll_role == ScrollRole::Viewport)
                ? node.scroll_state->y : node.layout.scroll_y;
            const int child_ox = ax - live_scroll_x;
            const int child_oy = ay - live_scroll_y;
            for (const auto& [child, child_layout_idx] :
                     std::views::zip(node.children, ln.children)) {
                paint_element(
                    child,
                    canvas,
                    pool,
                    layout_nodes,
                    child_layout_idx,
                    child_ox,
                    child_oy);
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

                    {
                        auto _ = canvas.clip_scope(Rect{
                            {Columns{content_x}, Rows{content_y}},
                            {Columns{content_w}, Rows{content_h}}
                        });
                        paint_element(overlay, canvas, pool, sub, sub_root,
                                      content_x, content_y);
                    }   // clip auto-popped here, even on throw

                    if (!was_in_use) overlay_in_use = false;
                }
            }

            // Outer `guard` (if engaged) auto-pops here as the lambda returns.
        },

        [&](const TextElement& node) {
            const auto& lines = node.format(aw);

            if (node.runs.empty()) {
                uint16_t style_id = pool.intern(node.style);
                for (const auto& [row, line] :
                         lines | std::views::enumerate | std::views::take(ah)) {
                    canvas.write_text(ax, ay + static_cast<int>(row),
                                      line.text, style_id);
                }
                return;
            }

            // Truncation modes produce a single line that differs from
            // content (ellipsis appended/prepended). The word-wrap path
            // below aligns runs to wrapped output via the byte offset
            // cached in WrappedLine; truncated lines hold synthetic
            // ellipsis bytes that don't appear in content, so they take
            // this explicit-mapping branch instead.
            const bool truncates = (node.wrap == TextWrap::TruncateEnd ||
                                    node.wrap == TextWrap::TruncateStart ||
                                    node.wrap == TextWrap::TruncateMiddle);
            if (truncates && lines.size() == 1) {
                const auto& line = lines[0].text;
                const int y = ay;
                constexpr std::string_view kEll = "\xe2\x80\xa6";

                auto run_sid_at = [&](std::size_t pos) -> uint16_t {
                    std::size_t ri = 0;
                    while (ri + 1 < node.runs.size() &&
                           pos >= node.runs[ri].byte_offset +
                                  node.runs[ri].byte_length)
                        ++ri;
                    return pool.intern(
                        node.runs[std::min(ri, node.runs.size() - 1)].style);
                };

                // Paint line bytes [ls..le) with runs, where ls maps to
                // content byte cs. Returns x after the painted segment.
                auto paint_seg = [&](const std::string& ln,
                                     std::size_t ls, std::size_t le,
                                     std::size_t cs,
                                     int x0, int yy) -> int {
                    int xc = x0;
                    std::size_t ri = 0;
                    while (ri + 1 < node.runs.size() &&
                           cs >= node.runs[ri].byte_offset +
                                 node.runs[ri].byte_length)
                        ++ri;
                    for (std::size_t b = ls; b < le; ) {
                        std::size_t cb = cs + (b - ls);
                        while (ri + 1 < node.runs.size() &&
                               cb >= node.runs[ri].byte_offset +
                                     node.runs[ri].byte_length)
                            ++ri;
                        const auto& r = node.runs[
                            std::min(ri, node.runs.size() - 1)];
                        std::size_t re = r.byte_offset + r.byte_length;
                        std::size_t rem = (re > cb) ? (re - cb) : 1;
                        std::size_t ce = std::min(le, b + rem);
                        if (ce <= b) ce = b + 1;
                        auto sv = std::string_view(ln).substr(b, ce - b);
                        canvas.write_text(xc, yy, sv, pool.intern(r.style));
                        xc += string_width(sv);
                        b = ce;
                    }
                    return xc;
                };

                if (line == node.content) {
                    paint_seg(line, 0, line.size(), 0, ax, y);
                    return;
                }

                if (node.wrap == TextWrap::TruncateEnd &&
                    line.size() >= kEll.size())
                {
                    std::size_t plen = line.size() - kEll.size();
                    int xc = paint_seg(line, 0, plen, 0, ax, y);
                    canvas.write_text(xc, y, kEll, run_sid_at(plen));
                    return;
                }

                if (node.wrap == TextWrap::TruncateStart &&
                    line.size() >= kEll.size())
                {
                    std::size_t slen = line.size() - kEll.size();
                    std::size_t M = node.content.size() - slen;
                    canvas.write_text(ax, y, kEll, run_sid_at(M));
                    paint_seg(line, kEll.size(), line.size(), M,
                              ax + 1, y);
                    return;
                }

                uint16_t sid = pool.intern(node.style);
                canvas.write_text(ax, y, line, sid);
                return;
            }

            // Word-wrap / NoWrap with styled runs.
            //
            // Each WrappedLine carries its byte_offset into content
            // (populated by format(); see element/text.cpp). Run
            // alignment is therefore O(runs_per_line) per line instead
            // of the legacy O(content_size × lines) substring search.
            // run_idx is monotonic across lines because both lines and
            // runs are sorted by content offset.
            {
                std::size_t run_idx = 0;

                for (const auto& [row, line] :
                         lines | std::views::enumerate | std::views::take(ah)) {
                    int y = ay + static_cast<int>(row);
                    const std::string& line_text = line.text;
                    const std::size_t content_byte = line.byte_offset;

                    int x_cursor = ax;
                    std::size_t line_byte = 0;
                    while (line_byte < line_text.size()) {
                        std::size_t abs_byte = content_byte + line_byte;
                        while (run_idx + 1 < node.runs.size() &&
                               abs_byte >= node.runs[run_idx].byte_offset +
                                           node.runs[run_idx].byte_length) {
                            ++run_idx;
                        }

                        const auto& run = node.runs[std::min(run_idx, node.runs.size() - 1)];
                        std::size_t run_end = run.byte_offset + run.byte_length;
                        std::size_t chunk_end;
                        if (run_end > abs_byte) {
                            chunk_end = std::min(line_text.size(),
                                                 line_byte + (run_end - abs_byte));
                        } else {
                            chunk_end = line_byte + 1;
                        }
                        if (chunk_end <= line_byte) chunk_end = line_byte + 1;

                        auto chunk = std::string_view(line_text)
                                         .substr(line_byte, chunk_end - line_byte);
                        uint16_t sid = pool.intern(run.style);
                        canvas.write_text(x_cursor, y, chunk, sid);
                        x_cursor += string_width(chunk);
                        line_byte = chunk_end;
                    }
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

        [&](const ElementListRef& node) {
            // Borrowed-fragment paint: identical shape to ElementList,
            // but reads through the application-supplied pointer. Zero
            // copy of the items vector — only the per-child paint
            // recursion happens here.
            if (!node.items_ref) return;
            for (const auto& [child, child_layout_idx] :
                     std::views::zip(*node.items_ref, ln.children)) {
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

            auto& cache = component_cache();

            // ── Fast path: cached cells exist for this entry ────────────
            // The first paint of an entry captures its painted cells
            // (see the miss path below). Every subsequent frame reaches
            // this branch and copies the cells straight onto the
            // canvas — a memcpy per row, no build_layout_tree, no
            // layout::compute, no recursive paint_element walk over
            // the cached Element. This is what makes per-cached-turn
            // cost O(width) rather than O(child_tree_size).
            if (auto* entry = find_component_cache(cache, node, content_w);
                entry && !entry->cells.empty() && entry->cells_rows > 0)
            {
                entry->last_frame = cache.current_frame;

                auto _ = canvas.clip_scope(Rect{
                    {Columns{content_x}, Rows{content_y}},
                    {Columns{content_w}, Rows{content_h}}
                });
                const int rows = std::min(entry->cells_rows, content_h);
                const int max_y = entry->cells_max_y;
                for (int y = 0; y < rows; ++y) {
                    canvas.blit_packed_row(
                        content_x, content_y + y,
                        entry->cells.data() + static_cast<std::size_t>(y) * entry->width,
                        entry->width,
                        /*row_has_content=*/y <= max_y);
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
            //  - cache_id is set (host opted into cross-frame identity), OR
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
                    if (!node.cache_id.empty()
                        || e->last_frame == cache.current_frame) {
                        return e;
                    }
                }
                return nullptr;
            }();
            if (reuse_entry) {
                child_ptr = &reuse_entry->result;
                reuse_entry->last_frame = cache.current_frame;
            } else {
                // Render fresh. For pointer-keyed entries we still
                // store the result so this frame's measure-then-paint
                // pair can share a single render() call — but we
                // don't trust it across frames.
                fresh_render = node.render(content_w, content_h);
                store_component_cache(cache, node, {
                    content_w,
                    /*height=*/content_h,
                    fresh_render,
                    cache.current_frame,
                    node.generation
                });
                if (auto* entry = find_component_cache(cache, node, content_w)) {
                    child_ptr = &entry->result;
                } else {
                    child_ptr = &fresh_render;
                }
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
                // paid once per (cache_id, width) and amortized across
                // every subsequent fast-path blit. We only run this
                // when the entry is going to be cells-cached.
                const bool will_cache =
                    !node.cache_id.empty() && content_w > 0 && content_h > 0;
                if (will_cache) {
                    canvas.fill(
                        Rect{{Columns{content_x}, Rows{content_y}},
                             {Columns{content_w}, Rows{content_h}}},
                        U' ', /*style_id=*/0);
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
            // Capture is gated on cache_id being set: pointer-keyed
            // entries are by definition ephemeral (the wrapper has a
            // fresh address each frame), and caching cells for them
            // would burn memory that's evicted next frame anyway.
            if (!node.cache_id.empty()) {
                if (auto* entry = find_component_cache(cache, node, content_w)) {
                    const int max_y_after = canvas.max_content_row();
                    int captured_rows = content_h;
                    if (entry->height > 0 && entry->height < captured_rows)
                        captured_rows = entry->height;
                    // Skip cache capture when the read rect overruns
                    // the canvas — happens on the first pass of the
                    // inline grow-and-retry loop in app.cpp before
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
                    const bool fits_on_canvas =
                        content_x >= 0 && content_y >= 0 &&
                        content_x + content_w <= canvas_w &&
                        content_y + captured_rows <= canvas_h;
                    if (!fits_on_canvas) {
                        entry->cells.clear();
                        entry->cells_rows = 0;
                        entry->cells_max_y = -1;
                    } else if (captured_rows > 0) {
                        entry->cells_rows = captured_rows;
                        // Highest non-blank row inside our region.
                        // If paint didn't bump max_y_, nothing
                        // visible got drawn — cells_max_y = -1 lets
                        // the fast path skip the max_y_ update.
                        entry->cells_max_y =
                            (max_y_after > max_y_before
                             && max_y_after >= content_y)
                                ? std::min(max_y_after - content_y,
                                           captured_rows - 1)
                                : -1;
                        entry->cells.assign(
                            static_cast<std::size_t>(captured_rows)
                                * static_cast<std::size_t>(content_w),
                            uint64_t{U' '});
                        for (int y = 0; y < captured_rows; ++y) {
                            for (int x = 0; x < content_w; ++x) {
                                entry->cells[
                                    static_cast<std::size_t>(y) * content_w + x] =
                                    canvas.get_packed(content_x + x,
                                                      content_y + y);
                            }
                        }
                    }
                }
            }
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

    // Cross-frame ComponentElement render cache management.
    //
    // At the OUTERMOST render_tree call: bump the frame counter and
    // evict entries whose last_frame_used is older than the previous
    // frame.  Stable entries (same ComponentElement* still in the
    // tree) get their timestamps refreshed during this frame's
    // measure/paint and survive; ephemeral entries (one-shot
    // components built fresh each frame) age out within a frame of
    // last access.
    //
    // Nested render_tree calls (a component recursively rendering
    // another tree) inherit the existing frame counter — they see the
    // same cache state as their parent.
    auto& depth = render_detail::render_depth();
    bool top_level = (depth == 0);
    ++depth;
    if (top_level) {
        // Bump scroll paint generation and clear the live-states list.
        // Each top-level render walk re-populates the list via the
        // writeback below; any state whose tree was removed since the
        // last paint stops receiving auto-dispatched events.
        ++detail::paint_generation;
        detail::live_scroll_states.clear();

        auto& cache = render_detail::component_cache();
        // Evict before bumping current_frame so entries from the
        // previous frame are still tagged "last_frame == previous".
        // Both maps share the same eviction policy — an entry that
        // wasn't touched in the immediately preceding frame is gone
        // by the next one, regardless of which key it lives under.
        const std::uint64_t prev = cache.current_frame;
        for (auto it = cache.entries.begin(); it != cache.entries.end(); ) {
            if (it->second.last_frame < prev) {
                it = cache.entries.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = cache.entries_by_id.begin(); it != cache.entries_by_id.end(); ) {
            if (it->second.last_frame < prev) {
                it = cache.entries_by_id.erase(it);
            } else {
                ++it;
            }
        }
        ++cache.current_frame;
    }
    struct Cleanup {
        int* d;
        ~Cleanup() { if (d) --*d; }
    } cleanup{&depth};

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
    {
        auto _ = canvas.clip_scope(Rect{
            {Columns{x}, Rows{y}},
            {Columns{w}, Rows{h}}
        });
        render_detail::paint_element(
            root, canvas, pool, layout_nodes, root_idx,
            /*offset_x=*/x, /*offset_y=*/y);
    }
}

} // namespace maya
