#pragma once
// maya::platform::posix::PosixResizeSignal - SIGWINCH via self-pipe
//
// Installs a SIGWINCH handler that writes a byte to a pipe.
// The read end is polled by PosixEventSource. This is the classic
// self-pipe trick for integrating async signals into an event loop
// without async-signal-safety violations.

#if !MAYA_PLATFORM_POSIX
#error "This header is for POSIX platforms only"
#endif

#include <cerrno>
#include <csignal>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#include "../../core/expected.hpp"
#include "../io.hpp"

namespace maya::platform::posix {

// ============================================================================
// Global signal state — process-wide singleton
// ============================================================================
// Only one SIGWINCH handler exists per process. These inline globals
// are shared across all TUs via C++17 inline variable rules.

namespace detail {

inline int g_signal_write_fd = -1;

inline void sigwinch_handler(int /*sig*/) {
    // Async-signal-safe: write(2) is safe. Ignore errors —
    // pipe full is harmless; the loop will see the resize anyway.
    if (g_signal_write_fd >= 0) {
        [[maybe_unused]] auto _ = ::write(g_signal_write_fd, "R", 1);
    }
}

} // namespace detail

// ============================================================================
// PosixResizeSignal — RAII SIGWINCH handler with self-pipe
// ============================================================================

class PosixResizeSignal {
    int read_fd_  = -1;
    int write_fd_ = -1;

    PosixResizeSignal() = default;

public:
    [[nodiscard]] static auto install() -> Result<PosixResizeSignal> {
        // Already installed — reuse existing pipe.
        if (detail::g_signal_write_fd >= 0) {
            // Return a non-owning instance that won't cleanup.
            // Only the first installer owns the pipe.
            PosixResizeSignal s;
            // Find the read fd from the write fd... we can't easily.
            // So just return invalid — caller should check.
            return ok(std::move(s));
        }

        int fds[2];
        if (::pipe(fds) < 0)
            return err<PosixResizeSignal>(Error::from_errno("pipe"));

        // Make both ends non-blocking and close-on-exec.
        for (int fd : fds) {
            int flags = ::fcntl(fd, F_GETFL);
            if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
                ::close(fds[0]);
                ::close(fds[1]);
                return err<PosixResizeSignal>(Error::from_errno("fcntl"));
            }
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        }

        detail::g_signal_write_fd = fds[1];

        struct sigaction sa{};
        sa.sa_handler = detail::sigwinch_handler;
        sa.sa_flags   = SA_RESTART;
        ::sigemptyset(&sa.sa_mask);
        if (::sigaction(SIGWINCH, &sa, nullptr) < 0) {
            ::close(fds[0]);
            ::close(fds[1]);
            detail::g_signal_write_fd = -1;
            return err<PosixResizeSignal>(Error::from_errno("sigaction(SIGWINCH)"));
        }

        PosixResizeSignal s;
        s.read_fd_  = fds[0];
        s.write_fd_ = fds[1];
        return ok(std::move(s));
    }

    [[nodiscard]] bool pending() const { return read_fd_ >= 0; }

    void drain() {
        if (read_fd_ < 0) return;
        char buf[64];
        while (::read(read_fd_, buf, sizeof(buf)) > 0) {}
    }

    [[nodiscard]] NativeHandle native_handle() const noexcept {
        return read_fd_;
    }

    // -- Move-only ------------------------------------------------------------

    PosixResizeSignal(PosixResizeSignal&& o) noexcept
        : read_fd_(std::exchange(o.read_fd_, -1))
        , write_fd_(std::exchange(o.write_fd_, -1))
    {}

    PosixResizeSignal& operator=(PosixResizeSignal&& o) noexcept {
        if (this != &o) {
            cleanup();
            read_fd_  = std::exchange(o.read_fd_, -1);
            write_fd_ = std::exchange(o.write_fd_, -1);
        }
        return *this;
    }

    PosixResizeSignal(const PosixResizeSignal&) = delete;
    PosixResizeSignal& operator=(const PosixResizeSignal&) = delete;

    ~PosixResizeSignal() { cleanup(); }

private:
    void cleanup() noexcept {
        if (read_fd_ >= 0) {
            struct sigaction sa{};
            sa.sa_handler = SIG_DFL;
            ::sigaction(SIGWINCH, &sa, nullptr);

            ::close(read_fd_);
            ::close(write_fd_);
            detail::g_signal_write_fd = -1;
            read_fd_  = -1;
            write_fd_ = -1;
        }
    }
};

} // namespace maya::platform::posix
