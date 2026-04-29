#pragma once
// maya::platform::posix::PosixResizeSignal - SIGWINCH via self-pipe
//
// Installs a SIGWINCH handler that writes a byte to a pipe.
// The read end is polled by PosixEventSource. This is the classic
// self-pipe trick for integrating async signals into an event loop
// without async-signal-safety violations.
//
// Lifetime model: the pipe + handler are a process-global singleton owned
// by a std::shared_ptr.  install() returns a non-owning *handle* that
// keeps the singleton alive through its embedded shared_ptr.  Multiple
// concurrent installers (e.g. opening a Runtime, closing it, opening
// another) all get a valid native_handle() pointing at the same pipe.
// When the last handle drops, the pipe is closed and the handler is
// uninstalled.  This eliminates the "second installer gets invalid fd"
// hazard the previous design had.

#if !MAYA_PLATFORM_POSIX
#error "This header is for POSIX platforms only"
#endif

#include <cerrno>
#include <csignal>
#include <memory>
#include <mutex>
#include <utility>

#include <fcntl.h>
#include <pthread.h>
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

// volatile sig_atomic_t is the only standard-blessed type for variables
// touched by both a signal handler and the rest of the program. Plain
// int loads are atomic on every supported target in practice, but this
// is what the standard guarantees.
inline volatile sig_atomic_t g_signal_write_fd = -1;

inline void sigwinch_handler(int /*sig*/) {
    // Async-signal-safe: write(2) is safe. Ignore errors —
    // pipe full is harmless; the loop will see the resize anyway.
    int fd = g_signal_write_fd;
    if (fd >= 0) {
        [[maybe_unused]] auto _ = ::write(fd, "R", 1);
    }
}

// Refcounted pipe + handler bundle.  Constructed inside install() under a
// mutex; destructed when the last shared_ptr referring to it drops.  The
// destructor restores SIGWINCH to SIG_DFL *before* closing the fds and
// before clearing g_signal_write_fd, so an in-flight handler invocation
// either runs against a still-valid fd (write succeeds, harmless) or
// short-circuits on the volatile sig_atomic_t guard (read sees -1, skips).
struct ResizePipe {
    int read_fd  = -1;
    int write_fd = -1;

    ResizePipe(int r, int w) noexcept : read_fd(r), write_fd(w) {}

    ResizePipe(const ResizePipe&)            = delete;
    ResizePipe& operator=(const ResizePipe&) = delete;

    ~ResizePipe() noexcept {
        // Block SIGWINCH for the duration of teardown. Without this,
        // a handler invocation that has already executed `int fd =
        // g_signal_write_fd;` but hasn't yet called `write(fd, ...)`
        // could race our close(write_fd) and (a) get EBADF — harmless
        // — or (b) write a stray byte to whatever fd the kernel
        // recycles for `write_fd` next. Tiny window, but the cost of
        // closing it is one sigprocmask pair.
        sigset_t mask, prev;
        sigemptyset(&mask);
        sigaddset(&mask, SIGWINCH);
#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
        // pthread_sigmask is the multi-thread-safe variant; sigprocmask
        // is undefined behaviour in the presence of multiple threads
        // per POSIX. Maya apps may have detached workers running, so
        // use pthread_sigmask whenever we have it.
        ::pthread_sigmask(SIG_BLOCK, &mask, &prev);
#else
        ::sigprocmask(SIG_BLOCK, &mask, &prev);
#endif

        // Order is load-bearing.  (1) un-install the handler so no NEW
        // signal will run our function; pending signals delivered while
        // the new disposition is being installed will see SIG_DFL
        // (which for SIGWINCH is "ignore").  (2) clear the global fd
        // so any handler that might still be mid-flight sees -1 and
        // skips its write.  (3) close the fds — at this point no
        // writer can still be live.
        struct sigaction sa{};
        sa.sa_handler = SIG_DFL;
        ::sigaction(SIGWINCH, &sa, nullptr);

        g_signal_write_fd = -1;

        if (read_fd  >= 0) ::close(read_fd);
        if (write_fd >= 0) ::close(write_fd);

#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
        ::pthread_sigmask(SIG_SETMASK, &prev, nullptr);
#else
        ::sigprocmask(SIG_SETMASK, &prev, nullptr);
#endif
    }
};

// Singleton accessor.  install() locks the mutex, upgrades the weak_ptr
// — if the singleton is alive, return the same shared_ptr; otherwise
// allocate the pipe + register the handler and stash a weak_ptr.
inline std::mutex&                  install_mutex() {
    static std::mutex m;
    return m;
}
inline std::weak_ptr<ResizePipe>&   install_singleton() {
    static std::weak_ptr<ResizePipe> w;
    return w;
}

} // namespace detail

// ============================================================================
// PosixResizeSignal — RAII handle on the SIGWINCH self-pipe singleton
// ============================================================================

class PosixResizeSignal {
    std::shared_ptr<detail::ResizePipe> pipe_;

    PosixResizeSignal() = default;
    explicit PosixResizeSignal(std::shared_ptr<detail::ResizePipe> p) noexcept
        : pipe_(std::move(p)) {}

public:
    [[nodiscard]] static auto install() -> Result<PosixResizeSignal> {
        std::lock_guard lk(detail::install_mutex());

        // Singleton is alive — share the existing pipe.  Both installers
        // see the same read_fd, the same handler, and a refcount that
        // keeps the pipe alive until the last handle drops.
        if (auto existing = detail::install_singleton().lock()) {
            return ok(PosixResizeSignal{std::move(existing)});
        }

        // No live singleton — allocate a fresh one.
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
        sigemptyset(&sa.sa_mask);
        if (::sigaction(SIGWINCH, &sa, nullptr) < 0) {
            ::close(fds[0]);
            ::close(fds[1]);
            detail::g_signal_write_fd = -1;
            return err<PosixResizeSignal>(Error::from_errno("sigaction(SIGWINCH)"));
        }

        auto pipe = std::make_shared<detail::ResizePipe>(fds[0], fds[1]);
        detail::install_singleton() = pipe;
        return ok(PosixResizeSignal{std::move(pipe)});
    }

    [[nodiscard]] bool pending() const noexcept {
        return pipe_ && pipe_->read_fd >= 0;
    }

    void drain() noexcept {
        if (!pipe_ || pipe_->read_fd < 0) return;
        char buf[64];
        while (::read(pipe_->read_fd, buf, sizeof(buf)) > 0) {}
    }

    [[nodiscard]] NativeHandle native_handle() const noexcept {
        return pipe_ ? pipe_->read_fd : -1;
    }

    // -- Move-only (shared_ptr handles refcount) ------------------------------

    PosixResizeSignal(PosixResizeSignal&&)                 noexcept = default;
    PosixResizeSignal& operator=(PosixResizeSignal&&)      noexcept = default;
    PosixResizeSignal(const PosixResizeSignal&)            = delete;
    PosixResizeSignal& operator=(const PosixResizeSignal&) = delete;
    ~PosixResizeSignal()                                   = default;
};

} // namespace maya::platform::posix
