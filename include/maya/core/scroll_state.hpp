#pragma once
// maya::ScrollState — viewport scroll position for one or two axes.
//
// Plain data + behavior. No Signal coupling. Works equally well as a
// field on a Program<P> Model or as a captured local in the simple
// run(event_fn, render_fn) API.
//
// Two ways to drive it:
//
//   1. AUTO-DISPATCH (default): just declare the state, place the bars
//      and viewport via the DSL, and the run() loops forward events
//      to the state automatically. No boilerplate.
//
//        ScrollState s;
//        run({.title = "demo"},
//            [&](const Event& ev) {
//                if (key(ev, 'q')) return false;
//                return true;
//            },
//            [&] { return content | dsl::scroll(s, /*h=*/8); });
//
//   2. MANUAL (set auto_dispatch = false): call state.handle(*ev) from
//      your event handler. Useful when you have multiple independent
//      scroll regions and need fine-grained routing.
//
// max_x and max_y are filled by the renderer after layout. The painted
// regions (viewport, scrollbars) are recorded as lists in bars_h/bars_v
// so a single state can drive many bars (the styles showcase shows the
// same scroll position through 13 visually distinct bars at once).

#include "../terminal/input.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace maya {

struct ScrollState;

namespace detail {
// Signaled by the renderer's writeback when a ScrollState's max_x or
// max_y changes during a render. The run() loops consult this AFTER
// paint and trigger an immediate re-render so the next frame's view
// function sees the fresh max_* values.
inline thread_local bool scroll_writeback_dirty = false;

// Monotonic generation counter incremented at the start of every
// render. ScrollState compares against its own `paint_gen_seen` to
// decide whether to clear its bar lists on the next writeback (i.e.,
// each render's writeback collects a fresh list of painted bars).
inline thread_local int paint_generation = 0;

// Live scroll states for the current paint generation. Populated by
// the writeback; consumed by the run() loops to auto-dispatch events.
// Cleared at the start of each render so dropped states stop receiving
// events automatically.
//
// Exposed via a function-local thread_local rather than an
// `inline thread_local` object: GCC on PE/COFF (MinGW) emits the TLS
// init wrapper for a dynamically-initialized inline thread_local
// without COMDAT linkage, so every TU that includes this header
// defines `TLS init function for ...live_scroll_states`, and the
// linker rejects the duplicates (surfaces under LTO). A function-local
// thread_local keeps the init wrapper local to this inline function,
// which is properly deduplicated.
inline std::vector<ScrollState*>& live_scroll_states() {
    static thread_local std::vector<ScrollState*> states;
    return states;
}
}  // namespace detail

// Cell-space rectangle (canvas coordinates, 0-based). Written by the
// renderer's writeback so hit-testing in mouse handlers doesn't need to
// know where the bar/viewport got painted.
struct ScrollRect {
    int x = 0, y = 0, w = 0, h = 0;
    [[nodiscard]] constexpr bool empty() const noexcept { return w <= 0 || h <= 0; }
    [[nodiscard]] constexpr bool contains(int cx, int cy) const noexcept {
        return !empty() && cx >= x && cx < x + w && cy >= y && cy < y + h;
    }
};

struct ScrollState {
    // -- Current scroll offset (top-left of viewport in content coords) --
    int x = 0;
    int y = 0;

    // -- Maximum scrollable offset, written back by the runtime after the
    //    layout pass. Equal to max(0, content_size - viewport_size) on
    //    each axis. 0 means content fits, no scrolling possible. --
    int max_x = 0;
    int max_y = 0;

    // -- How many rows/cols a single step moves. Default 1; bump for
    //    "fast" scroll. handle() multiplies these by direction. --
    int step_x = 1;
    int step_y = 1;

    // -- Painted regions, refilled by the renderer's writeback every
    //    frame. A single state can drive many bars (e.g., the styles
    //    showcase: 13 differently-styled bars all reading from one
    //    state). Hit-testing iterates the list version.
    //    `bar_h_bounds` / `bar_v_bounds` are convenience handles for
    //    the common single-bar case (= bars_h.front() when nonempty).
    //    `viewport_bounds` is the painted viewport rect. --
    std::vector<ScrollRect> bars_h;
    std::vector<ScrollRect> bars_v;
    ScrollRect              bar_h_bounds    = {};
    ScrollRect              bar_v_bounds    = {};
    ScrollRect              viewport_bounds = {};

