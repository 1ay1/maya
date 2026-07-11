#include "maya/render/renderer.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

#include "maya/core/overload.hpp"
#include "maya/core/hit.hpp"
#include "maya/core/render_context.hpp"
#include "maya/core/scroll_state.hpp"
#include "maya/render/scrollback_ledger.hpp"

namespace maya {

namespace render_detail {

// Diagnostic counter — see renderer.hpp. Relaxed: it's a monotone tally
// read only between frames by tests, never a synchronisation point.
namespace {
std::atomic<std::uint64_t> g_component_render_calls{0};
}
std::uint64_t component_render_calls() noexcept {
    return g_component_render_calls.load(std::memory_order_relaxed);
}

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
    // Wallclock timestamp of the last hit. Used by hash-keyed
    // (hash_id) eviction so a long idle that produces no frames
    // doesn't immediately drop entries the user is still interacting
    // with — fps=0 makes "frames since last hit" a poor proxy for
    // "how long ago." Set on every hit; checked at top-of-frame
    // eviction. Default-constructed time_point compares less-than any
    // sane now(), so a fresh-but-not-yet-touched entry is eligible
    // for eviction on the first sweep after store — but stays alive
    // for at least one frame because the SAME-FRAME hit-stamp at
    // measure/paint moves last_touched_at to now().
    std::chrono::steady_clock::time_point last_touched_at{};

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
    // Ambient background active when `cells` were captured (see
    // ambient_bg() below). The captured cells BAKE that bg into every
    // glyph's style id, so they are only valid to blit when the
    // CURRENT ambient matches. A hash-keyed component whose content
    // moves between differently-colored strips (e.g. a row widget that
    // gains a selection band) would otherwise blit stale-bg cells —
    // the fast path gates on this and falls through to a re-render +
    // recapture instead. Layout identity (width/height/result) is
    // bg-independent, so only the cells are gated, never the measure.
    std::optional<Color> cells_ambient;
    // Per-row rightmost non-blank column (length == cells_rows). Captured
    // alongside the cells in the slow path, then handed to
    // blit_packed_row on the fast path so the canvas can update its
    // last_col_ tracker without re-scanning the row from the right.
    // Saves O(content_w) per cached row per frame on long transcripts
    // — the per-frame inline-mode paint walks every cached prefix block
    // and previously paid that scan even though we knew the answer at
    // capture time. Empty when cells is empty.
    std::vector<int> cells_row_last_col;
};

struct ComponentCache {
    // Pointer-keyed cache: address-stable ComponentElements (typically
    // those held in a stable vector slot the host doesn't reallocate)
    // hit here. Pointer compare is one instruction so this is the
    // first-class hot path.
    std::unordered_map<const ComponentElement*, ComponentCacheEntry> entries;
    // Hash-keyed cache (Witness Chain): when a ComponentElement
    // carries a non-empty `hash_id` (typed CacheId), lookups go
    // through this map. The 64-bit hash is collision-bounded by the
    // FNV-1a + type-tag construction, so two unrelated widgets can't
    // alias each other's entries unless their typed hash inputs are
    // bit-identical — the same probabilistic floor (2⁻⁶⁴) the
    // shadow-of-wire hash already accepts.
    std::unordered_map<CacheId, ComponentCacheEntry>                 entries_by_hash;
    std::uint64_t current_frame = 0;
};

inline ComponentCache& component_cache() {
    thread_local ComponentCache c;
    return c;
}

// Look up a ComponentElement in the cross-frame cache. Priority order:
//   1. hash_id (typed CacheId)  — Witness Chain content keying.
//   2. &comp + generation       — pointer-keyed fallback for
//                                 components that opted out of
//                                 cross-frame caching.
//
// Returns nullptr on miss; caller stores via store_component_cache().
//
// Generation handling diverges between the paths:
//   - Pointer keying: generation MUST match. The pointer alone is not
//     a stable identity (the allocator can recycle a freed
//     ComponentElement's address); the generation check rejects the
//     aliased entry.
//   - hash_id: generation is IGNORED. The id IS the cross-frame
//     identity.
inline ComponentCacheEntry* find_component_cache(ComponentCache& cache,
                                                 const ComponentElement& comp,
                                                 int width) noexcept {
    if (!comp.hash_id.empty()) {
        auto it = cache.entries_by_hash.find(comp.hash_id);
        if (it != cache.entries_by_hash.end()
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

// Insert / overwrite a cache entry under the appropriate key. Returns a
// pointer to the stored entry so the caller can keep working without a
// second lookup. Pointer is valid until the next mutation of the
// matching map (insert/erase): the typical caller (paint slow path)
// uses it for one read/write before paint_element runs, then
// re-resolves after paint because the recursive walk may have inserted
// other entries and rehashed the map.
inline ComponentCacheEntry* store_component_cache(ComponentCache& cache,
                                                  const ComponentElement& comp,
                                                  ComponentCacheEntry entry) {
    if (!comp.hash_id.empty()) {
        auto it = cache.entries_by_hash.find(comp.hash_id);
        if (it != cache.entries_by_hash.end()
            && entry.cells.empty() && !it->second.cells.empty()
            && entry.width == it->second.width)
        {
            // Same-width re-store with no cells: this is the measure
            // pass landing on the slot the paint pass already populated
            // with cells at the SAME width. Both passes key on hash_id
            // alone (width is a field, not part of the key), so a naive
            // insert_or_assign here would wipe the paint pass's cells
            // every frame and the blit fast path could never fire — the
            // component would re-render forever. Keep the cells; refresh
            // only the cheap bookkeeping. The width-equality guard means
            // a genuine resize (different width) still falls through to
            // the replace below, correctly invalidating stale-width
            // cells.
            it->second.height          = entry.height;
            it->second.last_frame      = entry.last_frame;
            it->second.generation      = entry.generation;
            it->second.last_touched_at = entry.last_touched_at;
            it->second.result          = std::move(entry.result);
            return &it->second;
        }
        auto [iit, _] = cache.entries_by_hash.insert_or_assign(
            comp.hash_id, std::move(entry));
        return &iit->second;
    }
    auto [it, _] = cache.entries.insert_or_assign(&comp, std::move(entry));
    return &it->second;
}

inline int& render_depth() {
    thread_local int d = 0;
    return d;
}

// ── Ambient background (paint-time bg inheritance) ──────────────────
// The bg color of the nearest enclosing BoxElement that declared one.
//
// The terminal cell model doesn't composite: write_text/set REPLACE the
// cell wholesale, and a Style with no bg emits an SGR that resets the
// cell to the TERMINAL DEFAULT background. So a box's bgc() fill only
// survived in cells no descendant text touched — every fg-only glyph
// punched a default-bg hole in the strip (the classic "text has its own
// background" artifact on selected-row strips, filled buttons, chips).
//
// Fix: while painting a box with style.bg set, record that color as the
// ambient background; every descendant TextElement run that does NOT
// carry its own bg is interned with the ambient bg baked in — the same
// resolution CSS does for `background: transparent` children. Runs with
// an explicit bg (code chips, meter grooves, highlight marks) win over
// the ambient, and a run that WANTS the terminal-default bg inside a
// filled box can say so explicitly with Color::Kind::Default.
//
// Thread-local + RAII scope (not a paint_element parameter) so the
// inheritance flows through every recursion seam — ElementList
// fragments, stack overlays, and ComponentElement sub-renders — without
// touching the public paint_element signature. Hash-keyed component
// cell caches are ambient-safe WITHOUT host cooperation: each entry
// stamps the ambient active at capture time (cells_ambient) and the
// blit fast path refuses to serve cells captured under a different
// ambient — it falls through to a re-render + recapture. Hosts do NOT
// need to fold strip colors into hash_id.
inline std::optional<Color>& ambient_bg() {
    thread_local std::optional<Color> bg;
    return bg;
}

struct AmbientBgScope {
    std::optional<Color> prev_;
    bool engaged_ = false;
    explicit AmbientBgScope(const std::optional<Color>& next) {
        if (next.has_value()) {
            prev_ = ambient_bg();
            ambient_bg() = next;
            engaged_ = true;
        }
    }
    ~AmbientBgScope() { if (engaged_) ambient_bg() = prev_; }
    AmbientBgScope(const AmbientBgScope&) = delete;
    AmbientBgScope& operator=(const AmbientBgScope&) = delete;
};

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
        case Align::Auto:     return layout::Align::Auto;
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
                                // captures cells, and a genuine resize
                                // evicts the width-keyed entry.
                                if (it->second.height > 0) {
                                    it->second.last_frame = cache.current_frame;
                                    it->second.last_touched_at =
                                        std::chrono::steady_clock::now();
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
                                entry->last_touched_at =
                                    std::chrono::steady_clock::now();
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
                            comp.generation,
                            std::chrono::steady_clock::now()
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

    // Edge loops: extend to the corner cells WHEN no corner is drawn
    // there. A corner is only drawn when BOTH adjacent sides are
    // present (see the conditions above). If a corner is absent, the
    // edge owns that cell — otherwise the cell is just blank, which
    // visibly fragments left-only / right-only / top-only / bottom-only
    // borders (e.g. the Turn widget's bold left rail: top/bottom/right
    // are all false, so rows y0 and y1 had no glyph at all, producing
    // the missing-row gap at the top and bottom of every speaker turn).

    // Top edge — extends to (x0, y0) when no left, to (x1, y0) when no right.
    if (border.sides.top) {
        const int x_start = border.sides.left  ? x0 + 1 : x0;
        const int x_end   = border.sides.right ? x1     : x1 + 1;
        for (int x = x_start; x < x_end; ++x)
            canvas.set(x, y0, cp.top, style_id);
    }

    // Bottom edge — symmetric to top.
    if (border.sides.bottom) {
        const int x_start = border.sides.left  ? x0 + 1 : x0;
        const int x_end   = border.sides.right ? x1     : x1 + 1;
        for (int x = x_start; x < x_end; ++x)
            canvas.set(x, y1, cp.bottom, style_id);
    }

    // Left edge — extends to (x0, y0) when no top, to (x0, y1) when no bottom.
    if (border.sides.left) {
        const int y_start = border.sides.top    ? y0 + 1 : y0;
        const int y_end   = border.sides.bottom ? y1     : y1 + 1;
        for (int y = y_start; y < y_end; ++y)
            canvas.set(x0, y, cp.left, style_id);
    }

    // Right edge — symmetric to left.
    if (border.sides.right) {
        const int y_start = border.sides.top    ? y0 + 1 : y0;
        const int y_end   = border.sides.bottom ? y1     : y1 + 1;
        for (int y = y_start; y < y_end; ++y)
            canvas.set(x1, y, cp.right, style_id);
    }

    // Border title text (still uses write_text — rare path, variable-length string).
    auto paint_btext = [&](const BorderText& bt) {
        if (bt.content.empty()) return;
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
    };

    if (border.text.has_value())     paint_btext(*border.text);
    if (border.text_end.has_value()) paint_btext(*border.text_end);
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
            // 0. Hit-region writeback: a box tagged `| hit(id)` records its
            //    ABSOLUTE painted rect in the per-frame registry so mouse
            //    coordinates resolve to it via maya::hit_test(). Recorded
            //    BEFORE children paint — children registered after are
            //    later in the vector, and hit_test's backward scan gives
            //    them precedence (innermost/topmost target wins).
            if (node.hit_id != 0 && aw > 0 && ah > 0) {
                maya::detail::hit_regions().push_back(
                    {node.hit_id, ax, ay, aw, ah});
            }

            // 1. Fill background if the box has a bg color.
            if (node.style.bg.has_value()) {
                uint16_t bg_style_id = pool.intern(
                    Style{}.with_bg(*node.style.bg));
                canvas.fill(abs_rect, U' ', bg_style_id);
            }

            // 1b. Establish the ambient background for this box's entire
            //     subtree (children + stack overlays). Descendant text runs
            //     with no explicit bg inherit it at intern time — see the
            //     TextElement visitor — so glyphs can't punch terminal-
            //     default holes in the fill painted above. RAII: restores
            //     the enclosing ambient when this lambda returns.
            AmbientBgScope ambient_scope(node.style.bg);

            // 2. Draw border.
            if (node.has_border()) {
                Style border_style = node.style;
                if (node.border.colors.top.has_value()) {
                    border_style = border_style.with_fg(*node.border.colors.top);
                }
                uint16_t border_style_id = pool.intern(border_style);
                paint_border(canvas, node.border, abs_rect, border_style_id);
            }

            // 3. Push clip when children must not paint outside this box.
            //    RAII guard ensures the pop happens even if a child's paint
            //    throws — without it, a thrown paint callback would leave a
            //    stale clip on the stack and silently mangle later rendering.
            //
            //    Three cases force a clip:
            //      (a) overflow:hidden / scroll — the explicit opt-in.
            //      (b) the box has a DEFINITE (non-auto) main/cross size.
            //          A box the user sized explicitly (e.g. card(height=N))
            //          is a bounded region; when its content overflows that
            //          size the flex layout can position the overflow rows
            //          at clamped/overlapping coordinates, and with the
            //          default overflow:visible those rows used to paint ON
            //          TOP of the surviving rows — a longer row's tail then
            //          smeared through a shorter one (e.g. a fixed-height
            //          event-log card showing "…replenishedt 2 pods").
            //      (c) the box has a BORDER. A border is a hard visual
            //          boundary the user drew around the content; children
            //          must never paint past it. Without this an auto-height
            //          card that the flex layout squeezed (e.g. grow=1 cards
            //          on a short terminal) would render its overflowing
            //          children BELOW the closed bottom border — a meter row
            //          escaping through "╰─net ███…─╯". Clipping pins the
            //          content inside the frame and the excess is dropped.
            //
            //    An auto-sized, border-less box shrink-wraps to its content
            //    and can't overflow, so it stays unclipped to preserve any
            //    deliberate overflow:visible behaviour.
            //
            //    NB: clipping is suppressed during an auto-height render
            //    (inline mode / the fullscreen grow-and-retry measure pass).
            //    There the canvas starts smaller than the content and the
            //    host grows it based on how far content painted; a clip to
            //    the undersized box would stop content from extending, the
            //    measured height would never grow, and the screen would stay
            //    blank. Real overflow clipping only matters once the size is
            //    settled, which is the non-auto-height paint.
            bool definite_size = node.layout.width.is_fixed() ||
                                 node.layout.width.is_percent() ||
                                 node.layout.height.is_fixed() ||
                                 node.layout.height.is_percent();
            bool clipping = !is_auto_height() &&
                            (node.overflow == Overflow::Hidden ||
                             node.overflow == Overflow::Scroll ||
                             definite_size ||
                             node.has_border());
            // overflow:hidden / scroll are explicit and must clip even in an
            // auto-height pass (a scroll viewport defines its own bounded box).
            if (node.overflow == Overflow::Hidden ||
                node.overflow == Overflow::Scroll)
                clipping = true;

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
                    detail::live_scroll_states().push_back(s);
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

            // Ambient-bg fold: a run with no explicit bg inherits the
            // nearest enclosing box's bg (see AmbientBgScope). Without
            // this, the SGR for a bg-less style resets the cell to the
            // terminal default and the glyph punches a hole in the box's
            // fill. An explicit bg — including Color::Kind::Default as
            // the deliberate "terminal bg" opt-out — always wins.
            auto intern_bg = [&](const Style& s) -> uint16_t {
                const auto& amb = ambient_bg();
                if (amb.has_value() && !s.bg.has_value()) {
                    Style folded = s;
                    return pool.intern(folded.with_bg(*amb));
                }
                return pool.intern(s);
            };

            if (node.runs.empty()) {
                uint16_t style_id = intern_bg(node.style);
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
                    return intern_bg(
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
                        canvas.write_text(xc, yy, sv, intern_bg(r.style));
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

                uint16_t sid = intern_bg(node.style);
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
                        uint16_t sid = intern_bg(run.style);
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
            // Witness Chain — Trim Accounting write-back. When the
            // fragment is ledger-tagged, stamp each block's laid-out
            // height into the ledger BEFORE painting it. These heights
            // come from the SAME layout::compute pass whose positions
            // the compose serializes this frame, at the live width —
            // they ARE the wire heights, by construction. The ledger
            // mints trim-commit counts exclusively from these stamps,
            // which is what makes a host-side measurement drift
            // (the historical trim-corruption class) unrepresentable.
            const bool ledger_ok = node.ledger != nullptr
                && node.items_ref == &node.ledger->elements()
                && node.items_ref->size() == ln.children.size();
            // Also stamp the width the blocks were laid out at. This is
            // the constraint compute() handed the fragment's children —
            // the SAME width a block sealed next update cycle will be
            // laid out at in the freeze frame. seal_measured() warms
            // the measure cache at this width, which is what retires
            // the host's "reconstruct the content width by subtracting
            // the chrome paddings" fossil (the -4 comment stack).
            if (ledger_ok) node.ledger->record_paint_width(aw);
            std::size_t block_idx = 0;
            for (const auto& [child, child_layout_idx] :
                     std::views::zip(*node.items_ref, ln.children)) {
                if (ledger_ok) {
                    node.ledger->record_paint(
                        block_idx++,
                        layout_nodes[child_layout_idx]
                            .computed.size.height.raw());
                }
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
                        entry->last_touched_at =
                            std::chrono::steady_clock::now();
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
                entry->last_touched_at = std::chrono::steady_clock::now();

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
                for (int y = y_lo; y < y_hi; ++y) {
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
                reuse_entry->last_touched_at = std::chrono::steady_clock::now();
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
                    node.generation,
                    std::chrono::steady_clock::now()
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
    });
}

} // namespace render_detail

Size measure_element(const Element& elem, int max_width, int max_height)
{
    // A real layout pass over the fragment — the exact engine the renderer
    // runs, so the answer can't disagree with the eventual paint. The root
    // keeps its own width/height styles (usually auto): an auto-width
    // container shrink-wraps to its widest line (yoga.cpp §4), an auto-width
    // text leaf reports its natural columns, so the result is the fragment's
    // NATURAL size within the given bounds.
    if (max_width < 0) max_width = 0;
    if (max_height < 0) max_height = 0;
    thread_local std::vector<layout::LayoutNode> nodes;
    thread_local bool in_use = false;   // re-entrant (component measure can nest)
    std::vector<layout::LayoutNode> local;
    const bool was_in_use = in_use;
    auto& buf = was_in_use ? local : nodes;
    buf.clear();
    in_use = true;
    std::size_t root = render_detail::build_layout_tree(elem, buf, /*theme=*/{});
    layout::compute(buf, root, max_width, max_height);
    Size out{Columns{buf[root].computed.size.width.value},
             Rows{buf[root].computed.size.height.value}};
    if (!was_in_use) in_use = false;
    return out;
}

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
    // Inherit host-set fields that the dimension-only ctor doesn't carry.
    // inline_min_content is set on the App's persistent render_ctx_ AFTER
    // it computes the composer anti-bounce pad, then App re-invokes
    // render_tree on the SAME tree; without this inheritance the fresh
    // ctx here would zero it and the lazy pad component would read 0.
    if (detail::render_ctx_)
        ctx.inline_min_content = detail::render_ctx_->inline_min_content;
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
        detail::live_scroll_states().clear();
        // Same discipline for the hit-region registry: this frame's
        // paint re-registers every `| hit(id)` box at its new position;
        // targets that left the tree stop hit-testing automatically.
        maya::detail::hit_regions().clear();

        auto& cache = render_detail::component_cache();
        // Evict before bumping current_frame so entries from the
        // previous frame are still tagged "last_frame == previous".
        // The two maps share the same intent (an entry not touched
        // in the immediately preceding frame is gone), but use
        // different retention windows tuned to their identity model.
        const std::uint64_t prev = cache.current_frame;

        // Pointer-keyed entries: strict 1-frame eviction. Cross-frame
        // pointer-keyed REUSE is forbidden anyway (the slow-path lookup
        // gates on `last_frame == current_frame`, see the paint branch
        // below) because the wrapper's lambda may have closed over
        // mutable state, so keeping these around longer would just
        // waste memory on entries we'll never trust again.
        for (auto it = cache.entries.begin(); it != cache.entries.end(); ) {
            if (it->second.last_frame < prev) {
                it = cache.entries.erase(it);
            } else {
                ++it;
            }
        }

        // Hash-keyed entries: sized LRU with a high/low-water mark.
        // hash_id is a content-stable identity — the cached cells are
        // valid until the id stops appearing in the tree, regardless of
        // how long the host has been idle. A wallclock cutoff (the
        // previous policy) misfires under event-driven (fps=0) hosts
        // that paint only on input: every frozen scrollback entry got
        // evicted after a few seconds of idle, and the next keystroke
        // paid an O(N) full re-render to repopulate. Cap by entry count
        // instead; the LRU bounds memory without timing out live content.
        //
        // Trim to a LOW-WATER mark (not exactly kHashCacheMax) so the
        // O(N) build-index + nth_element fires once every ~(max-low)
        // insertions instead of on every frame that hovers at the cap.
        // Without the gap, a steady-state working set sitting right at
        // kHashCacheMax would re-run the full scan-and-drop every single
        // frame — an O(N) spike per frame exactly when the cache is
        // busiest.
        constexpr std::size_t kHashCacheMax = 4096;
        constexpr std::size_t kHashCacheLow = 3072;   // 75% — batch target
        if (cache.entries_by_hash.size() > kHashCacheMax) {
            std::vector<std::pair<std::chrono::steady_clock::time_point, CacheId>> idx;
            idx.reserve(cache.entries_by_hash.size());
            for (const auto& [id, entry] : cache.entries_by_hash)
                idx.emplace_back(entry.last_touched_at, id);
            const std::size_t drop =
                cache.entries_by_hash.size() - kHashCacheLow;
            std::nth_element(idx.begin(), idx.begin() + drop, idx.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
            for (std::size_t i = 0; i < drop; ++i)
                cache.entries_by_hash.erase(idx[i].second);
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
