// src/render/layout_build.cpp — Element tree -> layout nodes.
#include "render_internal.hpp"

namespace maya {
namespace render_detail {

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

                        // Clamp an unconstrained measure width to the
                        // canvas. The hash-keyed cache stores one entry
                        // per hash_id (width is a FIELD, not part of the
                        // key — see store_component_cache), so if the
                        // measure pass keys this component at the layout
                        // engine's kUnconstrained sentinel (1<<24) while
                        // the paint pass keys it at the real content
                        // width, the two passes overwrite each other's
                        // entry every frame and the cell cache never
                        // survives to be blitted — a permanent miss that
                        // makes every settled card re-render forever.
                        // Clamping both sites to the canvas width makes
                        // them agree. A component can't be wider than the
                        // canvas it measures against.
                        const int canvas_w = available_width();
                        if (canvas_w > 0 && max_width > canvas_w)
                            max_width = canvas_w;

                        auto& cache = component_cache();
                        // Trust a cached height for a hash-keyed entry
                        // even when the measure-time width differs from
                        // the width the entry was captured at. The
                        // measure pass for a component nested in an
                        // auto-height / stretch container is handed the
                        // layout engine's unconstrained width, which
                        // rarely equals the definite width the PAINT
                        // pass later resolves and caches cells at. If we
                        // demanded an exact width match here, the measure
                        // pass would re-render (and re-lay-out) the full
                        // body every frame even though paint already has
                        // valid cells — defeating the cache for exactly
                        // the tall settled cards it exists to accelerate.
                        // The height a component reports is overwhelmingly
                        // width-stable for these cards (a settled
                        // write/edit/read body wraps to the same line
                        // count across the small width deltas in play),
                        // and a genuine resize invalidates the cells via
                        // the paint-side width-keyed replace, so trusting
                        // the stored height here is safe and is what lets
                        // the measure pass become O(1).
                        if (!comp.hash_id.empty()) {
                            auto it = cache.entries_by_hash.find(comp.hash_id);
                            if (it != cache.entries_by_hash.end()) {
                                // Trust the stored height for a hash-keyed
                                // (content-identity) entry whether or not
                                // its cells have been captured. The height
                                // was produced by a real layout::compute
                                // over this exact content on the store
                                // path below, and it is width-stable across
                                // the small measure/paint width deltas in
                                // play (see the note above) — so it's valid
                                // regardless of paint state.
                                //
                                // The cells-empty case is NOT a corner case
                                // to reject: a hash-keyed component whose
                                // rows sit ABOVE the viewport (a windowed
                                // streaming-markdown head wrapper collapsing
                                // the old committed blocks; a frozen
                                // scrollback prefix) is measured every frame
                                // by the outer flex pass but NEVER painted
                                // (it's clipped off-screen), so its cells
                                // are never captured. Demanding non-empty
                                // cells here forced that component to
                                // re-render() + re-lay-out its whole subtree
                                // EVERY frame — reintroducing the O(N) per-
                                // frame cost the wrapper existed to remove.
                                // Trusting the stored height makes the
                                // measure O(1); if the component IS later
                                // painted (scrolls into view) the paint
                                // slow-path re-renders from `result` and
                                // captures cells.
                                if (it->second.height > 0) {
                                    it->second.last_frame = cache.current_frame;
                                    touch_hash_cache(cache, it->second);
                                    return {Columns{max_width},
                                            Rows{it->second.height}};
                                }
                            }
                        }
                        // Trust the cached height when:
                        //  - hash_id is set (host opted into cross-frame
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
                                !comp.hash_id.empty()
                                || entry->last_frame == cache.current_frame;
                            if (trust) {
                                entry->last_frame = cache.current_frame;
                                if (!comp.hash_id.empty())
                                    touch_hash_cache(cache, *entry);
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
                        g_component_render_calls.fetch_add(
                            1, std::memory_order_relaxed);
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

} // namespace render_detail
} // namespace maya
