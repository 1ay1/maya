#pragma once
// maya::StaticRegion — Freeze completed content into terminal scrollback
//
// When content is "done" (e.g., a completed chat message), freeze it into
// the static region above the active render area. Frozen content is written
// once and never re-rendered, saving CPU and enabling terminal scrollback.
//
// Usage:
//   StaticRegion region;
//   region.freeze(render_message(msg), width, pool);
//   // On next frame, App outputs frozen content above active area

#include <string>
#include <utility>

#include "../element/element.hpp"
#include "../render/canvas.hpp"
#include "../render/renderer.hpp"
#include "../render/serialize.hpp"
#include "../style/theme.hpp"
#include "../terminal/ansi.hpp"

namespace maya {

class StaticRegion {
    std::string pending_;
    int frozen_rows_ = 0;

public:
    /// Freeze an Element: render it once, queue ANSI output for the next frame.
    void freeze(const Element& elem, int width, StylePool& pool,
                const Theme& theme = theme::dark) {
        // Render into a temporary canvas
        Canvas canvas(width, 500, &pool);
        canvas.clear();
        render_tree(elem, canvas, pool, theme, /*auto_height=*/true);
        int h = content_height(canvas);
        if (h <= 0) return;

        // Serialize to ANSI
        Canvas view(width, h, &pool);
        view.clear();
        render_tree(elem, view, pool, theme);
        serialize(view, pool, pending_);
        pending_ += "\r\n";

        frozen_rows_ += h;
    }

    /// Check if there's frozen content waiting to be flushed.
    [[nodiscard]] bool has_pending() const noexcept { return !pending_.empty(); }

    /// Take the pending output (called by App::render_frame).
    [[nodiscard]] std::string take_pending() { return std::exchange(pending_, {}); }

    /// Total rows frozen so far.
    [[nodiscard]] int frozen_rows() const noexcept { return frozen_rows_; }

    /// Reset (used on terminal resize).
    void reset() noexcept {
        frozen_rows_ = 0;
        pending_.clear();
    }
};

// ============================================================================
// StaticSplit — DECSTBM partial-update region
// ============================================================================
// Splits the terminal into two stacked bands:
//
//   ┌──────────────────────────┐  rows [1 .. frozen_rows]
//   │  FROZEN  (write-once)     │  completed content — never repainted, never
//   │                          │  scrolled by the terminal
//   ├──────────────────────────┤
//   │  ACTIVE  (redrawn)        │  rows [frozen_rows+1 .. screen_h] — the live
//   │                          │  band; this is the only region the renderer
//   └──────────────────────────┘  diffs + repaints each frame
//
// The split is enforced with DECSTBM: the scrolling region is pinned to the
// ACTIVE band, so the terminal physically cannot move the frozen rows. As more
// content completes, push more rows into the frozen band — the margin slides
// down and the active band shrinks. This is the partial-terminal-update model
// (a.k.a. "static component"): completed UI is paid for once.
//
// This is a *wire-bytes generator*, not wired into the inline App loop — it is
// a reusable primitive a host composes into its own frame emit. Lifecycle:
//
//   StaticSplit s;
//   s.begin(screen_h, out);                 // arm DECSTBM for the whole screen
//   s.freeze(done_elem, w, pool, out);      // promote rows into the frozen band
//   s.draw_active(live_elem, w, pool, out);  // paint only the active band
//   ... per frame: s.draw_active(...) ...
//   s.end(out);                              // restore full-screen region
//
// Invariant: frozen_rows() never exceeds screen_h - 1 (at least one active
// row is always preserved so the cursor + live content have somewhere to go).

class StaticSplit {
    int  screen_h_   = 0;
    int  frozen_     = 0;     // rows currently frozen at the top
    bool armed_      = false;

public:
    /// Arm the split for a screen of `screen_h` rows. Initially the whole
    /// screen is the active region (frozen = 0). Idempotent per height.
    void begin(int screen_h, std::string& out) {
        screen_h_ = screen_h > 0 ? screen_h : 0;
        frozen_   = 0;
        armed_    = screen_h_ > 0;
        if (!armed_) return;
        // Active region = the full screen until something is frozen.
        ansi::write_scroll_region(out, 1, screen_h_);
        // Home the cursor into the (currently full-screen) active band.
        ansi::write_move_to(out, 1, 1);
    }

