#pragma once
// maya::app - Application entry point and event loop
//
// Provides two entry points:
//
//   App  — element-tree / component model. Builder pattern, event handlers,
//           render callback that returns an Element. Uses serialize + erase-
//           and-redraw (Ink model). Good for UI applications.
//
//   canvas_run() — imperative canvas animation loop. Double-buffered diff,
//                  fixed-fps timer, POLLOUT retry. Good for games / animations.
//
// Both share the same terminal infrastructure (signal pipe, raw mode, alt
// screen, SIGWINCH). SIGWINCH is caught via a self-pipe so resize events
// integrate cleanly with the poll loop.

#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <poll.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#include "../core/concepts.hpp"
#include "../core/expected.hpp"
#include "../core/types.hpp"
#include "../element/builder.hpp"
#include "../element/element.hpp"
#include "../render/canvas.hpp"
#include "../render/diff.hpp"
#include "../render/renderer.hpp"
#include "../render/serialize.hpp"
#include "../style/theme.hpp"
#include "../terminal/input.hpp"
#include "../terminal/terminal.hpp"
#include "../terminal/writer.hpp"
#include "context.hpp"

namespace maya {

// ============================================================================
// Self-pipe for signal delivery into the poll loop
// ============================================================================
// SIGWINCH writes a byte to the pipe; the event loop polls the read end
// alongside stdin. This avoids the classic async-signal-safety pitfalls.

namespace detail {

inline int signal_pipe_read_fd  = -1;
inline int signal_pipe_write_fd = -1;

void sigwinch_handler(int /*sig*/);

/// Create the self-pipe and install the SIGWINCH handler. Idempotent.
auto setup_signal_pipe() -> Status;

/// Drain any pending bytes from the signal pipe.
void drain_signal_pipe();

/// Tear down the signal pipe.
void cleanup_signal_pipe();

} // namespace detail

// ============================================================================
// App - The application entry point
// ============================================================================

class App {
public:
    // ========================================================================
    // Builder
    // ========================================================================

    class Builder {
        bool        alt_screen_ = true;
        bool        mouse_      = false;
        Theme       theme_      = theme::dark;
        std::string title_;

    public:
        Builder() = default;

        /// Whether to enter the alternate screen buffer (default: true).
        auto alt_screen(bool v) -> Builder&;

        /// Whether to enable mouse event reporting (default: false).
        auto mouse(bool v) -> Builder&;

        /// Set the color theme (default: theme::dark).
        auto theme(Theme t) -> Builder&;

        /// Set the terminal title (optional).
        auto title(std::string_view t) -> Builder&;

        /// Construct the App. Returns an error if terminal initialization fails.
        [[nodiscard]] auto build() -> Result<App>;
    };

    /// Create a Builder for fluent configuration.
    [[nodiscard]] static auto builder() -> Builder {
        return Builder{};
    }

    // ========================================================================
    // Running the app
    // ========================================================================

    /// Run the event loop with a Component (anything satisfying the Component
    /// concept, i.e., has a render() -> Element method).
    template <Component C>
    auto run(C&& component) -> Status {
        return run([&component]() -> Element {
            return component.render();
        });
    }

    /// Run the event loop with a render callback that produces an Element tree.
    auto run(std::function<Element()> render_fn) -> Status;

    /// Signal the event loop to exit after the current iteration.
    void quit() {
        running_ = false;
    }

    /// Set continuous rendering at the given frame rate (0 = event-driven).
    void set_fps(int fps) { fps_ = fps; }

    // ========================================================================
    // Event handlers
    // ========================================================================

    /// Register a key event handler. Returns true to consume the event.
    auto on_key(std::function<bool(const KeyEvent&)> handler) -> App& {
        key_handlers_.push_back(std::move(handler));
        return *this;
    }

    /// Register a mouse event handler. Returns true to consume the event.
    auto on_mouse(std::function<bool(const MouseEvent&)> handler) -> App& {
        mouse_handlers_.push_back(std::move(handler));
        return *this;
    }

