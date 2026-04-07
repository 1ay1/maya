#include "maya/app/app.hpp"

#include <algorithm>
#include <format>
#include <ranges>

#include "maya/core/focus.hpp"
#include "maya/core/overload.hpp"
#include "maya/core/scope_exit.hpp"

namespace maya {

// ============================================================================
// Self-pipe for signal delivery into the poll loop
// ============================================================================

namespace detail {

namespace {

struct SignalPipe {
    int read_fd  = -1;
    int write_fd = -1;
};

SignalPipe signal_pipe{};

} // anonymous namespace

void sigwinch_handler(int /*sig*/) {
    // Write a single byte to wake up the poll loop. Ignore errors (pipe
    // full is harmless; the loop will see the resize anyway).
    if (signal_pipe.write_fd >= 0) {
        [[maybe_unused]] auto _ = ::write(signal_pipe.write_fd, "R", 1);
    }
}

auto setup_signal_pipe() -> Status {
    if (signal_pipe.read_fd >= 0) return ok();

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

    signal_pipe.read_fd  = fds[0];
    signal_pipe.write_fd = fds[1];

    // Update the inline globals that the header exposes so the rest of the
    // codebase (poll setup in event_loop, canvas_run, etc.) can see them.
    signal_pipe_read_fd  = signal_pipe.read_fd;
    signal_pipe_write_fd = signal_pipe.write_fd;

    struct sigaction sa{};
    sa.sa_handler = sigwinch_handler;
    sa.sa_flags   = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (::sigaction(SIGWINCH, &sa, nullptr) < 0) {
        return err(Error::from_errno("sigaction(SIGWINCH)"));
    }

    return ok();
}

void drain_signal_pipe() {
    if (signal_pipe.read_fd < 0) return;
    char buf[64];
    while (::read(signal_pipe.read_fd, buf, sizeof(buf)) > 0) {}
}

void cleanup_signal_pipe() {
    if (signal_pipe.read_fd >= 0) {
        // Restore default SIGWINCH behavior.
        struct sigaction sa{};
        sa.sa_handler = SIG_DFL;
        ::sigaction(SIGWINCH, &sa, nullptr);

        ::close(signal_pipe.read_fd);
        ::close(signal_pipe.write_fd);
        signal_pipe = {-1, -1};

        signal_pipe_read_fd  = -1;
        signal_pipe_write_fd = -1;
    }
}

} // namespace detail

// ============================================================================
// App::Builder
// ============================================================================

auto App::Builder::alt_screen(bool v) -> Builder& {
    alt_screen_ = v;
    return *this;
}

auto App::Builder::mouse(bool v) -> Builder& {
    mouse_ = v;
    return *this;
}

auto App::Builder::theme(Theme t) -> Builder& {
    theme_ = t;
    return *this;
}

auto App::Builder::title(std::string_view t) -> Builder& {
    title_ = std::string{t};
    return *this;
}

auto App::Builder::build() -> Result<App> {
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
    app.theme_          = theme_;
    app.mouse_enabled_  = mouse_;
    app.started_inline_ = !alt_screen_;

    // Store theme in the context so render functions can access it.
    app.context_.set<Theme>(theme_);

    // Set terminal title if provided.
    if (!title_.empty()) {
        // OSC 0 ; title BEL
        auto seq = std::format("\x1b]0;{}\x07", title_);
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

// ============================================================================
// App::run (std::function overload)
// ============================================================================

auto App::run(std::function<Element()> render_fn) -> Status {
    render_fn_ = std::move(render_fn);
    running_ = true;

    // Initial render.
    needs_render_ = true;

    return event_loop();
}

// ============================================================================
// Event loop
// ============================================================================

auto App::event_loop() -> Status {
    using Clock = std::chrono::steady_clock;
    auto next_frame = Clock::now();

    while (running_) {
        // Compute poll timeout: fps-driven or 100ms idle.
        int poll_timeout_ms = 100;
        if (fps_ > 0) {
            auto now = Clock::now();
            auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(next_frame - now);
            poll_timeout_ms = std::max(0, static_cast<int>(wait.count()));
        }

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

        for (auto& ev : parser_.flush_timeout()) {
            dispatch_event(ev);
        }

        // fps-driven: always re-render when the frame deadline has passed.
        if (fps_ > 0 && Clock::now() >= next_frame) {
            needs_render_ = true;
            next_frame += std::chrono::microseconds(1'000'000 / fps_);
            // Don't let the frame deadline drift too far behind.
            if (Clock::now() > next_frame)
                next_frame = Clock::now();
        }

        if (needs_render_) {
            MAYA_TRY_VOID(render_frame());
            needs_render_ = false;
        }
    }

    // In inline mode, show cursor and emit a newline so the shell
    // prompt starts on a clean line below our output.
    if (is_inline()) {
        auto cleanup = std::format("{}{}\r\n", ansi::show_cursor, ansi::reset);
        (void)writer_->write_raw(cleanup);
    }

    return ok();
}

// ============================================================================
// Input reading and dispatch
// ============================================================================

auto App::read_and_dispatch() -> Status {
    char buf[1024];
    ssize_t n = ::read(fd_, buf, sizeof(buf));

    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) return ok();
        return err(Error::from_errno("read"));
    }
    if (n == 0) return ok(); // EOF

    for (auto& event : parser_.feed(std::string_view{buf, static_cast<size_t>(n)})) {
        dispatch_event(event);
    }

    return ok();
}

void App::dispatch_event(Event& event) {
    std::visit(overload{
        [this](KeyEvent& ev) {
            // Focus navigation: Tab / Shift+Tab cycle focus before user handlers
            if (current_focus_scope) {
                if (auto* sp = std::get_if<SpecialKey>(&ev.key)) {
                    if (*sp == SpecialKey::Tab) {
                        focus_next();
                        needs_render_ = true;
                        return;
                    }
                    if (*sp == SpecialKey::BackTab) {
                        focus_prev();
                        needs_render_ = true;
                        return;
                    }
                }
            }
            std::ranges::any_of(key_handlers_,
                [&](auto& h) { return h(ev); });
            needs_render_ = true;
        },
        [this](MouseEvent& ev) {
            std::ranges::any_of(mouse_handlers_,
                [&](auto& h) { return h(ev); });
            needs_render_ = true;
        },
        [this](ResizeEvent& ev) {
            size_ = {ev.width, ev.height};
            std::ranges::for_each(resize_handlers_,
                [&](auto& h) { h(size_); });
            needs_render_ = true;
        },
        [this](FocusEvent&) {
            needs_render_ = true;
        },
        [this](PasteEvent&) {
            needs_render_ = true;
        },
    }, event);
}

// ============================================================================
// Resize handling
// ============================================================================

void App::handle_resize() {
    Size new_size;
    if (alt_terminal_) {
        new_size = alt_terminal_->size();
    } else if (raw_terminal_) {
        new_size = raw_terminal_->size();
    } else {
        return;
    }

    if (new_size != size_) {
        // In inline mode, a width change means the terminal has reflowed
        // our output. Promote to alt screen for clean diff-based rendering.
        if (is_inline() && new_size.width != size_.width) {
            promote_to_alt_screen();
        }

        size_ = new_size;
        context_.set<Size>(size_);
        needs_clear_ = true;  // force full repaint after resize
        for (auto& handler : resize_handlers_) {
            handler(size_);
        }
        needs_render_ = true;
    }
}

void App::promote_to_alt_screen() {
    if (!raw_terminal_) return;

    // Erase inline output before switching buffers.
    {
        std::string cleanup;
        ansi::erase_lines(prev_height_, cleanup);
        cleanup += ansi::show_cursor;
        (void)writer_->write_raw(cleanup);
    }

    // Transition Raw → AltScreen.
    auto result = std::move(*raw_terminal_).enter_alt_screen();
    raw_terminal_.reset();

    if (result) {
        alt_terminal_ = std::move(*result);
    }

    // Force fresh double-buffered canvases at new size.
    canvas_ = Canvas(1, 1, &pool_);
    front_  = Canvas(1, 1, &pool_);
    pool_.clear();
    prev_height_ = 0;
    prev_width_  = 0;
    committed_height_ = 0;
    prev_content_height_ = 0;
    prev_row_hashes_.clear();
    needs_clear_ = true;
}

// ============================================================================
// Frame rendering
// ============================================================================

auto App::render_frame() -> Status {
    if (!render_fn_) return ok();

    const bool promoted = started_inline_ && !is_inline();
    const int w = is_inline()
        ? std::max(1, size_.width.raw() - 1)   // inline: avoid terminal auto-wrap
        : size_.width.raw();                     // alt screen: full width
    if (w <= 0) return ok();

    if (promoted) {
        // ── Promoted mode (was inline, now alt screen) ──────────────
        // Content-height layout (UI stays compact, not stretched to full
        // terminal height). Double-buffered diff in alt screen.
        constexpr int kMaxInlineHeight = 60;
        const int canvas_h = std::max(1,
            std::min(size_.height.raw(), kMaxInlineHeight));

        if (canvas_.width() != w || canvas_.height() != canvas_h) {
            canvas_ = Canvas(w, canvas_h, &pool_);
            front_  = Canvas(w, canvas_h, &pool_);
            front_.mark_all_damaged();
            needs_clear_ = true;
        }

        canvas_.clear();
        render_tree(render_fn_(), canvas_, pool_, theme_);

        out_.clear();
        out_ += ansi::hide_cursor;
        out_ += ansi::sync_start;
        if (needs_clear_) {
            out_ += "\x1b[2J\x1b[H";
            serialize(canvas_, pool_, out_);
            needs_clear_ = false;
        } else {
            diff(front_, canvas_, pool_, out_);
        }
        out_ += ansi::reset;
        out_ += ansi::sync_end;

        MAYA_TRY_VOID(writer_->write_raw(out_));
        std::swap(front_, canvas_);
        canvas_.reset_damage();
    } else if (is_inline()) {
        // ── Inline mode (Ink-style) ─────────────────────────────────
        // Content-sized layout: the root's height is NOT constrained to
        // the terminal — it sizes to content, exactly as Ink/Yoga does.
        // The canvas is tall enough to hold any reasonable content.
        // BSU/ESU wrapping prevents flicker during erase + redraw.
        constexpr int kMaxCanvasHeight = 500;
        const int canvas_h = kMaxCanvasHeight;

        if (canvas_.width() != w || canvas_.height() != canvas_h)
            canvas_ = Canvas(w, canvas_h, &pool_);

        canvas_.clear();
        render_tree(render_fn_(), canvas_, pool_, theme_, /*auto_height=*/true);

        int ch = content_height(canvas_);
        const int term_h = std::max(1, size_.height.raw());

        // Cap to terminal height minus 1 to avoid auto-scroll on last line.
        int display_rows = std::min(ch, term_h - 1);
        int skip_rows = ch - display_rows;

        // ── Row-hash comparison ────────────────────────────────
        // Compute a hash per visible row to find the stable prefix
        // (rows identical to last frame). Stable rows above the first
        // change are "committed" to scrollback and never overwritten,
        // so the user can scroll up to see old tool-card output, diffs,
        // etc. exactly as they appeared.
        const int W = canvas_.width();
        const uint64_t* cells = canvas_.cells();

        // Hash every row and find the stable prefix (contiguous matching
        // rows from the top).  We must check from row 0 — a change at
        // ANY row caps the stable prefix, preventing premature commitment
        // of rows below an active animation (spinner, streaming text).
        row_hashes_.resize(static_cast<size_t>(ch));
        int stable = 0;
        {
            int check = std::min(ch, prev_content_height_);
            int prev_sz = static_cast<int>(prev_row_hashes_.size());
            for (int y = 0; y < ch; ++y) {
                uint64_t h = 14695981039346656037ULL;
                const uint64_t* row = cells + y * W;
                for (int x = 0; x < W; ++x) {
                    h ^= row[x];
                    h *= 1099511628211ULL;
                }
                row_hashes_[static_cast<size_t>(y)] = h;

                // Extend stable prefix while rows match.
                if (y == stable && y < check && y < prev_sz
                    && h == prev_row_hashes_[static_cast<size_t>(y)]) {
                    stable = y + 1;
                }
            }
        }

        // Update committed_height_ to match the current stable prefix.
        // It can grow (new rows become stable) or shrink (content changed
        // in a previously-committed region, e.g. a new section was
        // inserted in the middle of the UI, pushing rows down).
        committed_height_ = stable;

        // The "live" region is everything from committed_height_ down.
        // We only move the cursor up into the live region, never into
        // committed rows.  prev_live is recomputed using the NEW
        // committed_height_ so that newly-committed rows are excluded
        // from cursor movement (they're now in scrollback).
        int live_rows  = std::max(0, display_rows - std::max(0, committed_height_ - skip_rows));
        int prev_live  = std::max(0, prev_height_  - std::max(0, committed_height_ - (prev_content_height_ - prev_height_)));

        // Where in the canvas does the live region start?
        int live_start = std::max(skip_rows, committed_height_);

        out_.clear();
        out_ += ansi::sync_start;
        out_ += ansi::hide_cursor;

        // Move cursor to the top of the live region.
        if (prev_live > 1) {
            ansi::write_cursor_up(out_, prev_live - 1);
        }
        if (prev_live > 0) {
            out_ += "\r";
        }

        // Incremental serialize: only re-render rows that changed.
        if (live_rows > 0) {
            const uint64_t* old_p = prev_row_hashes_.data();
            int old_n = static_cast<int>(prev_row_hashes_.size());
            serialize_changed(canvas_, pool_, out_, ch, live_start,
                              old_p, old_n,
                              row_hashes_.data(), ch);
        }

        // Erase leftover lines if the live area shrunk.
        if (live_rows < prev_live) {
            int extra = prev_live - live_rows;
            for (int i = 0; i < extra; ++i)
                out_ += "\r\n\x1b[2K";
            ansi::write_cursor_up(out_, extra);
        }

        out_ += ansi::reset;
        out_ += ansi::sync_end;

        MAYA_TRY_VOID(writer_->write_raw(out_));
        prev_height_ = display_rows;
        prev_content_height_ = ch;
        std::swap(prev_row_hashes_, row_hashes_);
    } else {
        // ── Alt-screen mode (started in alt screen) ─────────────────
        // Double-buffered diff: only emit ANSI for cells that changed.
        const int h = size_.height.raw();
        if (h <= 0) return ok();

        if (canvas_.width() != w || canvas_.height() != h) {
            canvas_ = Canvas(w, h, &pool_);
            front_  = Canvas(w, h, &pool_);
            front_.mark_all_damaged();
            needs_clear_ = true;
        }

        canvas_.clear();
        render_tree(render_fn_(), canvas_, pool_, theme_);

        out_.clear();
        out_ += ansi::hide_cursor;
        out_ += ansi::sync_start;
        if (needs_clear_) {
            out_ += "\x1b[2J\x1b[H";
            serialize(canvas_, pool_, out_);
            needs_clear_ = false;
        } else {
            diff(front_, canvas_, pool_, out_);
        }
        out_ += ansi::reset;
        out_ += ansi::sync_end;

        MAYA_TRY_VOID(writer_->write_raw(out_));
        std::swap(front_, canvas_);
        canvas_.reset_damage();
    }
    return ok();
}

// ============================================================================
// canvas_run - Imperative canvas animation loop
// ============================================================================

namespace detail {

Status canvas_run_impl(
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
        auto seq = std::format("\x1b]0;{}\x07", cfg.title);
        (void)::write(fd, seq.data(), seq.size());
    }

