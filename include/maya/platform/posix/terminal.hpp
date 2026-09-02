#pragma once
// maya::platform::posix::PosixTerminal - termios-based terminal backend
//
// Implements TerminalBackend for POSIX systems (Linux, macOS, *BSD).
// Uses termios for raw mode, ioctl(TIOCGWINSZ) for size, and
// read()/write() for byte-level I/O.

#if !MAYA_PLATFORM_POSIX
#error "This header is for POSIX platforms only"
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

namespace maya::platform::posix {

// ============================================================================
// PosixTerminal — termios backend
// ============================================================================

class PosixTerminal {
    NativeHandle in_fd_;
    NativeHandle out_fd_;
    struct termios original_{};
    bool raw_ = false;

    PosixTerminal() = default;

public:
    // -- Construction ---------------------------------------------------------

    [[nodiscard]] static auto open() -> Result<PosixTerminal> {
        PosixTerminal t;
        t.in_fd_  = STDIN_FILENO;
        t.out_fd_ = STDOUT_FILENO;

        if (::tcgetattr(t.in_fd_, &t.original_) < 0)
            return err<PosixTerminal>(
                Error::from_errno("tcgetattr: failed to get terminal attributes"));

        return ok(std::move(t));
    }

    // -- Raw mode -------------------------------------------------------------

    [[nodiscard]] auto enable_raw() -> Status {
        struct termios raw = original_;
        ::cfmakeraw(&raw);
        raw.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        raw.c_cc[VMIN]  = 0;   // non-blocking reads
        raw.c_cc[VTIME] = 1;   // 100ms read timeout

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

    // -- I/O ------------------------------------------------------------------

    [[nodiscard]] auto write_all(std::string_view data) -> Status {
        return io_write_all(out_fd_, data);
    }

    [[nodiscard]] auto write_some(std::string_view data) -> Result<std::size_t> {
        return io_write(out_fd_, data.data(), data.size());
    }

    [[nodiscard]] auto read_raw() -> Result<std::string> {
        // Drain what is available, not one small mouthful.
        //
        // This used to read 256 bytes per call, and the caller's loop is
        // poll -> read -> parse -> maybe render. A clipboard image arrives
        // as base64 measured in megabytes, so 256-byte reads turned one
        // paste into ~20k poll/read round-trips: seconds of apparent hang
        // with no frame drawn, because no render happens until the whole
        // OSC has been consumed. Parsing was never the cost (4 MB parses in
        // ~20 ms); the syscall count was.
        //
        // 64 KiB matches the pipe/pty buffer, so a burst usually lands in
        // one or two reads. The loop then keeps pulling while the fd has
        // more, but stops at kMaxBurst so a peer streaming without pause
        // cannot starve rendering and input handling.
        //
        // Safe on a BLOCKING fd too: we only go round again after a read
        // that filled the buffer completely, which means more was waiting.
        // A short read — including the 0 that io_read reports for
        // EAGAIN/EINTR — ends the burst, so we never block for bytes that
        // have not arrived.
        constexpr std::size_t kChunk    = 64u * 1024u;
        constexpr std::size_t kMaxBurst = 4u * 1024u * 1024u;

        // Heap, not stack: 64 KiB is beyond a comfortable frame budget on
        // small-stack threads, and this is reused across the whole burst.
        static thread_local std::vector<char> buf(kChunk);

        std::string out;
        for (;;) {
            auto result = io_read(in_fd_, buf.data(), buf.size());
            if (!result) {
                // A partial burst is still real input: hand back what we
                // have and let the next poll surface the error.
                if (!out.empty()) return ok(std::move(out));
                return err<std::string>(result.error());
            }
            const std::size_t n = *result;
            if (n == 0) break;                 // EAGAIN/EINTR/EOF: nothing more
            out.append(buf.data(), n);
            if (n < buf.size()) break;         // short read = fd drained
            if (out.size() >= kMaxBurst) break;
        }
        return ok(std::move(out));
    }

    // -- Properties -----------------------------------------------------------

    [[nodiscard]] auto size() const -> Size {
        return query_terminal_size(out_fd_);
    }

    [[nodiscard]] NativeHandle input_handle() const noexcept { return in_fd_; }
    [[nodiscard]] NativeHandle output_handle() const noexcept { return out_fd_; }

    // The original cooked termios captured at open(), for the emergency
    // restore path (so a crash/kill can put the tty back to cooked mode).
    [[nodiscard]] const struct termios& cooked_termios() const noexcept { return original_; }

    // -- Move-only ------------------------------------------------------------

    PosixTerminal(PosixTerminal&& o) noexcept
        : in_fd_(std::exchange(o.in_fd_, -1))
        , out_fd_(std::exchange(o.out_fd_, -1))
        , original_(o.original_)
        , raw_(std::exchange(o.raw_, false))
    {}

    PosixTerminal& operator=(PosixTerminal&& o) noexcept {
        if (this != &o) {
            if (raw_) ::tcsetattr(in_fd_, TCSAFLUSH, &original_);
            in_fd_    = std::exchange(o.in_fd_, -1);
            out_fd_   = std::exchange(o.out_fd_, -1);
            original_ = o.original_;
            raw_      = std::exchange(o.raw_, false);
        }
        return *this;
    }

    PosixTerminal(const PosixTerminal&) = delete;
    PosixTerminal& operator=(const PosixTerminal&) = delete;

    ~PosixTerminal() {
        if (raw_) ::tcsetattr(in_fd_, TCSAFLUSH, &original_);
    }
};

} // namespace maya::platform::posix
