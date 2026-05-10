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
inline ComponentCacheEntry* find_component_cache(ComponentCache& cache,
                                                 const ComponentElement& comp,
                                                 int width) noexcept {
    if (!comp.cache_id.empty()) {
        auto it = cache.entries_by_id.find(comp.cache_id);
        if (it != cache.entries_by_id.end()
            && it->second.width      == width
            && it->second.generation == comp.generation) {
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
                        if (auto* entry = find_component_cache(cache, comp, max_width)) {
                            // Cross-frame hit (pointer-keyed for stable
                            // ComponentElement slots, content-keyed via
                            // cache_id for value-copied wrappers). Reuse
                            // the cached render and height — no
                            // render() call, no recursive layout.
                            entry->last_frame = cache.current_frame;
                            return {Columns{max_width},
                                    Rows{entry->height}};
                        }

                        // Cache miss. Render, layout, store under
                        // whichever key matches comp.cache_id's state.
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
                    canvas.write_text(ax, ay + static_cast<int>(row), line, style_id);
                }
                return;
            }

            // Truncation modes produce a single line that differs from
            // content (ellipsis appended/prepended). The word-wrap path
            // below aligns runs via substring matching against content;
            // that search fails on truncated output because the ellipsis
            // bytes don't exist in content, causing the loop to overshoot
            // and split multi-byte UTF-8 (U+FFFD on the terminal).
            //
            // Fix: for TruncateEnd/Start, map the truncated line's byte
            // ranges back to content positions explicitly. The prefix/
            // suffix portions are byte-identical to their corresponding
            // content spans; only the ellipsis is synthetic.
            const bool truncates = (node.wrap == TextWrap::TruncateEnd ||
                                    node.wrap == TextWrap::TruncateStart ||
                                    node.wrap == TextWrap::TruncateMiddle);
            if (truncates && lines.size() == 1) {
                const auto& line = lines[0];
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

            // Word-wrap / NoWrap with styled runs — substring matching.
            {
                std::size_t content_byte = 0;
                std::size_t run_idx = 0;

                for (const auto& [row, line] :
                         lines | std::views::enumerate | std::views::take(ah)) {
                    int y = ay + static_cast<int>(row);

                    while (content_byte < node.content.size() &&
                           node.content.substr(content_byte, line.size()) != line) {
                        ++content_byte;
                    }

                    int x_cursor = ax;
                    std::size_t line_byte = 0;
                    while (line_byte < line.size()) {
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

            // Reuse the rendered Element from the auto-measure pass.  In
            // the steady state this is a cross-frame hit — the entry
            // was populated on the FIRST frame the component appeared
            // and has been refreshed (last_frame_used bumped) on every
            // subsequent frame.  Read by reference, not by move: the
            // entry must survive for the next frame's measure to find.
            Element child;
            auto& cache = component_cache();
            if (auto* entry = find_component_cache(cache, node, content_w)) {
                child = entry->result;              // copy — cheap shared tree
                entry->last_frame = cache.current_frame;
            } else {
                // Width mismatch, missing entry, or pointer-aliased
                // entry from a prior unrelated instance (different
                // generation). Render and overwrite. Defensive path —
                // measure populates the entry for the steady state;
                // this fires only when the measure callback wasn't
                // invoked (a parent that sized the child manually) or
                // address reuse hit the cache after the prior
                // instance was freed.
                child = node.render(content_w, content_h);
                store_component_cache(cache, node, {
                    content_w,
                    /*height=*/content_h,
                    child,
                    cache.current_frame,
                    node.generation
                });
            }

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

            {
                auto _ = canvas.clip_scope(Rect{
                    {Columns{content_x}, Rows{content_y}},
                    {Columns{content_w}, Rows{content_h}}
                });
                paint_element(child, canvas, pool, sub_nodes, sub_root,
                              content_x, content_y);
            }
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