    // All-motion mouse + SGR extended coordinates.
    static constexpr std::string_view kMouseOn  = "\x1b[?1003h\x1b[?1006h";
    static constexpr std::string_view kMouseOff = "\x1b[?1006l\x1b[?1003l";
    if (cfg.mouse) (void)::write(fd, kMouseOn.data(), kMouseOn.size());

    // Switch fd to non-blocking so write() returns EAGAIN when the pty buffer
    // is full rather than stalling the event loop. write_some() + pending_frame
    // resume the BSU frame on the next POLLOUT — this is what makes the loop
    // stay responsive on large terminals and over SSH.
    const int orig_fl = ::fcntl(fd, F_GETFL);
    if (orig_fl >= 0) ::fcntl(fd, F_SETFL, orig_fl | O_NONBLOCK);

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

    struct PendingFrame { std::string data; std::size_t offset = 0; };
    std::optional<PendingFrame> pending_frame;
    std::string out;
    out.reserve(static_cast<std::size_t>(W * H * 12));

    InputParser parser;
    bool running     = true;
    bool needs_clear = true; // first frame must clear (alt screen may have stale content)

    // RAII guards — cleanup runs automatically on any exit path.
    scope_exit mouse_guard([&] {
        if (cfg.mouse) (void)::write(fd, kMouseOff.data(), kMouseOff.size());
    });
    scope_exit signal_guard([] { cleanup_signal_pipe(); });
    scope_exit fcntl_guard([&] {
        if (orig_fl >= 0) ::fcntl(fd, F_SETFL, orig_fl);
    });

