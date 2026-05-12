#pragma once
// maya::platform::posix::PosixEventSource - poll()-based event multiplexer
//
// Waits for input readiness on stdin and the resize signal pipe
// using POSIX poll(). Returns ReadyFlags indicating what's available.

#if !MAYA_PLATFORM_POSIX
#error "This header is for POSIX platforms only"
#endif

#include <cerrno>
#include <chrono>

#include <poll.h>

#include "../../core/expected.hpp"
#include "../concepts.hpp"
#include "../io.hpp"

namespace maya::platform::posix {

// ============================================================================
// PosixEventSource — poll()-based multiplexer
// ============================================================================

class PosixEventSource {
    NativeHandle terminal_fd_;
    NativeHandle signal_fd_;
    NativeHandle wake_fd_ = -1;

public:
    PosixEventSource(NativeHandle term_fd, NativeHandle sig_fd) noexcept
        : terminal_fd_(term_fd), signal_fd_(sig_fd) {}

    // Uniform name across platforms — the parameter is the queue's
    // wake-side native handle (fd on POSIX). `invalid_handle` (-1)
    // disables wake multiplexing; poll just won't see it ready.
    void set_wake_handle(NativeHandle fd) noexcept { wake_fd_ = fd; }

    [[nodiscard]] auto wait(
        std::chrono::milliseconds timeout,
        bool want_write = false) -> Result<ReadyFlags>
    {
        struct pollfd pfds[4];
        int nfds = 0;

        // [0] stdin — read readiness
        pfds[nfds].fd      = terminal_fd_;
        pfds[nfds].events  = POLLIN;
        pfds[nfds].revents = 0;
        const int stdin_idx = nfds++;

        // [1] signal pipe — resize events
        int sig_idx = -1;
        if (signal_fd_ >= 0) {
            pfds[nfds].fd      = signal_fd_;
            pfds[nfds].events  = POLLIN;
            pfds[nfds].revents = 0;
            sig_idx = nfds++;
        }

        // [2] wake pipe — background task messages
        int wake_idx = -1;
        if (wake_fd_ >= 0) {
            pfds[nfds].fd      = wake_fd_;
            pfds[nfds].events  = POLLIN;
            pfds[nfds].revents = 0;
            wake_idx = nfds++;
        }

        // [3] stdout — write readiness (only when caller has pending data)
        int write_idx = -1;
        if (want_write) {
            pfds[nfds].fd      = STDOUT_FILENO;
            pfds[nfds].events  = POLLOUT;
            pfds[nfds].revents = 0;
            write_idx = nfds++;
        }

        int r = ::poll(pfds, static_cast<nfds_t>(nfds),
                        static_cast<int>(timeout.count()));
        if (r < 0) {
            if (errno == EINTR)
                return ok(ReadyFlags{});
            return err<ReadyFlags>(Error::from_errno("poll"));
        }

        ReadyFlags flags{};
        const short stdin_rev = pfds[stdin_idx].revents;
        flags.input     = (stdin_rev & POLLIN) != 0;
        flags.resize    = sig_idx >= 0 && (pfds[sig_idx].revents & POLLIN) != 0;
        flags.wake      = wake_idx >= 0 && (pfds[wake_idx].revents & POLLIN) != 0;
        flags.writeable = want_write
            ? (pfds[write_idx].revents & POLLOUT) != 0
            : true;  // not watching — assume writeable (blocking I/O)
        // POLLHUP on stdin = controlling terminal closed. Surface it so
        // the runtime drains any final input (POLLIN may also be set if
        // bytes remain) and then exits, rather than busy-looping on
        // read()=0 forever.
        flags.hangup    = (stdin_rev & (POLLHUP | POLLERR | POLLNVAL)) != 0;
        return ok(flags);
    }

    // -- Move-only (no resources to manage) -----------------------------------

    PosixEventSource(PosixEventSource&&) noexcept = default;
    PosixEventSource& operator=(PosixEventSource&&) noexcept = default;
    PosixEventSource(const PosixEventSource&) = delete;
    PosixEventSource& operator=(const PosixEventSource&) = delete;
};

} // namespace maya::platform::posix
