#pragma once
// maya::overlay — absolute-positioned, z-ordered floating layers
//
// The element/flex tree composes content in *flow*. Some UI — tooltips,
// dropdown menus, command palettes, toasts, context menus — must float at an
// absolute screen position, on its own compositing layer, painted AFTER the
// main tree regardless of where it sits in the source. That is what this
// header provides: a post-tree overlay compositor.
//
// Model:
//   * The main element tree renders normally into the canvas (flow layer 0).
//   * Zero or more `OverlayLayer`s are stamped on top, each at an absolute
//     anchor, in ascending z-index order (stable for equal z). Later layers
//     paint over earlier ones.
//   * Each layer is measured (natural size at a max width) then positioned by
//     its `Anchor` plus a pixel `(dx, dy)` nudge, and clamped on-screen.
//
// Why a separate compositor instead of a flex node? Absolute position breaks
// flex's size-propagation contract: a floating layer must NOT influence the
// layout of the flow content beneath it, and must be able to overhang it. A
// post-pass that paints with `render_tree_at` (clip + offset, no clear) keeps
// the flow layer pristine and the float free.
//
// Config is a *structural NTTP* (`OverlayCfg`) so anchor + z + offset are
// baked at compile time where the call site knows them, and validated by a
// consteval factory. A runtime variant (`overlay_at`) covers positions only
// known at run time (e.g. a mouse cursor).
//
// Cost: each layer is one extra `render_tree_at` into the already-allocated
// canvas. No heap on the hot path beyond the layer vector the caller builds.

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "../element/element.hpp"
#include "../render/canvas.hpp"
#include "../render/renderer.hpp"
#include "../render/serialize.hpp"
#include "../style/theme.hpp"

namespace maya {

// ============================================================================
// Anchor — which screen edge/corner a layer is pinned to
// ============================================================================

enum class Anchor : uint8_t {
    TopLeft,      TopCenter,    TopRight,
    CenterLeft,   Center,       CenterRight,
    BottomLeft,   BottomCenter, BottomRight,
};

// ============================================================================
// OverlayCfg — structural NTTP carried by the overlay_<> pipe
// ============================================================================
// z: higher paints later (on top). dx/dy: cell nudge from the anchor point
// (dx grows rightward, dy downward). max_w: width budget the layer is measured
// at (0 ⇒ full screen width). clamp: keep fully on-screen if it would overhang.

struct OverlayCfg {
    Anchor  anchor = Anchor::TopLeft;
    int     z      = 0;
    int     dx     = 0;
    int     dy     = 0;
    int     max_w  = 0;     // 0 ⇒ screen width
    bool    clamp  = true;
};

// consteval validator — catches an absurd config at the call site.
[[nodiscard]] consteval OverlayCfg overlay_cfg(Anchor a, int z = 0, int dx = 0,
                                               int dy = 0, int max_w = 0,
                                               bool clamp = true) {
    if (max_w < 0) throw "overlay_cfg: max_w must be >= 0";
    return OverlayCfg{a, z, dx, dy, max_w, clamp};
}

// ============================================================================
// OverlayLayer — one positioned float (runtime payload)
// ============================================================================

struct OverlayLayer {
    Element  content;
    OverlayCfg cfg{};
};

// `elem | overlay_<Cfg>` — attach a compile-time config to an element, yielding
// an OverlayLayer ready to push into an OverlayStack. The NTTP keeps the anchor
// and z in the type system at the call site.
template <OverlayCfg Cfg>
struct OverlayTag {};

template <OverlayCfg Cfg>
inline constexpr OverlayTag<Cfg> overlay_{};

template <OverlayCfg Cfg>
[[nodiscard]] inline OverlayLayer operator|(Element e, OverlayTag<Cfg>) {
    return OverlayLayer{std::move(e), Cfg};
}

// Runtime-positioned layer (anchor TopLeft, explicit dx/dy) — for positions
// only known at run time (mouse cursor, hit-test result).
[[nodiscard]] inline OverlayLayer overlay_at(Element e, int x, int y,
                                             int z = 0, int max_w = 0,
                                             bool clamp = true) {
    return OverlayLayer{std::move(e),
                        OverlayCfg{Anchor::TopLeft, z, x, y, max_w, clamp}};
}

// ============================================================================
// OverlayStack — base tree + z-sorted floats, composited to a canvas
// ============================================================================

class OverlayStack {
    std::vector<OverlayLayer> layers_;

public:
    OverlayStack() = default;

