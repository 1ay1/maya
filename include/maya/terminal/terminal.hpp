#pragma once
// maya::terminal - RAII type-state terminal
//
// The safety centerpiece of the library. The terminal's mode (Cooked, Raw,
// AltScreen) is encoded in the type system. A Terminal<Raw> cannot exist
// unless a Terminal<Cooked> was moved through enable_raw_mode(). The
// destructor always restores the original terminal state - no leaked modes,
// no orphaned alt screens, no hidden cursors.

#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "../core/expected.hpp"
#include "../core/types.hpp"
#include "ansi.hpp"

namespace maya {

// Forward declarations - input parsing lives in input.hpp
class InputParser;

// ============================================================================
// RawGuard - Standalone RAII guard for raw mode
// ============================================================================
// For code that needs raw mode without the full Terminal type-state machinery.
// Saves termios on construction, restores on destruction.

class RawGuard : MoveOnly {
    int fd_;
    struct termios original_;
    bool active_ = false;

public:
    [[nodiscard]] static auto create(int fd = STDIN_FILENO) -> Result<RawGuard> {
        RawGuard guard;
        guard.fd_ = fd;

        if (::tcgetattr(fd, &guard.original_) < 0) {
            return err<RawGuard>(Error::from_errno("tcgetattr"));
        }

        struct termios raw = guard.original_;
        ::cfmakeraw(&raw);
        raw.c_cc[VMIN]  = 0;
        raw.c_cc[VTIME] = 1; // 100ms timeout

        if (::tcsetattr(fd, TCSAFLUSH, &raw) < 0) {
            return err<RawGuard>(Error::from_errno("tcsetattr"));
        }

        guard.active_ = true;
        return ok(std::move(guard));
    }

    RawGuard() = default;

    RawGuard(RawGuard&& other) noexcept
        : fd_(other.fd_)
        , original_(other.original_)
        , active_(std::exchange(other.active_, false))
    {}

    RawGuard& operator=(RawGuard&& other) noexcept {
        if (this != &other) {
            restore();
            fd_       = other.fd_;
            original_ = other.original_;
            active_   = std::exchange(other.active_, false);
        }
        return *this;
    }

    ~RawGuard() { restore(); }

    void restore() noexcept {
        if (active_) {
            ::tcsetattr(fd_, TCSAFLUSH, &original_);
            active_ = false;
        }
    }

    [[nodiscard]] bool is_active() const noexcept { return active_; }
};

// ============================================================================
// Terminal<State> - Type-state terminal
// ============================================================================
// State is one of: Cooked, Raw, AltScreen (defined in core/types.hpp).
//
// Transitions consume the source object and return a new object in the
// target state. This is enforced by C++23 explicit object parameters
// (deducing this) taking `self` by value.
//
// Construction: Terminal<Cooked>::create(fd)
// Transitions:  Cooked -> Raw -> AltScreen
// Destruction:  restores everything in reverse order

namespace detail {
    struct TerminalPrivateTag {};
}

template <typename State = Cooked>
class Terminal : MoveOnly {
    int fd_;
    struct termios original_;
    bool owns_fd_ = false; // true if we should close fd on destruction

    // Private constructor - only create() and state transitions may construct
    using PrivateTag = detail::TerminalPrivateTag;
    Terminal(PrivateTag, int fd, struct termios orig)
        : fd_(fd), original_(orig) {}

    // Allow other Terminal instantiations to access private members
    template <typename OtherState>
    friend class Terminal;

public:
    // ========================================================================
    // Creation
    // ========================================================================

    /// The only way to obtain a terminal. Saves the original termios state.
    [[nodiscard]] static auto create(int fd = STDIN_FILENO) -> Result<Terminal<Cooked>>
        requires std::same_as<State, Cooked>
    {
        struct termios orig{};
        if (::tcgetattr(fd, &orig) < 0) {
            return err<Terminal<Cooked>>(Error::from_errno("tcgetattr: failed to get terminal attributes"));
        }
        return ok(Terminal<Cooked>{PrivateTag{}, fd, orig});
    }

    // ========================================================================
    // Type-state transitions (consume self, return new state)
    // ========================================================================

    /// Cooked -> Raw: enables raw mode (cfmakeraw, disable echo, VMIN/VTIME)
    [[nodiscard]] auto enable_raw_mode(this Terminal<Cooked> self) -> Result<Terminal<Raw>> {
        struct termios raw = self.original_;
        ::cfmakeraw(&raw);
        raw.c_lflag &= ~static_cast<tcflag_t>(ECHO);  // explicitly disable echo
        raw.c_cc[VMIN]  = 0;   // non-blocking reads
        raw.c_cc[VTIME] = 1;   // 100ms read timeout

        if (::tcsetattr(self.fd_, TCSAFLUSH, &raw) < 0) {
            // Restore is not needed - self still holds Cooked and its
            // destructor (which does nothing for Cooked) will run.
            return err<Terminal<Raw>>(Error::from_errno("tcsetattr: failed to enable raw mode"));
        }

        Terminal<Raw> result{PrivateTag{}, self.fd_, self.original_};
        self.fd_ = -1; // prevent Cooked destructor from touching fd
        return ok(std::move(result));
    }

