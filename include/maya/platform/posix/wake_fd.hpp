#pragma once
// maya::platform::posix::PosixWakeFd - Many-writer / one-reader wake.
//
// Hides the eventfd vs self-pipe choice from the rest of maya. Linux
// gets eventfd by default (one fd, atomic counter, one syscall to
// drain) and falls back to a self-pipe if eventfd() fails — common
// in seccomp sandboxes that filter out the syscall, or under fd
// table pressure.
//
// Lifetime model:
//   Move-only. The destructor closes the underlying fd(s). Used inside
//   a shared_ptr by BackgroundQueue so detached IsolatedTask threads
//   can keep the wake alive past runtime exit — once they finish the
//   shared_ptr drops and ~PosixWakeFd closes everything.
//
// Robustness:
//   create() returns Result<PosixWakeFd>. signal() and drain() are
//   noexcept and no-op on invalid state — they never throw, never
//   abort, never write to a recycled fd, even if construction failed.

#if !MAYA_PLATFORM_POSIX && !MAYA_PLATFORM_MACOS
#error "This header is for POSIX-family platforms only"
#endif

#include <cerrno>
#include <cstdint>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/eventfd.h>
#endif

#include "../../core/expected.hpp"
#include "../io.hpp"

namespace maya::platform::posix {

class PosixWakeFd {
    // Linux: eventfd path uses fd_ (read fd == write fd).
    // Other POSIX: self-pipe uses read_fd_ + write_fd_, fd_ stays -1.
    // Linux fallback (eventfd blocked): same self-pipe path.
    int fd_       = -1;
    int read_fd_  = -1;
    int write_fd_ = -1;

public:
    // ── Construction ──────────────────────────────────────────────────
    // Default-constructed instance is the "invalid wake" sentinel —
    // signal() and drain() are no-ops, native_handle() returns
    // invalid_handle, valid() returns false. Used as a degraded
    // fallback when create() fails: dispatch still works (queue
    // drained by the runtime's polling main loop) just without
    // event-driven wake.
    PosixWakeFd() = default;
    [[nodiscard]] static auto create() noexcept -> Result<PosixWakeFd> {
        PosixWakeFd w;

#ifdef __linux__
        // Try eventfd first — single fd, atomic 64-bit counter, one
        // syscall per signal/drain. EFD_NONBLOCK so drain() doesn't
        // block; EFD_CLOEXEC so subprocesses don't inherit the wake.
        int efd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (efd >= 0) {
            w.fd_ = efd;
            return ok(std::move(w));
        }
        // eventfd unavailable — fall through to pipe fallback.
#endif

        if (auto status = w.init_pipe(); !status)
            return err<PosixWakeFd>(status.error());
        return ok(std::move(w));
    }

    // ── Read-side accessor ────────────────────────────────────────────
    [[nodiscard]] NativeHandle native_handle() const noexcept {
        if (fd_      >= 0) return fd_;        // eventfd
        if (read_fd_ >= 0) return read_fd_;   // pipe fallback
        return invalid_handle;
    }

    [[nodiscard]] bool valid() const noexcept {
        return fd_ >= 0 || read_fd_ >= 0;
    }

    // ── Signal / drain ────────────────────────────────────────────────
    void signal() noexcept {
        if (fd_ >= 0) {
            // eventfd: write 8-byte counter increment. EAGAIN if the
            // counter would overflow (UINT64_MAX-1) — vanishingly rare,
            // and the next read drains the saturated value either way,
            // so silently ignore.
            const uint64_t val = 1;
            (void)::write(fd_, &val, sizeof(val));
            return;
        }
        if (write_fd_ >= 0) {
            // pipe: 1-byte signal. EAGAIN if the pipe is full — that
            // means the reader hasn't drained yet, so the wake is
            // already pending. Coalescing is automatic.
            const char byte = 1;
            (void)::write(write_fd_, &byte, 1);
        }
    }

    void drain() noexcept {
        if (fd_ >= 0) {
            uint64_t val;
            (void)::read(fd_, &val, sizeof(val));
            return;
        }
        if (read_fd_ >= 0) {
            // pipe: drain everything pending. Bounded buffer + NONBLOCK
            // means the loop terminates when the kernel has nothing
            // queued — never blocks the UI thread.
            char buf[4096];
            while (::read(read_fd_, buf, sizeof(buf)) > 0) {}
        }
    }

    // ── Move-only ─────────────────────────────────────────────────────
    PosixWakeFd(PosixWakeFd&& o) noexcept
        : fd_      (std::exchange(o.fd_,      -1))
        , read_fd_ (std::exchange(o.read_fd_, -1))
        , write_fd_(std::exchange(o.write_fd_, -1))
    {}

    PosixWakeFd& operator=(PosixWakeFd&& o) noexcept {
        if (this != &o) {
            close_all();
            fd_       = std::exchange(o.fd_,       -1);
            read_fd_  = std::exchange(o.read_fd_,  -1);
            write_fd_ = std::exchange(o.write_fd_, -1);
        }
        return *this;
    }

    PosixWakeFd(const PosixWakeFd&)            = delete;
    PosixWakeFd& operator=(const PosixWakeFd&) = delete;

    ~PosixWakeFd() noexcept { close_all(); }

private:
    Status init_pipe() noexcept {
        int fds[2];
        if (::pipe(fds) != 0)
            return std::unexpected{Error::from_errno("pipe")};

        for (int fd : fds) {
            int flags = ::fcntl(fd, F_GETFL);
            if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
                // O_NONBLOCK is non-negotiable — drain() relies on
                // read() returning EAGAIN on empty. A blocking pipe
                // would wedge the UI thread inside drain().
                ::close(fds[0]); ::close(fds[1]);
                return std::unexpected{Error::from_errno("fcntl(O_NONBLOCK)")};
            }
            // FD_CLOEXEC failure is non-fatal (subprocess hygiene only).
            (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
        read_fd_  = fds[0];
        write_fd_ = fds[1];
        return ok();
    }

    void close_all() noexcept {
        if (fd_       >= 0) { ::close(fd_);       fd_       = -1; }
        if (read_fd_  >= 0) { ::close(read_fd_);  read_fd_  = -1; }
        if (write_fd_ >= 0) { ::close(write_fd_); write_fd_ = -1; }
    }
};

} // namespace maya::platform::posix
