#pragma once
// maya::print / maya::inline_run — Inline (non-fullscreen) rendering
//
// Two APIs for rendering maya elements without taking over the terminal:
//
//   maya::print(element)
//       One-shot: render an element tree to stdout and return.
//       Great for styled CLI output — tables, reports, status cards.
//
//   maya::inline_run({.fps = 30}, [&](float dt) {
//       return text("frame " + std::to_string(n++));
//   });
//       Live: re-renders inline at the given fps. Return the element tree
//       each frame. Call maya::quit() to stop the loop.
//
// Neither API uses the alternate screen buffer. Output stays in the
// terminal's scrollback like normal program output.

#include <chrono>
#include <concepts>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>

#include "../core/types.hpp"
#include "../element/element.hpp"
#include "../render/canvas.hpp"
#include "../render/renderer.hpp"
#include "../render/serialize.hpp"
#include "../style/theme.hpp"
#include "../terminal/ansi.hpp"
#include "run.hpp"  // for maya::quit() / detail::quit_requested

#ifdef __unix__
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace maya {

// ── Terminal width detection ─────────────────────────────────────────────────

namespace detail {

inline int detect_terminal_width() noexcept {
    #ifdef __unix__
    struct winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    #endif
    return 80;
}

// Find the last non-empty row in a canvas.
inline int content_height(const Canvas& c) noexcept {
    const int w = c.width();
    const int h = c.height();
    for (int y = h - 1; y >= 0; --y) {
        for (int x = 0; x < w; ++x) {
            auto cell = Cell::unpack(c.cells()[y * w + x]);
            if (cell.character != U' ' && cell.character != 0) return y + 1;
        }
    }
    return 1;
}

// Render element → serialize → write to stdout, erasing previous output.
inline int render_inline(const Element& root, int width, StylePool& pool,
                         std::string& buf, int prev_height) {
    constexpr int kMaxHeight = 60;
    Canvas canvas{width, kMaxHeight, &pool};
    render_tree(root, canvas, pool, theme::dark);

    int h = content_height(canvas);

    // Trim canvas to content height
    Canvas trimmed{width, h, &pool};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < width; ++x) {
            auto cell = Cell::unpack(canvas.cells()[y * width + x]);
            trimmed.set(x, y, cell.character, cell.style_id);
        }

    buf.clear();
    if (prev_height > 0) ansi::erase_lines(prev_height, buf);
    serialize(trimmed, pool, buf);

    std::fwrite(buf.data(), 1, buf.size(), stdout);
    std::fflush(stdout);
    return h;
}

} // namespace detail

// ── maya::print ──────────────────────────────────────────────────────────────

/// Print an element tree to stdout (one-shot, no event loop).
///
///   maya::print(
///       vstack()(
///           text("Hello", bold),
///           text("World", dim)
///       )
///   );
inline void print(const Element& root) {
    int width = detail::detect_terminal_width() - 1;
    StylePool pool;
    std::string buf;
    detail::render_inline(root, width, pool, buf, 0);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

/// Print with explicit width.
inline void print(const Element& root, int width) {
    StylePool pool;
    std::string buf;
    detail::render_inline(root, width, pool, buf, 0);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// ── InlineConfig ─────────────────────────────────────────────────────────────

struct InlineConfig {
    int   fps       = 30;      ///< Target frames per second
    int   max_width = 0;       ///< 0 = auto-detect terminal width
    bool  cursor    = false;   ///< Show cursor during rendering
};

// ── Render function concepts ─────────────────────────────────────────────────

template <typename F>
concept InlineRenderFnDt =
    std::invocable<F, float> &&
    std::convertible_to<std::invoke_result_t<F, float>, Element>;

template <typename F>
concept InlineRenderFnPlain =
    std::invocable<F> &&
    std::convertible_to<std::invoke_result_t<F>, Element>;

template <typename F>
concept AnyInlineRenderFn = InlineRenderFnDt<F> || InlineRenderFnPlain<F>;

// ── maya::inline_run ─────────────────────────────────────────────────────────

/// Run an inline render loop. The render function is called each frame and
/// should return an Element. Call maya::quit() to stop.
///
///   int n = 0;
///   maya::inline_run({.fps = 30}, [&](float dt) {
///       if (n++ > 100) maya::quit();
///       return text("Frame " + std::to_string(n));
///   });
///
/// Also accepts () -> Element (no delta time parameter):
///
///   maya::inline_run({}, [&] {
///       return text("hello");
///   });
template <AnyInlineRenderFn RenderFn>
void inline_run(InlineConfig cfg, RenderFn&& render_fn) {
    detail::quit_requested = false;

    int width = cfg.max_width > 0
        ? cfg.max_width
        : detail::detect_terminal_width() - 1;
    if (width < 20) width = 20;

    auto frame_duration = std::chrono::microseconds(1000000 / std::max(1, cfg.fps));

    StylePool pool;
    std::string buf;
    int prev_h = 0;

    if (!cfg.cursor) std::fputs("\x1b[?25l", stdout); // hide cursor

    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();

    while (!detail::quit_requested) {
        auto now = Clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;

        Element root = [&]() -> Element {
            if constexpr (InlineRenderFnDt<RenderFn>) {
                return render_fn(dt);
            } else {
                return render_fn();
            }
        }();

        if (detail::quit_requested) {
            // Final render before exit
            prev_h = detail::render_inline(root, width, pool, buf, prev_h);
            break;
        }

        prev_h = detail::render_inline(root, width, pool, buf, prev_h);

        auto elapsed = Clock::now() - now;
        if (elapsed < frame_duration) {
            std::this_thread::sleep_for(frame_duration - elapsed);
        }
    }

    if (!cfg.cursor) std::fputs("\x1b[?25h", stdout); // show cursor
    std::fputc('\n', stdout);
    std::fflush(stdout);

    detail::quit_requested = false;
}

} // namespace maya
