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

inline void sigwinch_handler(int /*sig*/) {
    // Write a single byte to wake up the poll loop. Ignore errors (pipe
    // full is harmless; the loop will see the resize anyway).
    if (signal_pipe_write_fd >= 0) {
        [[maybe_unused]] auto _ = ::write(signal_pipe_write_fd, "R", 1);
    }
}

/// Create the self-pipe and install the SIGWINCH handler. Idempotent.
inline auto setup_signal_pipe() -> Status {
    if (signal_pipe_read_fd >= 0) return ok();

    int fds[2];
    if (::pipe(fds) < 0) {
        return err(Error::from_errno("pipe"));
    }

    // Make both ends non-blocking and close-on-exec.
    for (int fd : fds) {
        int flags = ::fcntl(fd, F_GETFL);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            ::close(fds[0]);
            ::close(fds[1]);
            return err(Error::from_errno("fcntl"));
        }
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }

    signal_pipe_read_fd  = fds[0];
    signal_pipe_write_fd = fds[1];

    struct sigaction sa{};
    sa.sa_handler = sigwinch_handler;
    sa.sa_flags   = SA_RESTART;
    ::sigemptyset(&sa.sa_mask);
    if (::sigaction(SIGWINCH, &sa, nullptr) < 0) {
        return err(Error::from_errno("sigaction(SIGWINCH)"));
    }

    return ok();
}

/// Drain any pending bytes from the signal pipe.
inline void drain_signal_pipe() {
    if (signal_pipe_read_fd < 0) return;
    char buf[64];
    while (::read(signal_pipe_read_fd, buf, sizeof(buf)) > 0) {}
}