    /// Render `elem` once and promote its rows into the FROZEN band, sliding
    /// the active-region top margin down by that many rows. The frozen rows
    /// are emitted at their absolute position and will never be repainted.
    /// Returns the number of rows frozen by THIS call (0 if it would leave no
    /// active row, or the element is empty).
    int freeze(const Element& elem, int width, StylePool& pool,
               std::string& out, const Theme& theme = theme::dark) {
        if (!armed_ || width <= 0) return 0;

        // Measure natural height.
        Canvas measure(width, screen_h_, &pool);
        measure.clear();
        render_tree(elem, measure, pool, theme, /*auto_height=*/true);
        int h = content_height(measure);
        if (h <= 0) return 0;

        // Never consume the last active row.
        const int max_freeze = screen_h_ - 1 - frozen_;
        if (max_freeze <= 0) return 0;
        if (h > max_freeze) h = max_freeze;

        // Render exactly h rows and serialize.
        Canvas view(width, h, &pool);
        view.clear();
        render_tree(elem, view, pool, theme);
        std::string body;
        serialize(view, pool, body);

        // Temporarily release the scroll region so we can write into rows the
        // current margin would otherwise protect, place the cursor at the new
        // frozen band's first row, emit, then re-arm the (now-narrower) band.
        ansi::write_scroll_region_reset(out);
        ansi::write_move_to(out, 1, frozen_ + 1);   // 1-based row
        out += body;

        frozen_ += h;
        // Re-pin the active region below the frozen band.
        ansi::write_scroll_region(out, frozen_ + 1, screen_h_);
        ansi::write_move_to(out, 1, frozen_ + 1);
        return h;
    }

    /// Repaint the ACTIVE band with `elem`. Clears the band first (so stale
    /// content from a taller previous frame is erased), then stamps the
    /// element clipped to the band. Frozen rows are untouched.
    void draw_active(const Element& elem, int width, StylePool& pool,
                     std::string& out, const Theme& theme = theme::dark) {
        if (!armed_ || width <= 0) return;
        const int top    = frozen_ + 1;          // 1-based first active row
        const int height = screen_h_ - frozen_;  // active band height
        if (height <= 0) return;

        Canvas band(width, height, &pool);
        band.clear();
        render_tree(elem, band, pool, theme, /*auto_height=*/true);
        std::string body;
        serialize(band, pool, body);

        // Erase the active band, then paint it. Move per-row to keep frozen
        // rows safe even if the body under-fills the band.
        for (int r = 0; r < height; ++r) {
            ansi::write_move_to(out, 1, top + r);
            out += "\x1b[2K";   // erase entire line
        }
        ansi::write_move_to(out, 1, top);
        out += body;
    }

    /// Trim the TOP `n` rows of the frozen band into native scrollback.
    ///
    /// This is the DECSTBM analog of a host scrollback commit: when the
    /// frozen prefix grows past a budget, the oldest rows must graduate into
    /// the terminal's own scrollback. We do it by (1) releasing the scroll
    /// region to the full screen, (2) homing to the top and scrolling the
    /// WHOLE screen up by `n` — which feeds exactly those `n` physical top
    /// rows into native scrollback — then (3) re-pinning the now-`n`-shorter
    /// frozen band. Because the rows leave via a real scroll (not a repaint),
    /// no committed row is ever rewritten: the duplicate-strand failure mode
    /// of a from-the-top re-emit cannot occur here.
    ///
    /// Returns the number of rows actually trimmed (clamped so at least one
    /// frozen row of context can remain; pass a value <= frozen_rows()).
    int commit_top(int n, std::string& out) {
        if (!armed_ || n <= 0) return 0;
        if (n > frozen_) n = frozen_;
        if (n <= 0) return 0;

        ansi::write_scroll_region_reset(out);
        ansi::write_move_to(out, 1, 1);
        ansi::write_scroll_up(out, n);          // SU: top n rows → scrollback
        frozen_ -= n;
        // Re-pin the active region below the now-shorter frozen band.
        ansi::write_scroll_region(out, frozen_ + 1, screen_h_);
        ansi::write_move_to(out, 1, frozen_ + 1);
        return n;
    }

    /// Tear down: restore the full-screen scroll region and drop below the
    /// content. Always call before handing the terminal to another program.
    void end(std::string& out) {
        if (!armed_) return;
        ansi::write_scroll_region_reset(out);
        ansi::write_move_to(out, 1, screen_h_);
        armed_ = false;
    }

    [[nodiscard]] int  frozen_rows() const noexcept { return frozen_; }
    [[nodiscard]] int  active_rows() const noexcept {
        return armed_ ? screen_h_ - frozen_ : 0;
    }
    [[nodiscard]] bool armed() const noexcept { return armed_; }
};

} // namespace maya
