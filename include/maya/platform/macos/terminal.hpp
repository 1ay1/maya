#pragma once
// maya::platform::macos::MacTerminal - termios backend for macOS
//
// macOS terminal I/O is termios-based, same as generic POSIX.
// This thin wrapper reuses the POSIX terminal implementation
// under the macos namespace for consistent platform dispatch.

#if !MAYA_PLATFORM_MACOS
#error "This header is for macOS only"
#endif

#include <cerrno>
#include <string>
#include <string_view>
#include <utility>

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "../../core/expected.hpp"
#include "../../core/types.hpp"
#include "../io.hpp"

namespace maya::platform::macos {

// ============================================================================
// MacTerminal — termios backend (identical to POSIX, macOS-native)
// ============================================================================

class MacTerminal {
    NativeHandle in_fd_;
    NativeHandle out_fd_;
    struct termios original_{};
    bool raw_ = false;

    MacTerminal() = default;

public:
    [[nodiscard]] static auto open() -> Result<MacTerminal> {
        MacTerminal t;
        t.in_fd_  = STDIN_FILENO;
        t.out_fd_ = STDOUT_FILENO;

        if (::tcgetattr(t.in_fd_, &t.original_) < 0)
            return err<MacTerminal>(
                Error::from_errno("tcgetattr: failed to get terminal attributes"));

        return ok(std::move(t));
    }

    [[nodiscard]] auto enable_raw() -> Status {
        struct termios raw = original_;
        ::cfmakeraw(&raw);
        raw.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        // Pure non-blocking: kqueue handles all waiting, so read()
        // should return immediately with whatever is available.
        raw.c_cc[VMIN]  = 0;
        raw.c_cc[VTIME] = 0;

        if (::tcsetattr(in_fd_, TCSAFLUSH, &raw) < 0)
            return err(Error::from_errno("tcsetattr: failed to enable raw mode"));

        raw_ = true;
        return ok();
    }

    [[nodiscard]] auto disable_raw() -> Status {
        if (raw_) {
            if (::tcsetattr(in_fd_, TCSAFLUSH, &original_) < 0)
                return err(Error::from_errno("tcsetattr: failed to restore terminal"));
            raw_ = false;
        }
        return ok();
    }

    [[nodiscard]] auto write_all(std::string_view data) -> Status {
        return io_write_all(out_fd_, data);
    }

    [[nodiscard]] auto write_some(std::string_view data) -> Result<std::size_t> {
        return io_write(out_fd_, data.data(), data.size());
    }

    [[nodiscard]] auto read_raw() -> Result<std::string> {
        char buf[4096];   // 4KB: handles paste events in one syscall
        auto result = io_read(in_fd_, buf, sizeof(buf));
        if (!result) return err<std::string>(result.error());
        return ok(std::string(buf, *result));
    }

    [[nodiscard]] auto size() const -> Size {
        return query_terminal_size(out_fd_);
    }

    [[nodiscard]] NativeHandle input_handle() const noexcept { return in_fd_; }
    [[nodiscard]] NativeHandle output_handle() const noexcept { return out_fd_; }

    MacTerminal(MacTerminal&& o) noexcept
        : in_fd_(std::exchange(o.in_fd_, -1))
        , out_fd_(std::exchange(o.out_fd_, -1))
        , original_(o.original_)
        , raw_(std::exchange(o.raw_, false))
    {}

    MacTerminal& operator=(MacTerminal&& o) noexcept {
        if (this != &o) {
            if (raw_) ::tcsetattr(in_fd_, TCSAFLUSH, &original_);
            in_fd_    = std::exchange(o.in_fd_, -1);
            out_fd_   = std::exchange(o.out_fd_, -1);
            original_ = o.original_;
            raw_      = std::exchange(o.raw_, false);
        }
        return *this;
    }

    MacTerminal(const MacTerminal&) = delete;
    MacTerminal& operator=(const MacTerminal&) = delete;

    ~MacTerminal() {
        if (raw_) ::tcsetattr(in_fd_, TCSAFLUSH, &original_);
    }
};

} // namespace maya::platform::macos
