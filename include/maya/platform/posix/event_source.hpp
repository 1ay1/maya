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
        struct pollfd pfds[2];
        int nfds = 0;

        pfds[nfds].fd      = terminal_fd_;
        pfds[nfds].events  = POLLIN | (want_write ? POLLOUT : 0);
        pfds[nfds].revents = 0;
        ++nfds;

        if (signal_fd_ >= 0) {
            pfds[nfds].fd      = signal_fd_;
            pfds[nfds].events  = POLLIN;
            pfds[nfds].revents = 0;
            ++nfds;
        }

        int r = ::poll(pfds, static_cast<nfds_t>(nfds),
                        static_cast<int>(timeout.count()));
        if (r < 0) {
            if (errno == EINTR)
                return ok(ReadyFlags{});
            return err<ReadyFlags>(Error::from_errno("poll"));
        }

        return ok(ReadyFlags{
            .input     = (pfds[0].revents & POLLIN) != 0,
            .resize    = nfds > 1 && (pfds[1].revents & POLLIN) != 0,
            .writeable = (pfds[0].revents & POLLOUT) != 0,
        });
    }

    // -- Move-only (no resources to manage) -----------------------------------

    PosixEventSource(PosixEventSource&&) noexcept = default;
    PosixEventSource& operator=(PosixEventSource&&) noexcept = default;
    PosixEventSource(const PosixEventSource&) = delete;
    PosixEventSource& operator=(const PosixEventSource&) = delete;
};

} // namespace maya::platform::posix