    /// Register a resize handler.
    auto on_resize(std::function<void(Size)> handler) -> App& {
        resize_handlers_.push_back(std::move(handler));
        return *this;
    }

    // ========================================================================
    // Accessors
    // ========================================================================

    /// Whether the app is running in inline mode (no alt screen).
    [[nodiscard]] bool is_inline() const noexcept { return raw_terminal_.has_value(); }

    /// Current terminal size.
    [[nodiscard]] Size size() const noexcept { return size_; }

    /// Current theme.
    [[nodiscard]] const Theme& theme() const noexcept { return theme_; }

    /// Context map (read-only access for render functions).
    [[nodiscard]] const ContextMap& context() const noexcept { return context_; }

    /// Context map (mutable access for setting context values between frames).
    [[nodiscard]] ContextMap& context() noexcept { return context_; }


    // ========================================================================
    // Move-only
    // ========================================================================

    App(App&&) noexcept = default;
    App& operator=(App&&) noexcept = default;
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    ~App() {
        detail::cleanup_signal_pipe();
    }

private:
    App() = default;

    // -- Terminal ownership ---------------------------------------------------
    std::optional<Terminal<AltScreen>> alt_terminal_;
    std::optional<Terminal<Raw>>       raw_terminal_;
    int fd_ = -1;

    // -- Rendering pipeline ---------------------------------------------------
    // Single canvas: render element tree into it, serialize to ANSI, erase
    // the previous output, write the new frame. No front/back buffers — the
    // terminal IS the front buffer (exactly what Ink / Claude Code does).
    std::unique_ptr<Writer> writer_;
    StylePool               pool_;
    Canvas                  canvas_;
    int                     prev_height_ = 0;  // lines written in last frame
    std::string             out_;               // reused output buffer

    // -- Configuration --------------------------------------------------------
    Theme      theme_         = theme::dark;
    bool       mouse_enabled_ = false;
    int        fps_           = 0;   // 0 = event-driven, >0 = continuous
    ContextMap context_;
    Size       size_{};

    // -- Event handling -------------------------------------------------------
    InputParser parser_;
    std::function<Element()> render_fn_;
    bool                     running_      = false;
    bool                     needs_render_ = true;

    std::vector<std::function<bool(const KeyEvent&)>>   key_handlers_;
    std::vector<std::function<bool(const MouseEvent&)>> mouse_handlers_;
    std::vector<std::function<void(Size)>>              resize_handlers_;

    // ========================================================================
    // Private methods (implemented in app.cpp)
    // ========================================================================

    auto event_loop() -> Status;
    auto read_and_dispatch() -> Status;
    void dispatch_event(Event& event);
    void handle_resize();
    auto render_frame() -> Status;
};

// ============================================================================
// canvas_run - Imperative canvas animation loop
// ============================================================================
// For applications that paint cells directly rather than composing element
// trees. The framework owns every rendering concern: double-buffering, diff,
// non-blocking I/O with POLLOUT retry, frame pacing, resize, and signal
// handling. Users provide three narrow callbacks:
//
//   on_resize(pool, w, h)  — rebuild size-dependent state; called once at
//                            startup and again after each SIGWINCH. The pool
//                            is cleared before the call — re-intern styles here.
//
//   on_event(event) → bool — handle input; return false to quit.
//
//   on_paint(canvas, w, h) — draw the current frame into the back buffer,
//                            which is already cleared. Do not call canvas.clear().

struct CanvasConfig {
    int         fps        = 60;    // target frame rate
    bool        mouse      = false; // enable all-motion mouse reporting
    bool        alt_screen = true;  // use the alternate screen buffer
    std::string title;              // terminal window title (optional)
};

[[nodiscard]] Status canvas_run(
    CanvasConfig                                   cfg,
    std::function<void(StylePool&, int w, int h)>  on_resize,
    std::function<bool(const Event&)>              on_event,
    std::function<void(Canvas&, int w, int h)>     on_paint);

} // namespace maya