    // -- Auto-dispatch. When true (default), the run() loops forward
    //    every input event to this state's handle() automatically as
    //    long as it was painted in the previous frame. Set false for
    //    fine-grained routing in apps with multiple scroll regions. --
    bool auto_dispatch = true;

    // -- Internal drag state — set on Press, cleared on Release. The
    //    captured `drag_bar` is the specific bar the drag started on,
    //    so dimensions stay stable even if subsequent paints rearrange
    //    the bar list. --
    bool       dragging_h    = false;
    bool       dragging_v    = false;
    ScrollRect drag_bar      = {};
    int        drag_offset_h = 0;
    int        drag_offset_v = 0;

    // -- Paint-generation tracking for vector lifecycle. Compare to
    //    detail::paint_generation; when they differ, clear bars_h/v
    //    on the next writeback (start-of-frame reset). --
    int paint_gen_seen = -1;

    // ------------------------------------------------------------------
    // Position queries + clamping
    // ------------------------------------------------------------------

    void clamp() noexcept {
        x = std::clamp(x, 0, max_x);
        y = std::clamp(y, 0, max_y);
    }

    [[nodiscard]] bool at_top()    const noexcept { return y <= 0; }
    [[nodiscard]] bool at_bottom() const noexcept { return y >= max_y; }
    [[nodiscard]] bool at_left()   const noexcept { return x <= 0; }
    [[nodiscard]] bool at_right()  const noexcept { return x >= max_x; }

    [[nodiscard]] bool is_dragging() const noexcept {
        return dragging_h || dragging_v;
    }

    // ------------------------------------------------------------------
    // Imperative movement
    // ------------------------------------------------------------------

    void scroll_by(int dx, int dy) noexcept {
        x = std::clamp(x + dx, 0, max_x);
        y = std::clamp(y + dy, 0, max_y);
    }
    void scroll_to(int nx, int ny) noexcept {
        x = std::clamp(nx, 0, max_x);
        y = std::clamp(ny, 0, max_y);
    }
    void scroll_to_top()         noexcept { y = 0; }
    void scroll_to_bottom()      noexcept { y = max_y; }
    void scroll_to_left()        noexcept { x = 0; }
    void scroll_to_right()       noexcept { x = max_x; }
    void scroll_to_origin()      noexcept { x = 0; y = 0; }

    void end_drag() noexcept {
        dragging_h = false;
        dragging_v = false;
    }

    // ------------------------------------------------------------------
    // Thumb dimensions for a specific bar rect — same math the
    // scrollbar widget uses so click + drag stays in lock-step with
    // the rendered thumb.
    // ------------------------------------------------------------------

    [[nodiscard]] int thumb_w(const ScrollRect& bar) const noexcept {
        if (bar.w <= 0) return 0;
        const int content_w = bar.w + max_x;
        return std::max(1, bar.w * bar.w / std::max(1, content_w));
    }
    [[nodiscard]] int thumb_h(const ScrollRect& bar) const noexcept {
        if (bar.h <= 0) return 0;
        const int content_h = bar.h + max_y;
        return std::max(1, bar.h * bar.h / std::max(1, content_h));
    }

    // ------------------------------------------------------------------
    // Click / drag → scroll position. THUMB-RELATIVE: keeps the cursor
    // glued to the same point on the thumb (no lag).
    //
    // Public so apps with custom scrollbars can reuse the math: capture
    // a bar rect via a writeback, set drag_offset_*, then call these on
    // each move.
    // ------------------------------------------------------------------

    void set_x_from_h_bar_click(const ScrollRect& bar, int cursor_col) noexcept {
        if (bar.w <= 0 || max_x <= 0) return;
        const int tw = thumb_w(bar);
        const int track = std::max(1, bar.w - tw);
        const int new_thumb_x = std::clamp(
            cursor_col - bar.x - drag_offset_h,
            0, bar.w - tw);
        x = std::clamp(new_thumb_x * max_x / track, 0, max_x);
    }
    void set_y_from_v_bar_click(const ScrollRect& bar, int cursor_row) noexcept {
        if (bar.h <= 0 || max_y <= 0) return;
        const int th = thumb_h(bar);
        const int track = std::max(1, bar.h - th);
        const int new_thumb_y = std::clamp(
            cursor_row - bar.y - drag_offset_v,
            0, bar.h - th);
        y = std::clamp(new_thumb_y * max_y / track, 0, max_y);
    }