/// Tear down the signal pipe.
inline void cleanup_signal_pipe() {
    if (signal_pipe_read_fd >= 0) {
        // Restore default SIGWINCH behavior.
        struct sigaction sa{};
        sa.sa_handler = SIG_DFL;
        ::sigaction(SIGWINCH, &sa, nullptr);

        ::close(signal_pipe_read_fd);
        ::close(signal_pipe_write_fd);
        signal_pipe_read_fd  = -1;
        signal_pipe_write_fd = -1;
    }
}

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
        auto alt_screen(bool v) -> Builder& {
            alt_screen_ = v;
            return *this;
        }

        /// Whether to enable mouse event reporting (default: false).
        auto mouse(bool v) -> Builder& {
            mouse_ = v;
            return *this;
        }

        /// Set the color theme (default: theme::dark).
        auto theme(Theme t) -> Builder& {
            theme_ = t;
            return *this;
        }

        /// Set the terminal title (optional).
        auto title(std::string_view t) -> Builder& {
            title_ = std::string{t};
            return *this;
        }

        /// Construct the App. Returns an error if terminal initialization fails.
        [[nodiscard]] auto build() -> Result<App> {
            // Set up the self-pipe for SIGWINCH delivery.
            MAYA_TRY_VOID(detail::setup_signal_pipe());

            // Acquire the terminal and move it through the type-state chain.
            auto cooked = MAYA_TRY(Terminal<Cooked>::create(STDIN_FILENO));
            auto raw    = MAYA_TRY(std::move(cooked).enable_raw_mode());

            int fd = raw.fd();

            // Optionally enter alt screen (consumes the Raw terminal).
            // If alt_screen is disabled, we stay in raw mode.
            std::optional<Terminal<AltScreen>> alt_term;
            std::optional<Terminal<Raw>>       raw_term;

            if (alt_screen_) {
                alt_term = MAYA_TRY(std::move(raw).enter_alt_screen());
            } else {
                raw_term = std::move(raw);
            }

            // Construct the app.
            App app;
            app.alt_terminal_  = std::move(alt_term);
            app.raw_terminal_  = std::move(raw_term);
            app.fd_            = fd;
            app.writer_        = std::make_unique<Writer>(fd);
            app.theme_         = theme_;
            app.mouse_enabled_ = mouse_;

            // Store theme in the context so render functions can access it.
            app.context_.set<Theme>(theme_);

            // Set terminal title if provided.
            if (!title_.empty()) {
                // OSC 0 ; title BEL
                std::string seq = "\x1b]0;" + title_ + "\x07";
                auto written = ::write(fd, seq.data(), seq.size());
                (void)written;
            }

            // Query initial terminal size.
            if (app.alt_terminal_) {
                app.size_ = app.alt_terminal_->size();
            } else if (app.raw_terminal_) {
                app.size_ = app.raw_terminal_->size();
            }

            return ok(std::move(app));
        }
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
    auto run(std::function<Element()> render_fn) -> Status {
        render_fn_ = std::move(render_fn);
        running_ = true;

        // Initial render.
        needs_render_ = true;

        return event_loop();
    }

    /// Signal the event loop to exit after the current iteration.
    void quit() {
        running_ = false;
    }

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
    // Event loop
    // ========================================================================
    // 1. Poll stdin + signal pipe with 100ms timeout
    // 2. Parse input events
    // 3. Dispatch to handlers
    // 4. Call render function to get new Element tree
    // 5. Layout + render to canvas
    // 6. Diff old vs new frame
    // 7. Optimize and flush to terminal
    // 8. Handle SIGWINCH for resize via self-pipe

    auto event_loop() -> Status {
        constexpr int poll_timeout_ms = 100;

        while (running_) {
            struct pollfd pfds[2];
            int nfds = 0;

            pfds[nfds].fd      = fd_;
            pfds[nfds].events  = POLLIN;
            pfds[nfds].revents = 0;
            ++nfds;

            if (detail::signal_pipe_read_fd >= 0) {
                pfds[nfds].fd      = detail::signal_pipe_read_fd;
                pfds[nfds].events  = POLLIN;
                pfds[nfds].revents = 0;
                ++nfds;
            }

            int poll_result = ::poll(pfds, static_cast<nfds_t>(nfds), poll_timeout_ms);

            if (poll_result < 0) {
                if (errno == EINTR) continue;
                return err(Error::from_errno("poll"));
            }

            if (nfds > 1 && (pfds[1].revents & POLLIN)) {
                detail::drain_signal_pipe();
                handle_resize();
            }

            if (pfds[0].revents & POLLIN) {
                MAYA_TRY_VOID(read_and_dispatch());
            }

            auto timeout_events = parser_.flush_timeout();
            for (auto& ev : timeout_events) {
                dispatch_event(ev);
            }

            if (needs_render_) {
                MAYA_TRY_VOID(render_frame());
                needs_render_ = false;
            }
        }

        return ok();
    }

    // ========================================================================
    // Input reading and dispatch
    // ========================================================================

    auto read_and_dispatch() -> Status {
        char buf[1024];
        ssize_t n = ::read(fd_, buf, sizeof(buf));

        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) return ok();
            return err(Error::from_errno("read"));
        }
        if (n == 0) return ok(); // EOF

        auto events = parser_.feed(std::string_view{buf, static_cast<size_t>(n)});
        for (auto& event : events) {
            dispatch_event(event);
        }

        return ok();
    }

    void dispatch_event(Event& event) {
        std::visit([this](auto& ev) {
            using T = std::decay_t<decltype(ev)>;

            if constexpr (std::same_as<T, KeyEvent>) {
                for (auto& handler : key_handlers_) {
                    if (handler(ev)) break;
                }
                // Always re-render: the handler may have mutated state.
                needs_render_ = true;
            }
            else if constexpr (std::same_as<T, MouseEvent>) {
                for (auto& handler : mouse_handlers_) {
                    if (handler(ev)) break;
                }
                needs_render_ = true;
            }
            else if constexpr (std::same_as<T, ResizeEvent>) {
                size_ = {ev.width, ev.height};
                for (auto& handler : resize_handlers_) {
                    handler(size_);
                }
                needs_render_ = true;
            }
            else if constexpr (std::same_as<T, FocusEvent>) {
                needs_render_ = true;
            }
            else if constexpr (std::same_as<T, PasteEvent>) {
                needs_render_ = true;
            }
        }, event);
    }

    // ========================================================================
    // Resize handling
    // ========================================================================

    void handle_resize() {
        Size new_size;
        if (alt_terminal_) {
            new_size = alt_terminal_->size();
        } else if (raw_terminal_) {
            new_size = raw_terminal_->size();
        } else {
            return;
        }

        if (new_size != size_) {
            size_ = new_size;
            context_.set<Size>(size_);
            for (auto& handler : resize_handlers_) {
                handler(size_);
            }
            needs_render_ = true;
        }
    }

    // ========================================================================
    // Frame rendering
    // ========================================================================
    // Calls the user's render function, runs the full layout engine, renders
    // to the canvas, diffs against the previous frame, and flushes changes.

    auto render_frame() -> Status {
        if (!render_fn_) return ok();

        const int w = size_.width.raw();
        const int h = size_.height.raw();
        if (w <= 0 || h <= 0) return ok();

        // Resize the single canvas to match the terminal.
        if (canvas_.width() != w || canvas_.height() != h) {
            canvas_ = Canvas(w, h, &pool_);
            prev_height_ = 0; // terminal contents are unknown after resize
        }

        // Layout + paint the element tree onto the canvas.
        render_tree(render_fn_(), canvas_, pool_, theme_);

        // Build the frame:
        //   BSU start → erase previous output → new content → BSU end
        out_.clear();
        out_ += ansi::sync_start;
        ansi::erase_lines(prev_height_, out_);
        out_ += ansi::hide_cursor;
        serialize(canvas_, pool_, out_);
        out_ += ansi::sync_end;

        MAYA_TRY_VOID(writer_->write_raw(out_));

        prev_height_ = h; // next render must erase exactly h lines
        return ok();
    }
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

