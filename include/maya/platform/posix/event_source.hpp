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

public:
    PosixEventSource(NativeHandle term_fd, NativeHandle sig_fd) noexcept
        : terminal_fd_(term_fd), signal_fd_(sig_fd) {}

    [[nodiscard]] auto wait(
        std::chrono::milliseconds timeout,
        bool want_write = false) -> Result<ReadyFlags>
    {
        struct pollfd pfds[3];
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

        // [2] stdout — write readiness (only when caller has pending data)
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
        flags.input     = (pfds[stdin_idx].revents & POLLIN) != 0;
        flags.resize    = sig_idx >= 0 && (pfds[sig_idx].revents & POLLIN) != 0;
        flags.writeable = want_write
            ? (pfds[write_idx].revents & POLLOUT) != 0
            : true;  // not watching — assume writeable (blocking I/O)
        return ok(flags);
    }

    // -- Move-only (no resources to manage) -----------------------------------

    PosixEventSource(PosixEventSource&&) noexcept = default;
    PosixEventSource& operator=(PosixEventSource&&) noexcept = default;
    PosixEventSource(const PosixEventSource&) = delete;
    PosixEventSource& operator=(const PosixEventSource&) = delete;
};

} // namespace maya::platform::posix
