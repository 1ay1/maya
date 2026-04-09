#pragma once
// maya::platform::macos::MacEventSource - kqueue-based event multiplexer
//
// Uses macOS-native kqueue instead of poll(). kqueue is the kernel's
// native event notification mechanism — zero-copy, O(1) dispatch,
// and supports watching file descriptors + signals in a single call.

#if !MAYA_PLATFORM_MACOS
#error "This header is for macOS only"
#endif

#include <cerrno>
#include <chrono>
#include <utility>

#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

#include "../../core/expected.hpp"
#include "../concepts.hpp"
#include "../io.hpp"

namespace maya::platform::macos {

// ============================================================================
// MacEventSource — kqueue-based multiplexer
// ============================================================================

class MacEventSource {
    int kq_ = -1;
    NativeHandle terminal_fd_;

    MacEventSource() = default;

public:
    MacEventSource(NativeHandle term_fd, NativeHandle /*sig_fd*/) noexcept
        : terminal_fd_(term_fd)
    {
        kq_ = ::kqueue();
        if (kq_ < 0) return;   // fallback: wait() will return error

        // Register stdin for read events.
        // NOTE_LOWAT=1: only wake when at least 1 byte available,
        // reducing spurious wakeups during high-frequency input.
        struct kevent changes[2];
        int nchanges = 0;

        EV_SET(&changes[nchanges++], term_fd, EVFILT_READ,
               EV_ADD | EV_ENABLE, NOTE_LOWAT, 1, nullptr);

        // Register SIGWINCH as a kernel event — no self-pipe needed
        // Signal delivery is handled by the kqueue directly
        EV_SET(&changes[nchanges++], SIGWINCH, EVFILT_SIGNAL,
               EV_ADD | EV_ENABLE, 0, 0, nullptr);

        // Ignore SIGWINCH default handler so kqueue gets it
        ::signal(SIGWINCH, SIG_IGN);

        ::kevent(kq_, changes, nchanges, nullptr, 0, nullptr);
    }

    [[nodiscard]] auto wait(
        std::chrono::milliseconds timeout,
        bool /*want_write*/ = false) -> Result<ReadyFlags>
    {
        if (kq_ < 0)
            return err<ReadyFlags>(Error::io("kqueue: failed to create"));

        struct timespec ts;
        ts.tv_sec  = timeout.count() / 1000;
        ts.tv_nsec = (timeout.count() % 1000) * 1000000;

        struct kevent events[4];
        int n = ::kevent(kq_, nullptr, 0, events, 4, &ts);

        if (n < 0) {
            if (errno == EINTR)
                return ok(ReadyFlags{});
            return err<ReadyFlags>(Error::from_errno("kevent"));
        }

        ReadyFlags flags{};
        for (int i = 0; i < n; ++i) {
            if (events[i].filter == EVFILT_READ &&
                static_cast<int>(events[i].ident) == terminal_fd_) {
                flags.input = true;
            }
            if (events[i].filter == EVFILT_SIGNAL &&
                events[i].ident == static_cast<uintptr_t>(SIGWINCH)) {
                flags.resize = true;
            }
        }
        return ok(flags);
    }

    // -- Move-only ------------------------------------------------------------

    MacEventSource(MacEventSource&& o) noexcept
        : kq_(std::exchange(o.kq_, -1))
        , terminal_fd_(std::exchange(o.terminal_fd_, -1))
    {}

    MacEventSource& operator=(MacEventSource&& o) noexcept {
        if (this != &o) {
            if (kq_ >= 0) ::close(kq_);
            kq_          = std::exchange(o.kq_, -1);
            terminal_fd_ = std::exchange(o.terminal_fd_, -1);
        }
        return *this;
    }

    MacEventSource(const MacEventSource&) = delete;
    MacEventSource& operator=(const MacEventSource&) = delete;

    ~MacEventSource() {
        if (kq_ >= 0) ::close(kq_);
    }
};

} // namespace maya::platform::macos
