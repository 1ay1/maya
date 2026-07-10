#pragma once
// maya::hit — paint-time hit-region registry.
//
// THE PROBLEM. maya's TEA loop hands mouse events to the app as raw
// (x, y) cell coordinates. Fullscreen apps with clickable chrome
// (footer hints, tab bars, table headers, panel bodies) historically
// had to reverse-engineer where the renderer painted each target by
// re-deriving the layout math by hand — leading offsets, per-tab label
// widths, gap accounting — and keep that mirror in lockstep with the
// widget forever. Every visual tweak (a chip gaining a pad cell, a
// header growing a glyph) silently broke the mirror; the bug class is
// "click misses / hits the wrong thing after a restyle".
//
// THE FIX. Let the renderer itself record where things landed. A box
// tagged with a HitId (via the `| hit(id)` DSL pipe) gets its ABSOLUTE
// painted rect appended to a per-frame registry during paint — the
// same coordinates the terminal reports mouse events in (fullscreen;
// inline hosts subtract inline_mouse_dy() first, exactly as they
// already do for scroll states). The app then asks:
//
//     if (auto id = maya::hit_test(mx, my)) {
//         switch (*id) { case kFooterQuit: ...; }
//     }
//
// and the answer is CORRECT BY CONSTRUCTION — it came from the same
// layout pass that put the pixels on screen. No mirror, no lockstep.
//
// ID SCHEME. HitId is a raw uint64 the host fully owns. For a static
// target, use an enum value. For a parameterized target (row N of a
// table, tab K of a bar), pack the parameter into the id — the helper
// hit_id(kind, index) packs a 32-bit kind + 32-bit index, and
// hit_kind()/hit_index() unpack. Collisions are the host's contract,
// same as any enum.
//
// OVERLAP. Registration order is paint order, and paint order is
// z-order (children paint after parents, overlays after content).
// hit_test returns the LAST registered rect containing the point —
// the topmost / innermost target — so a row id nested inside a panel
// id resolves to the row, and a modal overlay painted last wins over
// everything under it. The registry is a flat vector scanned backward:
// tens of entries at most in a real frame, so the scan is nanoseconds
// and needs no spatial index.
//
// LIFECYCLE. The registry is cleared at the top of every render pass
// (same discipline as detail::live_scroll_states) and repopulated
// during paint. Between frames it holds the LAST PAINTED frame's
// geometry — which is precisely what a mouse event that arrives
// between frames must be tested against. Thread-local, single-UI-
// thread ownership; same model as every other renderer registry.
//
// COMPONENT-CACHE NOTE. A hash-keyed ComponentElement that takes the
// cell-blit fast path does NOT re-paint its subtree, so hit ids INSIDE
// the cached subtree are not re-registered on cached frames. Tag boxes
// at or above the component boundary (the panel, the row container) —
// not deep inside cell-cached content. In practice chrome targets are
// exactly the boxes hosts already build fresh each frame.

#include <cstdint>
#include <optional>
#include <vector>

namespace maya {

/// Host-owned hit-target identity. 0 is reserved as "no id".
using HitId = std::uint64_t;

/// Pack a (kind, index) pair into a HitId — the idiom for parameterized
/// targets like table rows: `hit_id(kProcRow, row_index)`.
[[nodiscard]] constexpr HitId hit_id(std::uint32_t kind,
                                     std::uint32_t index = 0) noexcept {
    return (static_cast<std::uint64_t>(kind) << 32)
         | static_cast<std::uint64_t>(index);
}
[[nodiscard]] constexpr std::uint32_t hit_kind(HitId id) noexcept {
    return static_cast<std::uint32_t>(id >> 32);
}
[[nodiscard]] constexpr std::uint32_t hit_index(HitId id) noexcept {
    return static_cast<std::uint32_t>(id & 0xFFFFFFFFULL);
}

/// One registered region: the absolute painted rect of a hit-tagged box.
struct HitRegion {
    HitId id = 0;
    int x = 0, y = 0, w = 0, h = 0;
    [[nodiscard]] constexpr bool contains(int cx, int cy) const noexcept {
        return cx >= x && cx < x + w && cy >= y && cy < y + h;
    }
};

namespace detail {
// Function-local thread_local for the same PE/COFF TLS-init-COMDAT
// reason documented at live_scroll_states().
inline std::vector<HitRegion>& hit_regions() {
    static thread_local std::vector<HitRegion> regions;
    return regions;
}
}  // namespace detail

/// Topmost hit target at (x, y) in absolute cell coordinates, or
/// nullopt. Backward scan = last painted wins (z-order).
[[nodiscard]] inline std::optional<HitId> hit_test(int x, int y) noexcept {
    const auto& rs = detail::hit_regions();
    for (auto it = rs.rbegin(); it != rs.rend(); ++it)
        if (it->contains(x, y)) return it->id;
    return std::nullopt;
}

/// Painted rect of the FIRST region registered under `id` this frame,
/// or nullopt if it wasn't painted. Reverse query — lets a host anchor
/// a popup/tooltip to a target without knowing where layout put it.
[[nodiscard]] inline std::optional<HitRegion> hit_rect(HitId id) noexcept {
    for (const auto& r : detail::hit_regions())
        if (r.id == id) return r;
    return std::nullopt;
}

}  // namespace maya
