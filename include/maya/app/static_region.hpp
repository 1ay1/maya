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

} // namespace maya
