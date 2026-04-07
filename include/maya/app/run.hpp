#pragma once
// maya::run — The single entry point for maya applications
//
// Design goals:
//   1. Zero ceremony: one function call is all it takes.
//   2. Stability: RunConfig uses C++20 designated initializers with defaults,
//      so adding new options never breaks existing code.
//   3. Type-erased internals: Terminal, Canvas, StylePool, diff — none of
//      these appear in user code. The implementation can be replaced entirely.
//
// Usage:
//
//   maya::run(
//       {.title = "my app", .mouse = true},
//       [&](const Event& ev) {
//           if (key(ev, 'q')) return false;   // false = quit
//           if (key(ev, '+')) count++;
//           return true;
//       },
//       [&](const Ctx& ctx) {
//           return text("Count: " + std::to_string(count.get()));
//       }
//   );

#include <concepts>
#include <functional>
#include <iostream>
#include <type_traits>

#include "../core/expected.hpp"
#include "../element/element.hpp"
#include "../style/theme.hpp"
#include "../terminal/input.hpp"
#include "app.hpp"
#include "events.hpp"

namespace maya {

// ── RunConfig ──────────────────────────────────────────────────────────────
// C++20 aggregate. Add new fields with defaults — all existing call sites
// continue to compile unchanged.

struct RunConfig {
    std::string_view title      = "";         ///< Terminal window title (OSC 0)
    int              fps        = 0;          ///< Continuous rendering at N fps (0 = event-driven only)
    bool             mouse      = false;      ///< Enable mouse event reporting
    bool             alt_screen = true;       ///< Use the alternate screen buffer
    Theme            theme      = theme::dark;///< Colour theme for the app
};

// ── Render context ─────────────────────────────────────────────────────────
// Passed to render functions that declare a (const Ctx&) parameter.
// Grows over time (new fields, defaults) without breaking existing render fns.

struct Ctx {
    Size  size;   ///< Current terminal dimensions (width, height in cells)
    Theme theme;  ///< Active colour theme
};

// ── maya::quit() ───────────────────────────────────────────────────────────
// Call from anywhere — Effect lambdas, render functions, signal handlers —
// to schedule a clean exit after the current frame.

namespace detail {
    inline thread_local bool quit_requested = false;
} // namespace detail

/// Request a clean exit. Safe to call from any context during run().
inline void quit() noexcept { detail::quit_requested = true; }

// ── Render function concepts ───────────────────────────────────────────────

template <typename F>
concept RenderFnNoCtx =
    std::invocable<F> &&
    std::convertible_to<std::invoke_result_t<F>, Element>;

template <typename F>
concept RenderFnWithCtx =
    std::invocable<F, const Ctx&> &&
    std::convertible_to<std::invoke_result_t<F, const Ctx&>, Element>;

template <typename F>
concept AnyRenderFn = RenderFnNoCtx<F> || RenderFnWithCtx<F>;

// ── Event function concepts ────────────────────────────────────────────────

template <typename F>
concept EventFnBool =
    std::invocable<F, const Event&> &&
    std::same_as<std::invoke_result_t<F, const Event&>, bool>;

template <typename F>
concept EventFnVoid =
    std::invocable<F, const Event&> &&
    std::same_as<std::invoke_result_t<F, const Event&>, void>;

template <typename F>
concept AnyEventFn = EventFnBool<F> || EventFnVoid<F>;

// ── run() implementation ───────────────────────────────────────────────────

namespace detail {

template <AnyEventFn EventFn, AnyRenderFn RenderFn>
void run_impl(RunConfig cfg, EventFn&& event_fn, RenderFn&& render_fn)
{
    // Reset quit flag in case run() is called multiple times in one process.
    detail::quit_requested = false;

    // Build App through the existing machinery (Terminal RAII, FrameBuffer, etc.)
    auto result = App::builder()
        .title(cfg.title)
        .mouse(cfg.mouse)
        .alt_screen(cfg.alt_screen)
        .theme(cfg.theme)
        .build();

    if (!result) {
        std::println(std::cerr, "maya: failed to initialize terminal: {}",
                     result.error().message);
        std::exit(1);
    }

    auto app = std::move(*result);
    if (cfg.fps > 0) app.set_fps(cfg.fps);

    // Keep a live Ctx updated on resize.
    Ctx ctx{app.size(), app.theme()};

    // Helper: invoke the event function and handle quit.
    auto dispatch = [&](const Event& ev) {
        if constexpr (EventFnBool<EventFn>) {
            if (!event_fn(ev)) app.quit();
        } else {
            event_fn(ev);
        }
        // Also honour maya::quit() called from inside the handler or from
        // an Effect that ran as a side-effect of signal mutation.
        if (detail::quit_requested) {
            detail::quit_requested = false;
            app.quit();
        }
    };

    // Bridge App's separate on_key / on_mouse / on_resize into a single handler.
    app.on_key([&](const KeyEvent& ke) -> bool {
        dispatch(Event{ke});
        return true; // always consume at this layer
    });

    app.on_mouse([&](const MouseEvent& me) -> bool {
        dispatch(Event{me});
        return true;
    });

    app.on_resize([&](Size sz) {
        ctx.size = sz;
        ResizeEvent re{sz.width, sz.height};
        dispatch(Event{re});
    });

    // Dispatch an initial resize event so widgets can learn the terminal
    // dimensions before the first frame.
    {
        auto sz = app.size();
        ResizeEvent re{sz.width, sz.height};
        dispatch(Event{re});
    }

    // Build a render wrapper that handles both () and (const Ctx&) signatures,
    // and checks quit_requested after each frame.
    std::function<Element()> render_wrapper;
    if constexpr (RenderFnWithCtx<RenderFn>) {
        render_wrapper = [&]() -> Element {
            auto elem = render_fn(ctx);
            if (detail::quit_requested) {
                detail::quit_requested = false;
                app.quit();
            }
            return elem;
        };
    } else {
        render_wrapper = [&]() -> Element {
            auto elem = render_fn();
            if (detail::quit_requested) {
                detail::quit_requested = false;
                app.quit();
            }
            return elem;
        };
    }

    auto status = app.run(std::move(render_wrapper));
    if (!status) {
        std::println(std::cerr, "maya: runtime error: {}", status.error().message);
        std::exit(1);
    }
}

} // namespace detail

// ── Public overloads ───────────────────────────────────────────────────────

/// Run a maya application with explicit configuration.
///
///   maya::run(
///       {.title = "app", .mouse = true},
///       [&](const Event& ev) { return !key(ev, 'q'); },
///       [&](const Ctx& ctx)  { return text("hello"); }
///   );
template <AnyEventFn EventFn, AnyRenderFn RenderFn>
void run(RunConfig cfg, EventFn&& event_fn, RenderFn&& render_fn)
{
    detail::run_impl(cfg,
                     std::forward<EventFn>(event_fn),
                     std::forward<RenderFn>(render_fn));
}

/// Run a maya application with default configuration (no title, no mouse,
/// alt screen, dark theme).
///
///   maya::run(
///       [&](const Event& ev) { return !key(ev, 'q'); },
///       [&]                  { return text("hello"); }
///   );
template <AnyEventFn EventFn, AnyRenderFn RenderFn>
void run(EventFn&& event_fn, RenderFn&& render_fn)
{
    detail::run_impl({},
                     std::forward<EventFn>(event_fn),
                     std::forward<RenderFn>(render_fn));
}

} // namespace maya