    auto handle_resize = [&](int nw, int nh) {
        W = nw; H = nh;
        pool.clear();
        on_resize(pool, W, H);
        front = Canvas(W, H, &pool);
        back  = Canvas(W, H, &pool);
        front.mark_all_damaged();
        pending_frame.reset();
        needs_clear = true; // terminal screen has stale content from old size
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
            return err(Error::from_errno("poll"));
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
            auto& pf   = *pending_frame;
            auto  sv   = std::string_view(pf.data).substr(pf.offset);
            auto  result = writer.write_some(sv);
            if (!result) return err(result.error());
            pf.offset += *result;
            if (pf.offset >= pf.data.size()) {
                std::swap(front, back);
                back.reset_damage();
                pending_frame.reset();
            }
            // else: still pending, POLLOUT will fire again
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
            if (needs_clear) {
                // After resize (or first frame), the terminal has stale content
                // that doesn't match our front canvas. Bypass diff entirely:
                // clear the screen and serialize every cell. This is the only
                // way to guarantee no ghost cells — diff skips blank cells that
                // match front, but the terminal might not actually be blank.
                out += "\x1b[2J\x1b[H";
                serialize(back, pool, out);
                needs_clear = false;
            } else {
                diff(front, back, pool, out);
            }
            out += ansi::reset;
            out += ansi::sync_end;

            static const std::size_t kMinSize =
                ansi::sync_start.size() + ansi::reset.size() + ansi::sync_end.size();
            if (out.size() == kMinSize) {
                std::swap(front, back);
                back.reset_damage();
            } else {
                auto result = writer.write_some(out);
                if (!result) return err(result.error());
                const std::size_t written = *result;
                if (written < out.size()) {
                    // Partial write — BSU frame is open; resume on next POLLOUT.
                    pending_frame = PendingFrame{out, written};
                } else {
                    std::swap(front, back);
                    back.reset_damage();
                }
            }
        }
    }

    return ok();
}

} // namespace detail
} // namespace maya