    // ------------------------------------------------------------------
    // Bar-list queries
    // ------------------------------------------------------------------

    [[nodiscard]] const ScrollRect* h_bar_at(int cx, int cy) const noexcept {
        for (const auto& b : bars_h) if (b.contains(cx, cy)) return &b;
        return nullptr;
    }
    [[nodiscard]] const ScrollRect* v_bar_at(int cx, int cy) const noexcept {
        for (const auto& b : bars_v) if (b.contains(cx, cy)) return &b;
        return nullptr;
    }

    // ------------------------------------------------------------------
    // Key handling
    // ------------------------------------------------------------------
    //
    //   ↑/↓        — move vertically by step_y
    //   ←/→        — move horizontally by step_x
    //   PgUp/PgDn  — move vertically by viewport_h
    //   Home/End   — jump to top / bottom
    //   Ctrl+Home  — jump to (0, 0)
    //   Ctrl+End   — jump to (max_x, max_y)
    //
    // viewport_h / viewport_w size the page-scroll steps; pass 0 for
    // an axis you don't care about (page-scroll becomes 1-row).

    [[nodiscard]] bool handle(const KeyEvent& ev,
                              int viewport_h = 0,
                              int viewport_w = 0) noexcept {
        auto* sk = std::get_if<SpecialKey>(&ev.key);
        if (!sk) return false;
        const int sx = std::max(1, step_x);
        const int sy = std::max(1, step_y);
        switch (*sk) {
            case SpecialKey::Up:       scroll_by(0, -sy);                  return true;
            case SpecialKey::Down:     scroll_by(0, +sy);                  return true;
            case SpecialKey::Left:     scroll_by(-sx, 0);                  return true;
            case SpecialKey::Right:    scroll_by(+sx, 0);                  return true;
            case SpecialKey::PageUp:   scroll_by(0, -std::max(1,viewport_h)); return true;
            case SpecialKey::PageDown: scroll_by(0, +std::max(1,viewport_h)); return true;
            case SpecialKey::Home:
                if (ev.mods.ctrl) scroll_to_origin();
                else              scroll_to_top();
                return true;
            case SpecialKey::End:
                if (ev.mods.ctrl) scroll_to(max_x, max_y);
                else              scroll_to_bottom();
                return true;
            default: return false;
        }
    }

    // ------------------------------------------------------------------
    // Mouse handling
    // ------------------------------------------------------------------
    //
    // Routes Press / Move / Release for click-and-drag scrollbars AND
    // wheel events for traditional scrolling. Returns true if consumed.

