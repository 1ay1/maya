#pragma once
// maya::canvas_run - Imperative canvas animation loop
//
// For applications that paint cells directly rather than composing element
// trees. The framework owns every rendering concern: double-buffering, diff,
// non-blocking I/O with POLLOUT retry, frame pacing, resize, signal handling,
// and input parsing. Users provide three narrow callbacks:
//
//   on_resize(pool, w, h)  — rebuild size-dependent state; called once at
//                            startup and again after each SIGWINCH. The pool
//                            is cleared before the call — re-intern styles here.
//
//   on_event(event) → bool — handle input; return false to quit.
//
//   on_paint(canvas, w, h) — draw the current frame. The canvas is the back
//                            buffer, already cleared. Do not call canvas.clear().

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <poll.h>
#include <unistd.h>

#include "../core/expected.hpp"
#include "../render/canvas.hpp"
#include "../render/diff.hpp"
#include "../terminal/ansi.hpp"
#include "../terminal/input.hpp"
#include "../terminal/terminal.hpp"
#include "../terminal/writer.hpp"
#include "app.hpp"

namespace maya {

struct CanvasConfig {
    int         fps        = 60;    // target frame rate
    bool        mouse      = false; // enable all-motion mouse reporting
    bool        alt_screen = true;  // use the alternate screen buffer
    std::string title;              // terminal window title (optional)
};

// canvas_run — run a canvas-based animation loop.
//
// Returns when on_event returns false, or on a fatal I/O error.
[[nodiscard]] inline Status canvas_run(
    CanvasConfig                                   cfg,
    std::function<void(StylePool&, int w, int h)>  on_resize,
    std::function<bool(const Event&)>              on_event,
    std::function<void(Canvas&, int w, int h)>     on_paint)
{
    using Clock = std::chrono::steady_clock;

    // ── Terminal setup ────────────────────────────────────────────────────────
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

    static constexpr std::string_view kMouseOn  = "\x1b[?1003h";
    static constexpr std::string_view kMouseOff = "\x1b[?1003l";
    if (cfg.mouse) (void)::write(fd, kMouseOn.data(), kMouseOn.size());

    Writer writer{fd};

    // ── Canvas & StylePool ────────────────────────────────────────────────────
    Size sz;
    if (alt_term)      sz = alt_term->size();
    else if (raw_term) sz = raw_term->size();
    int W = std::max(1, sz.width.value);
    int H = std::max(1, sz.height.value);

    StylePool pool;
    on_resize(pool, W, H);

    Canvas front(W, H, &pool), back(W, H, &pool);
    front.mark_all_damaged();

    // ── Timing ────────────────────────────────────────────────────────────────
    auto frame_ns   = std::chrono::nanoseconds(1'000'000'000LL / std::max(1, cfg.fps));
    auto next_frame = Clock::now();  // render the first frame immediately

    // ── I/O state ─────────────────────────────────────────────────────────────
    // When a write returns WouldBlock we cache the frame and retry as soon as
    // the fd signals POLLOUT — without re-running paint or re-computing the diff.
    std::optional<std::string> pending_frame;
    std::string out;
    out.reserve(static_cast<std::size_t>(W * H * 12));

    InputParser parser;
    bool running = true;

    // ── Shared cleanup ────────────────────────────────────────────────────────
    auto cleanup = [&](Status result) -> Status {
        if (cfg.mouse) (void)::write(fd, kMouseOff.data(), kMouseOff.size());
        detail::cleanup_signal_pipe();
        return result;
    };

    // ── Resize helper ─────────────────────────────────────────────────────────
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

    // ── Event loop ────────────────────────────────────────────────────────────
    while (running) {
        // Compute poll timeout: wait until the next frame is due.
        // POLLOUT wakes us early if the terminal drains the pending frame.
        auto now = Clock::now();
        int timeout_ms;
        if (now >= next_frame) {
            timeout_ms = 0;  // frame is due right now — don't block
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

        // ── SIGWINCH ──────────────────────────────────────────────────────────
        if (nfds > 1 && (pfds[1].revents & POLLIN)) {
            detail::drain_signal_pipe();
            Size new_sz;
            if (alt_term)      new_sz = alt_term->size();
            else if (raw_term) new_sz = raw_term->size();
            int nw = std::max(1, new_sz.width.value);
            int nh = std::max(1, new_sz.height.value);
            if (nw != W || nh != H) handle_resize(nw, nh);
        }

        // ── Flush pending frame as soon as fd is writable ─────────────────────
        if (pending_frame.has_value() && (pfds[0].revents & POLLOUT)) {
            auto result = writer.write_raw(*pending_frame);
            if (result) {
                std::swap(front, back);
                back.reset_damage();
                pending_frame.reset();
            } else if (result.error().kind != ErrorKind::WouldBlock) {
                return cleanup(result);
            }
            // Still WouldBlock — keep pending_frame; POLLOUT will fire again.
        }

        // ── Input ─────────────────────────────────────────────────────────────
        if (pfds[0].revents & POLLIN) {
            char buf[1024];
            ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n > 0) {
                for (auto& ev : parser.feed({buf, static_cast<std::size_t>(n)})) {
                    if (!on_event(ev)) { running = false; break; }
                }
            }
        }

        // ── Frame render ──────────────────────────────────────────────────────
        // Skip if we're waiting for a pending frame to drain — the cached bytes
        // already represent the most recent paint. We'll catch up on the next tick.
        if (running && !pending_frame.has_value() && Clock::now() >= next_frame) {
            // Advance timer. If we've fallen more than one frame behind (e.g.
            // after a slow write), reset to now rather than accumulating debt.
            next_frame += frame_ns;
            if (Clock::now() > next_frame + frame_ns)
                next_frame = Clock::now() + frame_ns;

            back.clear();
            on_paint(back, W, H);

            // Build the ANSI frame: sync start → diff → SGR reset → sync end.
            out.clear();
            out += ansi::sync_start;
            diff(front, back, pool, out);
            out += ansi::reset;
            out += ansi::sync_end;

            // Skip the write if nothing actually changed (only overhead bytes).
            static const std::size_t kMinSize =
                ansi::sync_start.size() + ansi::reset.size() + ansi::sync_end.size();
            if (out.size() == kMinSize) {
                // Nothing changed: still commit so front tracks back.
                std::swap(front, back);
                back.reset_damage();
            } else {
                auto result = writer.write_raw(out);
                if (!result) {
                    if (result.error().kind == ErrorKind::WouldBlock) {
                        // Cache the frame; POLLOUT will retry it without re-painting.
                        pending_frame = out;
                        // Do NOT swap — front still reflects the terminal's state.
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