[[nodiscard]] inline Status canvas_run(
    CanvasConfig                                   cfg,
    std::function<void(StylePool&, int w, int h)>  on_resize,
    std::function<bool(const Event&)>              on_event,
    std::function<void(Canvas&, int w, int h)>     on_paint)
{
    using Clock = std::chrono::steady_clock;

    MAYA_TRY_VOID(detail::setup_signal_pipe());

    auto cooked = MAYA_TRY(Terminal<Cooked>::create(STDIN_FILENO));
    auto raw    = MAYA_TRY(std::move(cooked).enable_raw_mode());
    int  fd     = raw.fd();

    std::optional<Terminal<AltScreen>> alt_term;
    std::optional<Terminal<Raw>>       raw_term;
    if (cfg.alt_screen) {
        alt_term = MAYA_TRY(std::move(raw).enter_alt_screen());
    } else {
        raw_term = std::move(raw);
    }

    if (!cfg.title.empty()) {
        std::string seq = "\x1b]0;" + cfg.title + "\x07";
        (void)::write(fd, seq.data(), seq.size());
    }

    // All-motion mouse + SGR extended coordinates.
    static constexpr std::string_view kMouseOn  = "\x1b[?1003h\x1b[?1006h";
    static constexpr std::string_view kMouseOff = "\x1b[?1006l\x1b[?1003l";
    if (cfg.mouse) (void)::write(fd, kMouseOn.data(), kMouseOn.size());

    Writer writer{fd};

    Size sz;
    if (alt_term)      sz = alt_term->size();
    else if (raw_term) sz = raw_term->size();
    int W = std::max(1, sz.width.value);
    int H = std::max(1, sz.height.value);

    StylePool pool;
    on_resize(pool, W, H);

    Canvas front(W, H, &pool), back(W, H, &pool);
    front.mark_all_damaged();

    auto frame_ns   = std::chrono::nanoseconds(1'000'000'000LL / std::max(1, cfg.fps));
    auto next_frame = Clock::now();

    std::optional<std::string> pending_frame;
    std::string out;
    out.reserve(static_cast<std::size_t>(W * H * 12));

    InputParser parser;
    bool running = true;

    auto cleanup = [&](Status result) -> Status {
        if (cfg.mouse) (void)::write(fd, kMouseOff.data(), kMouseOff.size());
        detail::cleanup_signal_pipe();
        return result;
    };

    auto handle_resize = [&](int nw, int nh) {
        W = nw; H = nh;
        pool.clear();
        on_resize(pool, W, H);
        front = Canvas(W, H, &pool);
        back  = Canvas(W, H, &pool);
        front.mark_all_damaged();
        pending_frame.reset();
        out.reserve(static_cast<std::size_t>(W * H * 12));
    };

    while (running) {
        auto now = Clock::now();
        int timeout_ms;
        if (now >= next_frame) {
            timeout_ms = 0;
        } else {
            auto wait  = std::chrono::duration_cast<std::chrono::milliseconds>(next_frame - now);
            timeout_ms = static_cast<int>(wait.count()) + 1;
        }

        struct pollfd pfds[2];
        int nfds = 0;
        pfds[nfds].fd      = fd;
        pfds[nfds].events  = POLLIN | (pending_frame.has_value() ? POLLOUT : 0);
        pfds[nfds].revents = 0;
        ++nfds;
        if (detail::signal_pipe_read_fd >= 0) {
            pfds[nfds].fd      = detail::signal_pipe_read_fd;
            pfds[nfds].events  = POLLIN;
            pfds[nfds].revents = 0;
            ++nfds;
        }

        int pr = ::poll(pfds, static_cast<nfds_t>(nfds), timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return cleanup(err(Error::from_errno("poll")));
        }

        if (nfds > 1 && (pfds[1].revents & POLLIN)) {
            detail::drain_signal_pipe();
            Size new_sz;
            if (alt_term)      new_sz = alt_term->size();
            else if (raw_term) new_sz = raw_term->size();
            int nw = std::max(1, new_sz.width.value);
            int nh = std::max(1, new_sz.height.value);
            if (nw != W || nh != H) handle_resize(nw, nh);
        }

        if (pending_frame.has_value() && (pfds[0].revents & POLLOUT)) {
            auto result = writer.write_raw(*pending_frame);
            if (result) {
                std::swap(front, back);
                back.reset_damage();
                pending_frame.reset();
            } else if (result.error().kind != ErrorKind::WouldBlock) {
                return cleanup(result);
            }
        }

        if (pfds[0].revents & POLLIN) {
            char buf[1024];
            ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n > 0) {
                for (auto& ev : parser.feed({buf, static_cast<std::size_t>(n)})) {
                    if (!on_event(ev)) { running = false; break; }
                }
            }
        }

        if (running && !pending_frame.has_value() && Clock::now() >= next_frame) {
            next_frame += frame_ns;
            if (Clock::now() > next_frame + frame_ns)
                next_frame = Clock::now() + frame_ns;

            back.clear();
            on_paint(back, W, H);

            out.clear();
            out += ansi::sync_start;
            diff(front, back, pool, out);
            out += ansi::reset;
            out += ansi::sync_end;

            static const std::size_t kMinSize =
                ansi::sync_start.size() + ansi::reset.size() + ansi::sync_end.size();
            if (out.size() == kMinSize) {
                std::swap(front, back);
                back.reset_damage();
            } else {
                auto result = writer.write_raw(out);
                if (!result) {
                    if (result.error().kind == ErrorKind::WouldBlock) {
                        pending_frame = out;
                    } else {
                        return cleanup(result);
                    }
                } else {
                    std::swap(front, back);
                    back.reset_damage();
                }
            }
        }
    }

    return cleanup(ok());
}

} // namespace maya