    OverlayStack& push(OverlayLayer layer) {
        layers_.push_back(std::move(layer));
        return *this;
    }
    OverlayStack& operator+=(OverlayLayer layer) { return push(std::move(layer)); }

    [[nodiscard]] bool empty() const noexcept { return layers_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return layers_.size(); }

    // Composite onto a canvas already painted with the flow layer. Stamps each
    // overlay in ascending z (stable for equal z), measuring + positioning by
    // anchor. The canvas dimensions define the screen rect. No clear.
    void composite(Canvas& canvas, StylePool& pool,
                   const Theme& theme = theme::dark) const {
        if (layers_.empty()) return;
        const int sw = canvas.width();
        const int sh = canvas.height();
        if (sw <= 0 || sh <= 0) return;

        // Stable z-sort by index so equal-z layers keep insertion order.
        std::vector<const OverlayLayer*> order;
        order.reserve(layers_.size());
        for (const auto& l : layers_) order.push_back(&l);
        std::stable_sort(order.begin(), order.end(),
                         [](const OverlayLayer* a, const OverlayLayer* b) {
                             return a->cfg.z < b->cfg.z;
                         });

        for (const auto* lp : order) {
            const OverlayLayer& l = *lp;
            const int budget = l.cfg.max_w > 0 ? std::min(l.cfg.max_w, sw) : sw;

            // Measure natural size at the width budget by rendering into a
            // scratch canvas (thread-local, reused across calls/frames).
            auto [mw, mh] = measure(l.content, budget, sh, pool, theme);
            if (mw <= 0 || mh <= 0) continue;

            // Resolve anchor → top-left origin, then apply (dx, dy) nudge.
            auto [ax, ay] = anchor_origin(l.cfg.anchor, sw, sh, mw, mh);
            int x = ax + l.cfg.dx;
            int y = ay + l.cfg.dy;

            if (l.cfg.clamp) {
                x = std::clamp(x, 0, std::max(0, sw - mw));
                y = std::clamp(y, 0, std::max(0, sh - mh));
            }

            // Stamp on top (clipped to the canvas, no clear).
            render_tree_at(l.content, canvas, pool, theme, x, y, mw, mh);
        }
    }

private:
    // Render into a scratch canvas to learn the natural (w, h) at a width
    // budget. Width is the longest painted row; height is content_height.
    static std::pair<int, int> measure(const Element& e, int budget, int max_h,
                                        StylePool& pool, const Theme& theme) {
        thread_local Canvas scratch(1, 1, nullptr);
        scratch.set_style_pool(&pool);
        scratch.resize(budget, std::max(1, max_h));
        scratch.clear();
        render_tree(e, scratch, pool, theme, /*auto_height=*/true);
        const int h = std::min(content_height(scratch), max_h);
        int w = 0;
        for (int row = 0; row < h; ++row)
            w = std::max(w, scratch.last_content_col(row) + 1);
        return {std::min(w, budget), h};
    }

    static std::pair<int, int> anchor_origin(Anchor a, int sw, int sh,
                                             int mw, int mh) noexcept {
        int cx = 0, cy = 0;
        switch (a) {
            case Anchor::TopLeft:      cx = 0;              cy = 0;              break;
            case Anchor::TopCenter:    cx = (sw - mw) / 2;  cy = 0;              break;
            case Anchor::TopRight:     cx = sw - mw;        cy = 0;              break;
            case Anchor::CenterLeft:   cx = 0;              cy = (sh - mh) / 2;  break;
            case Anchor::Center:       cx = (sw - mw) / 2;  cy = (sh - mh) / 2;  break;
            case Anchor::CenterRight:  cx = sw - mw;        cy = (sh - mh) / 2;  break;
            case Anchor::BottomLeft:   cx = 0;              cy = sh - mh;        break;
            case Anchor::BottomCenter: cx = (sw - mw) / 2;  cy = sh - mh;        break;
            case Anchor::BottomRight:  cx = sw - mw;        cy = sh - mh;        break;
        }
        return {cx, cy};
    }
};

} // namespace maya