    /// Raw -> AltScreen: enters alt screen buffer + hide cursor + enable
    /// mouse tracking + enable focus events + enable bracketed paste
    [[nodiscard]] auto enter_alt_screen(this Terminal<Raw> self) -> Result<Terminal<AltScreen>> {
        // Build the mode-enter sequence as a single write
        std::string seq;
        seq.reserve(128);
        seq += ansi::alt_screen_enter;
        seq += ansi::hide_cursor;
        seq += ansi::enable_mouse;
        seq += ansi::enable_alt_scroll;
        seq += ansi::enable_focus;
        seq += ansi::enable_bracketed_paste;
        seq += ansi::clear_screen();
        seq += ansi::home();

        auto written = ::write(self.fd_, seq.data(), seq.size());
        if (written < 0) {
            // Self (Raw) will be destroyed, restoring raw -> cooked
            return err<Terminal<AltScreen>>(Error::from_errno("write: failed to enter alt screen"));
        }

        Terminal<AltScreen> result{PrivateTag{}, self.fd_, self.original_};
        self.fd_ = -1; // prevent Raw destructor from partial cleanup
        return ok(std::move(result));
    }

    // ========================================================================
    // Operations available in all non-Cooked states
    // ========================================================================

    /// Write raw bytes to the terminal.
    auto write(std::string_view data) -> Status
        requires (!std::same_as<State, Cooked>)
    {
        const char* ptr = data.data();
        auto remaining  = static_cast<ssize_t>(data.size());

        while (remaining > 0) {
            ssize_t n = ::write(fd_, ptr, static_cast<size_t>(remaining));
            if (n < 0) {
                if (errno == EINTR) continue;
                return err(Error::from_errno("write"));
            }
            ptr       += n;
            remaining -= n;
        }
        return ok();
    }

    /// Query the terminal dimensions.
    [[nodiscard]] auto size() const -> Size
        requires (!std::same_as<State, Cooked>)
    {
        struct winsize ws{};
        if (::ioctl(fd_, TIOCGWINSZ, &ws) < 0) {
            // Fallback: 80x24 is the historical default
            return {Columns{80}, Rows{24}};
        }
        return {Columns{ws.ws_col}, Rows{ws.ws_row}};
    }

    /// Read raw bytes from the terminal file descriptor.
    /// Returns a string of raw bytes for the input parser to process.
    [[nodiscard]] auto read_raw() -> Result<std::string>
        requires (!std::same_as<State, Cooked>)
    {
        char buf[256];
        ssize_t n = ::read(fd_, buf, sizeof(buf));

        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                return ok(std::string{});
            }
            return err<std::string>(Error::from_errno("read"));
        }

        return ok(std::string(buf, static_cast<size_t>(n)));
    }

    /// Get the underlying file descriptor (for polling, etc.)
    [[nodiscard]] int fd() const noexcept { return fd_; }

    // ========================================================================
    // Destructor - RAII cleanup in reverse order of state transitions
    // ========================================================================

    ~Terminal() {
        if (fd_ < 0) return; // moved-from state

        if constexpr (std::same_as<State, AltScreen>) {
            // Reverse of enter_alt_screen: disable paste, focus, mouse,
            // show cursor, leave alt screen
            std::string seq;
            seq.reserve(128);
            seq += ansi::disable_bracketed_paste;
            seq += ansi::disable_focus;
            seq += ansi::disable_alt_scroll;
            seq += ansi::disable_mouse;
            seq += ansi::show_cursor;
            seq += ansi::alt_screen_leave;
            ::write(fd_, seq.data(), seq.size());

            // Also restore original termios (undo raw mode)
            ::tcsetattr(fd_, TCSAFLUSH, &original_);
        }
        else if constexpr (std::same_as<State, Raw>) {
            // Restore original termios, show cursor just in case
            ::write(fd_, ansi::show_cursor.data(), ansi::show_cursor.size());
            ::tcsetattr(fd_, TCSAFLUSH, &original_);
        }
        // Cooked: nothing to restore
    }

    // ========================================================================
    // Move operations
    // ========================================================================

    Terminal(Terminal&& other) noexcept
        : fd_(std::exchange(other.fd_, -1))
        , original_(other.original_)
    {}

    Terminal& operator=(Terminal&& other) noexcept {
        if (this != &other) {
            // Destroy current state first
            this->~Terminal();
            fd_       = std::exchange(other.fd_, -1);
            original_ = other.original_;
        }
        return *this;
    }
};

} // namespace maya
