#pragma once
// src/render/render_internal.hpp — the renderer's internals, shared by its
// .cpp files only (never installed):
//   - the cross-frame component render cache
//   - render depth and the ambient background
//   - the flex enum mappers
//   - PaintCtx and one painter per element kind (paint_box.cpp, ...)

#include "maya/render/renderer.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
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

// Telemetry counters (defined in renderer.cpp; relaxed monotone tallies).
extern std::atomic<std::uint64_t> g_component_render_calls;
extern std::atomic<std::uint64_t> g_blit_rows_epoch_skip;
extern std::atomic<std::uint64_t> g_blit_rows_compared;
extern std::atomic<std::uint64_t> g_blit_entries;
extern std::atomic<std::uint64_t> g_rt_build_ns;
extern std::atomic<std::uint64_t> g_rt_layout_ns;
extern std::atomic<std::uint64_t> g_rt_paint_ns;

// ── cross-frame component cache ─────────────────────────────────────────
struct ComponentCacheEntry {
    int      width  = -1;         // width at which the result was rendered
    int      height = 0;          // measured natural height at that width
    Element  result;              // cached render() output
    // Monotonic frame stamp retained for pointer-keyed same-frame reuse.
    std::uint64_t last_frame = 0;
    // Pointer identity needs a generation check because an allocator may
    // recycle an address for an unrelated ComponentElement.
    std::uint64_t generation = 0;
    // Hash-keyed entries are linked into ComponentCache::hash_lru.  Keeping
    // the iterator in the value makes a hit an allocation-free list splice.
    std::list<CacheId>::iterator lru_it{};
    bool lru_linked = false;

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

    // ── Whole-entry blit-skip record (write-epoch proof) ────────────
    // After a successful cells blit, we snapshot WHERE we blitted
    // (canvas uid, absolute position, span, row range, effective clip)
    // and the canvas's write-epoch counter. On the next frame, if the
    // geometry is identical AND no row in our range carries an epoch
    // greater than the snapshot, then nothing (no clear, no other
    // component, no direct write) touched those rows since our blit —
    // the canvas provably still holds our exact bytes AND the
    // last_col_/max_y_ bookkeeping our blit implied (clear_below
    // preserves both together; every reset path stamps). The entire
    // per-row bulk_eq/memcpy loop can be skipped: O(rows) integer
    // compares instead of O(rows × width) byte compares. This is what
    // makes a tall frozen prefix O(~zero) per frame instead of the
    // dominant render_tree cost on a long streaming turn.
    //
    // Correctness never depends on the skip: any doubt (geometry moved,
    // clip changed, epoch advanced, cells recaptured → record reset)
    // falls through to the byte-compare path that has always run.
    std::uint64_t     blit_skip_uid   = 0;   // 0 = no record (uids start at 1)
    std::uint64_t     blit_skip_epoch = 0;
    int               blit_skip_x     = 0;
    int               blit_skip_y     = 0;
    int               blit_skip_w     = 0;
    int               blit_skip_y_lo  = 0;
    int               blit_skip_y_hi  = 0;
    Canvas::ClipBounds blit_skip_clip{};
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
    std::unordered_map<CacheId, ComponentCacheEntry> entries_by_hash;
    // Most-recently used at front. This is deliberately separate from the
    // hash table: touching an entry is O(1), and trimming needs no scan or
    // temporary index allocation.
    std::list<CacheId> hash_lru;
    std::uint64_t current_frame = 0;
    // Canvas width at the last top-level render_tree. A cached height is only
    // valid for the width it was measured at; when the terminal is resized the
    // hash-keyed heights (trusted width-agnostically by the O(1) measure fast
    // path) become stale, so a width change must invalidate them. -1 = never
    // rendered. See the invalidation in render_tree's top-level block.
    int last_canvas_width = -1;
};

inline ComponentCache& component_cache() {
    thread_local ComponentCache c;
    return c;
}

inline void clear_component_cache_impl() noexcept {
    // Component cache cells carry StylePool-local uint16_t ids. Replacing the
    // cache wholesale is both cheaper and safer than walking/remapping them.
    component_cache() = ComponentCache{};
}

inline void touch_hash_cache(ComponentCache& cache,
                             ComponentCacheEntry& entry) noexcept {
    if (entry.lru_linked)
        cache.hash_lru.splice(cache.hash_lru.begin(), cache.hash_lru,
                              entry.lru_it);
}

inline void erase_hash_cache_entry(ComponentCache& cache,
                                   std::unordered_map<CacheId, ComponentCacheEntry>::iterator it) {
    if (it->second.lru_linked)
        cache.hash_lru.erase(it->second.lru_it);
    cache.entries_by_hash.erase(it);
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
            touch_hash_cache(cache, it->second);
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
            it->second.height     = entry.height;
            it->second.last_frame = entry.last_frame;
            it->second.generation = entry.generation;
            it->second.result     = std::move(entry.result);
            touch_hash_cache(cache, it->second);
            return &it->second;
        }
        if (it != cache.entries_by_hash.end())
            erase_hash_cache_entry(cache, it);
        auto [iit, _] = cache.entries_by_hash.emplace(comp.hash_id,
                                                       std::move(entry));
        iit->second.lru_it = cache.hash_lru.insert(cache.hash_lru.begin(),
                                                    comp.hash_id);
        iit->second.lru_linked = true;
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

// ── flex enum mappers ───────────────────────────────────────────────────
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

// ── painters ────────────────────────────────────────────────────────────
void paint_border(Canvas& canvas, const BorderConfig& border, const Rect& rect, uint16_t style_id);

// What an element's painter needs: where it landed, and where to draw.
struct PaintCtx {
    Canvas&                                 canvas;
    StylePool&                              pool;
    const std::vector<layout::LayoutNode>&  layout_nodes;
    const layout::LayoutNode&               ln;
    const Rect&                             computed;
    int ax, ay, aw, ah;                     // absolute rect on the canvas
    Rect abs_rect;
};

void paint_box(const PaintCtx& c, const BoxElement& node);
void paint_text(const PaintCtx& c, const TextElement& node);
void paint_list(const PaintCtx& c, const ElementList& node);
void paint_list_ref(const PaintCtx& c, const ElementListRef& node);
void paint_component(const PaintCtx& c, const ComponentElement& node);

} // namespace render_detail
} // namespace maya