    [[nodiscard]] bool handle(const MouseEvent& me) noexcept {
        // SGR mouse coordinates are 1-based; bounds are 0-based.
        const int mx = me.x.value - 1;
        const int my = me.y.value - 1;

        // -- Release: end drag only on LEFT release. Right/Middle
        //    release mid-drag (context menu, etc.) is ignored. --
        if (me.kind == MouseEventKind::Release) {
            if (me.button == MouseButton::Left && is_dragging()) {
                end_drag();
                return true;
            }
            return false;
        }

        // -- Move: continue active drag using the bar captured at
        //    Press time (so re-paints don't desync the math). --
        if (me.kind == MouseEventKind::Move) {
            if (dragging_h) { set_x_from_h_bar_click(drag_bar, mx); return true; }
            if (dragging_v) { set_y_from_v_bar_click(drag_bar, my); return true; }
            return false;
        }

        if (me.kind != MouseEventKind::Press) return false;
        const int sx = std::max(1, step_x);
        const int sy = std::max(1, step_y);

        // -- Left-click on a scrollbar: start drag + jump.
        //    On-thumb press preserves offset (zero-lag drag); on-track
        //    press recenters thumb on cursor and uses thumb_w/2 offset
        //    for subsequent drag. --
        if (me.button == MouseButton::Left) {
            if (auto* bar = h_bar_at(mx, my); bar && max_x > 0) {
                dragging_v = false;
                dragging_h = true;
                drag_bar   = *bar;
                const int tw = thumb_w(*bar);
                const int track = std::max(1, bar->w - tw);
                const int thumb_x_in_bar = track * x / std::max(1, max_x);
                const int click_in_bar = mx - bar->x;
                if (click_in_bar >= thumb_x_in_bar && click_in_bar < thumb_x_in_bar + tw) {
                    drag_offset_h = click_in_bar - thumb_x_in_bar;
                } else {
                    drag_offset_h = tw / 2;
                    set_x_from_h_bar_click(*bar, mx);
                }
                return true;
            }
            if (auto* bar = v_bar_at(mx, my); bar && max_y > 0) {
                dragging_h = false;
                dragging_v = true;
                drag_bar   = *bar;
                const int th = thumb_h(*bar);
                const int track = std::max(1, bar->h - th);
                const int thumb_y_in_bar = track * y / std::max(1, max_y);
                const int click_in_bar = my - bar->y;
                if (click_in_bar >= thumb_y_in_bar && click_in_bar < thumb_y_in_bar + th) {
                    drag_offset_v = click_in_bar - thumb_y_in_bar;
                } else {
                    drag_offset_v = th / 2;
                    set_y_from_v_bar_click(*bar, my);
                }
                return true;
            }
            return false;
        }

        // -- Wheel routing --
        //   (1) over any h-bar     → horizontal scroll (modifier-free)
        //   (2) ScrollLeft/Right    → horizontal scroll (native)
        //   (3) Shift/Alt+wheel     → horizontal scroll (fallback)
        //   (4) plain wheel         → vertical
        //
        // Multi-pane apps: a wheel event must scroll exactly ONE
        // state — the one whose viewport sits under the cursor.
        // Without a cursor-vs-viewport check, every painted state with
        // auto_dispatch=true would consume the same wheel event and
        // every pane would scroll in unison. The h-bar shortcut still
        // works regardless of viewport because it's an explicit "hover
        // the horizontal bar" gesture.
        const bool over_hbar = h_bar_at(mx, my) != nullptr;
        if (over_hbar) {
            if (me.button == MouseButton::ScrollUp)    { scroll_by(-sx, 0); return true; }
            if (me.button == MouseButton::ScrollDown)  { scroll_by(+sx, 0); return true; }
            if (me.button == MouseButton::ScrollLeft)  { scroll_by(-sx, 0); return true; }
            if (me.button == MouseButton::ScrollRight) { scroll_by(+sx, 0); return true; }
        }
        // For all other wheel routing, require the cursor to be inside
        // this state's painted viewport (or v-bar). If the viewport
        // wasn't painted yet (paint_gen_seen < 0) we accept the event —
        // a single-pane app needs to scroll before its first paint
        // generation completes.
        const bool over_vbar = v_bar_at(mx, my) != nullptr;
        const bool unpainted = viewport_bounds.w == 0 && viewport_bounds.h == 0;
        const bool in_viewport = unpainted || over_vbar
            || viewport_bounds.contains(mx, my);
        if (!in_viewport) return false;
        if (me.button == MouseButton::ScrollLeft)  { scroll_by(-sx, 0); return true; }
        if (me.button == MouseButton::ScrollRight) { scroll_by(+sx, 0); return true; }
        if (me.mods.shift || me.mods.alt) {
            if (me.button == MouseButton::ScrollUp)   { scroll_by(-sx, 0); return true; }
            if (me.button == MouseButton::ScrollDown) { scroll_by(+sx, 0); return true; }
        }
        if (me.button == MouseButton::ScrollUp)   { scroll_by(0, -sy); return true; }
        if (me.button == MouseButton::ScrollDown) { scroll_by(0, +sy); return true; }
        return false;
    }

    // ------------------------------------------------------------------
    // Generic event dispatcher — accepts any Event. Used by the
    // framework's auto-dispatch path. Returns true if consumed.
    // ------------------------------------------------------------------

    [[nodiscard]] bool handle_event(const Event& ev) noexcept {
        // Auto-dispatch path: size page-scroll steps from the recorded
        // viewport rect (or fall back to 1 if not yet painted).
        if (auto* k = std::get_if<KeyEvent>(&ev))   return handle(*k, viewport_bounds.h, viewport_bounds.w);
        if (auto* m = std::get_if<MouseEvent>(&ev)) return handle(*m);
        return false;
    }
};

} // namespace maya
