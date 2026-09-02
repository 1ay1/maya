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
#include <vector>
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
        // Drain the fd, don't take one mouthful per poll. 4 KiB "handles a
        // paste in one syscall" only for a TYPED-size paste; a clipboard
        // image is base64 measured in megabytes, which at 4 KiB a poll is
        // ~1.3k round-trips with no frame drawn between them. Read in
        // 64 KiB chunks and keep going while the fd keeps filling them.
        //
        // Blocking-safe: another read is only attempted after one that
        // filled the buffer, i.e. more was definitely waiting. Any short
        // read (including the 0 io_read reports for EAGAIN/EINTR) ends the
        // burst. kMaxBurst keeps a nonstop peer from starving the frame.
        constexpr std::size_t kChunk    = 64u * 1024u;
        constexpr std::size_t kMaxBurst = 4u * 1024u * 1024u;

        static thread_local std::vector<char> buf(kChunk);

        std::string out;
        for (;;) {
            auto result = io_read(in_fd_, buf.data(), buf.size());
            if (!result) {
                if (!out.empty()) return ok(std::move(out));
                return err<std::string>(result.error());
            }
            const std::size_t n = *result;
            if (n == 0) break;
            out.append(buf.data(), n);
            if (n < buf.size()) break;
            if (out.size() >= kMaxBurst) break;
        }
        return ok(std::move(out));
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
