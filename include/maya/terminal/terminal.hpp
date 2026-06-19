#pragma once
// maya::terminal - RAII type-state terminal
//
// The safety centerpiece of the library. The terminal's mode (Cooked, Raw,
// AltScreen) is encoded in the type system. A Terminal<Raw> cannot exist
// unless a Terminal<Cooked> was moved through enable_raw_mode(). The
// destructor always restores the original terminal state - no leaked modes,
// no orphaned alt screens, no hidden cursors.
//
// Generic over TerminalBackend — the platform backend is selected at
// compile time via platform::NativeTerminal. No vtable, no indirection.

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#if MAYA_PLATFORM_POSIX || MAYA_PLATFORM_MACOS
    #include <cerrno>       // errno, EINTR (for emergency_emit's retry loop)
    #include <unistd.h>     // ::write, STDOUT_FILENO
    #include <csignal>      // sigaction, raise — restore tty on fatal signals
    #include <termios.h>    // tcsetattr — restore cooked mode in emergency
#endif

#include "../core/expected.hpp"
#include "../core/types.hpp"
#include "../platform/select.hpp"
#include "ansi.hpp"

namespace maya {

// Forward declarations - input parsing lives in input.hpp
class InputParser;

// ============================================================================
// Emergency terminal restore (std::set_terminate + std::atexit hook)
// ============================================================================
//
// The Terminal destructor restores DECAWM, cursor visibility, KKP stack,
// bracketed-paste, and raw-mode on normal scope exit. But uncaught
// exceptions invoke `std::terminate` BEFORE any destructors run, and
// raw `std::exit()` from a tool subprocess wrapper bypasses static
// destructors entirely. Both leave the terminal in raw inline mode with
// a hidden cursor, DECAWM off, and KKP pushed — the user's shell prints
// garbage until they manually `reset` their tty.
//
// install_emergency_restore() stashes the bytes the destructor would
// emit, then registers a `std::set_terminate` handler and an `atexit`
// callback that write those bytes directly to STDOUT_FILENO via raw
// `::write` (async-signal-safe, doesn't depend on a backend that may
// already be destroyed by the time terminate runs). clear_emergency_
// restore() disarms the handler on normal scope exit so a subsequent
// exception in unrelated code doesn't re-emit stale sequences.

namespace detail {

struct EmergencyState {
    std::atomic<bool>           active{false};
    int                         fd{-1};
    std::array<char, 128>       seq{};
    std::size_t                 seq_len{0};
    std::terminate_handler      prior_terminate{nullptr};
#if MAYA_PLATFORM_POSIX || MAYA_PLATFORM_MACOS
    // Cooked termios to restore, so a crash/kill doesn't leave the tty in raw
    // mode (no echo/line-editing). Set when raw mode is enabled.
    bool                        have_termios{false};
    int                         termios_fd{-1};
    struct termios              cooked{};
    // Prior dispositions for the fatal signals we hook, so we can chain to a
    // host's handler (or the default) after restoring the terminal.
    std::array<struct sigaction, 16> prior_signal{};
#endif
};

inline EmergencyState& emergency_state() {
    static EmergencyState s;
    return s;
}

inline void emergency_emit() noexcept {
#if MAYA_PLATFORM_POSIX || MAYA_PLATFORM_MACOS
    auto& s = emergency_state();
    if (!s.active.load(std::memory_order_acquire)) return;
    // Write the escape restore (alt-screen leave, mouse/paste/focus off, show
    // cursor). ::write is async-signal-safe; short writes / EINTR are tolerable
    // — anything beats leaving the user's terminal in alt-screen + raw.
    if (s.fd >= 0 && s.seq_len > 0) {
        std::size_t off = 0;
        while (off < s.seq_len) {
            auto n = ::write(s.fd, s.seq.data() + off, s.seq_len - off);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n == 0) break;
            off += static_cast<std::size_t>(n);
        }
    }
    // Restore cooked termios so the shell isn't left without echo/line-editing.
    // tcsetattr isn't on the strict async-signal-safe list, but in practice it
    // works from a handler and a usable terminal is worth the pragmatism.
    if (s.have_termios && s.termios_fd >= 0) {
        while (::tcsetattr(s.termios_fd, TCSANOW, &s.cooked) < 0 && errno == EINTR) {}
    }
#endif
}

[[noreturn]] inline void emergency_terminate_handler() noexcept {
    emergency_emit();
    auto prior = emergency_state().prior_terminate;
    if (prior && prior != &emergency_terminate_handler) {
        prior();
    }
    std::abort();
}

inline void emergency_atexit() noexcept {
    emergency_emit();
}

#if MAYA_PLATFORM_POSIX || MAYA_PLATFORM_MACOS
// Fatal signals whose default action terminates the process WITHOUT running
// atexit handlers or C++ destructors — so without this, a `kill`, a closed
// terminal (SIGHUP), or a crash would strand the tty in alt-screen + raw +
// mouse-reporting.
//
// SIGINT is included but handled SAFELY by chaining: emergency_signal_handler
// restores the PRIOR disposition and re-raises, so a Python/host runtime that
// installed its own SIGINT handler (for KeyboardInterrupt) still receives it —
// we only interpose long enough to put the tty back first. In raw mode ^C is
// normally delivered as a byte (ISIG off) and never reaches here; this hook
// only fires when SIGINT arrives by another route (e.g. `kill -INT`, or before
// raw mode is fully established) and nothing else would have restored the tty.
// Without it, such a SIGINT to a maya app with no host handler strands the
// terminal in raw + alt-screen — the exact failure this machinery exists to
// prevent.
inline constexpr int kEmergencySignals[] = {
    SIGINT, SIGHUP, SIGTERM, SIGQUIT, SIGSEGV, SIGABRT, SIGBUS, SIGFPE,
};

inline void emergency_signal_handler(int sig) noexcept {
    emergency_emit();                       // restore tty (async-signal-safe-ish)
    auto& s = emergency_state();
    // Restore the prior disposition and re-raise, so a host's handler (crash
    // reporter) or the default action (correct exit status / core dump) runs.
    if (sig >= 0 && sig < static_cast<int>(std::size(s.prior_signal)))
        (void)::sigaction(sig, &s.prior_signal[sig], nullptr);
    else
        (void)std::signal(sig, SIG_DFL);
    (void)::raise(sig);
}
#endif

#if MAYA_PLATFORM_POSIX || MAYA_PLATFORM_MACOS
// Register the cooked termios to restore on an emergency exit. Called when raw
// mode is enabled (POSIX only — Windows console restore rides the escape seq).
inline void install_emergency_termios(int fd, const struct termios& cooked) noexcept {
    auto& s = emergency_state();
    s.cooked       = cooked;
    s.termios_fd   = fd;
    s.have_termios = true;
}
#endif

inline void install_emergency_restore(std::string_view seq) noexcept {
#if MAYA_PLATFORM_POSIX || MAYA_PLATFORM_MACOS
    auto& s = emergency_state();
    const std::size_t n = std::min(seq.size(), s.seq.size());
    if (n > 0) std::memcpy(s.seq.data(), seq.data(), n);
    s.seq_len = n;
    s.fd      = STDOUT_FILENO;
    // Hook terminate + atexit + fatal signals exactly once per process. We
    // chain to prior handlers (set_terminate returns the old one; sigaction
    // saves the old action) so a host crash reporter / signal handler still
    // fires after we've restored the terminal.
    static std::once_flag installed;
    std::call_once(installed, [&]{
        s.prior_terminate = std::set_terminate(&emergency_terminate_handler);
        (void)std::atexit(&emergency_atexit);
        struct sigaction sa{};
        sa.sa_handler = &emergency_signal_handler;
        ::sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_NODEFER;   // allow re-raise to reach the prior handler
        for (int sig : kEmergencySignals) {
            if (sig >= 0 && sig < static_cast<int>(std::size(s.prior_signal)))
                (void)::sigaction(sig, &sa, &s.prior_signal[sig]);
        }
    });
    s.active.store(true, std::memory_order_release);
#else
    (void)seq;
#endif
}

inline void clear_emergency_restore() noexcept {
    emergency_state().active.store(false, std::memory_order_release);
}

} // namespace detail

// ============================================================================
// RawGuard - Standalone RAII guard for raw mode
// ============================================================================
// For code that needs raw mode without the full Terminal type-state machinery.
// Saves terminal state on construction, restores on destruction.

class RawGuard : MoveOnly {
    std::optional<platform::NativeTerminal> backend_;
    bool active_ = false;

public:
    [[nodiscard]] static auto create() -> Result<RawGuard> {
        RawGuard guard;
        auto result = platform::NativeTerminal::open();
        if (!result) return err<RawGuard>(result.error());

        guard.backend_.emplace(std::move(*result));
        auto status = guard.backend_->enable_raw();
        if (!status) return err<RawGuard>(status.error());

        guard.active_ = true;
        return ok(std::move(guard));
    }

    RawGuard() = default;
    RawGuard(RawGuard&&) noexcept = default;
    RawGuard& operator=(RawGuard&&) noexcept = default;

    ~RawGuard() { restore(); }

    void restore() noexcept {
        if (active_) {
            (void)backend_->disable_raw();
            active_ = false;
        }
    }

    [[nodiscard]] bool is_active() const noexcept { return active_; }
};

// ============================================================================
// Terminal<State, Backend> - Type-state terminal
// ============================================================================
// State is one of: Cooked, Raw, AltScreen (defined in core/types.hpp).
//
// Backend satisfies platform::TerminalBackend — selected at compile time.
// Default is platform::NativeTerminal (POSIX termios or Win32 Console API).
//
// Transitions consume the source object and return a new object in the
// target state. This is enforced by C++23 explicit object parameters
// (deducing this) taking `self` by value.
//
// Construction: Terminal<Cooked>::create()
// Transitions:  Cooked -> Raw -> AltScreen
// Destruction:  restores everything in reverse order

namespace detail {
    struct TerminalPrivateTag {};
}

template <typename State = Cooked,
          platform::TerminalBackend Backend = platform::NativeTerminal>
class Terminal : MoveOnly {
    Backend backend_;
    bool moved_from_ = true;  // default-constructed = moved-from

    // Private constructor - only create() and state transitions may construct
    using PrivateTag = detail::TerminalPrivateTag;
    Terminal(PrivateTag, Backend&& b)
        : backend_(std::move(b)), moved_from_(false) {}

    // Allow other Terminal instantiations to access private members
    template <typename OtherState, platform::TerminalBackend OtherBackend>
    friend class Terminal;

public:
    Terminal() = default;

    // ========================================================================
    // Creation
    // ========================================================================

    /// The only way to obtain a terminal. Saves the original state.
    [[nodiscard]] static auto create() -> Result<Terminal<Cooked, Backend>>
        requires std::same_as<State, Cooked>
    {
        MAYA_TRY_DECL(auto backend, Backend::open());
        return ok(Terminal<Cooked, Backend>{PrivateTag{}, std::move(backend)});
    }

    // ========================================================================
    // Type-state transitions (consume self, return new state)
    // ========================================================================

    /// Cooked -> Raw: enables raw mode
    [[nodiscard]] auto enable_raw_mode(this Terminal<Cooked, Backend> self)
        -> Result<Terminal<Raw, Backend>>
    {
        MAYA_TRY_VOID(self.backend_.enable_raw());
        Terminal<Raw, Backend> result{PrivateTag{}, std::move(self.backend_)};
        self.moved_from_ = true;
        return ok(std::move(result));
    }

    /// Raw -> Inline: enables the per-feature opt-ins that inline-mode
    /// applications (Claude-Code-style chat UIs) need WITHOUT entering the
    /// alt screen — scrollback is preserved.  The destructor disables each
    /// feature in reverse order; an exception escaping `run<P>()` therefore
    /// restores the user's terminal exactly as well as a graceful exit.
    /// This is the linear-RAII counterpart to the loose `(void)write(...)`
    /// calls that used to live in `Runtime::create`.
    [[nodiscard]] auto enable_inline_mode(this Terminal<Raw, Backend> self)
        -> Result<Terminal<InlineMode, Backend>>
    {
        // Two keyboard-protocol opt-ins so we get key disambiguation on the
        // widest set of terminals: KKP (Kitty/Foot/WezTerm/Ghostty/iTerm
        // 3.5+/Konsole 22.04+/recent xterm) AND xterm modifyOtherKeys=2
        // (VTE/GNOME/Tilix/stock xterm). Terminals that recognise neither
        // silently ignore both. hide_cursor: after each frame the cursor
        // sits at the end of the last rendered row — visually distracting,
        // and the composer paints its own caret glyph.
        std::string seq;
        seq.reserve(64);
        seq += ansi::enable_bracketed_paste;
        seq += ansi::kkp_push;
        seq += ansi::modify_other_keys_on;
        seq += ansi::hide_cursor;
        auto status = self.backend_.write_all(seq);
        if (!status) {
            return err<Terminal<InlineMode, Backend>>(status.error());
        }
        // Arm the emergency restore: if anything between here and the
        // destructor calls std::terminate / std::exit, these bytes get
        // written via raw ::write so the user's tty isn't left raw.
        // Mirrors the destructor's reverse sequence below.
        {
            std::string restore;
            restore.reserve(96);
            restore += ansi::modify_other_keys_off;
            restore += ansi::kkp_pop;
            restore += ansi::disable_bracketed_paste;
            // Mouse reporting is turned on by the Runtime (not here) when an
            // app sets mouse=true, so the inline Terminal's RAII never sees it.
            // Disable it in the emergency restore too — otherwise a crash/kill
            // leaves the tty spewing mouse escapes. Harmless no-op if mouse was
            // never enabled.
            restore += ansi::disable_alt_scroll;
            restore += ansi::disable_mouse;
            restore += "\x1b[?7h";    // DECAWM on
            restore += ansi::show_cursor;
            restore += ansi::reset;
            restore += "\r\n";
            detail::install_emergency_restore(restore);
#if MAYA_PLATFORM_POSIX || MAYA_PLATFORM_MACOS
            if constexpr (requires { self.backend_.cooked_termios(); })
                detail::install_emergency_termios(self.backend_.input_handle(),
                                                  self.backend_.cooked_termios());
#endif
        }
        Terminal<InlineMode, Backend> result{PrivateTag{}, std::move(self.backend_)};
        self.moved_from_ = true;
        return ok(std::move(result));
    }

    /// Raw -> AltScreen: enters alt screen buffer + hide cursor + enable
    /// mouse tracking + enable focus events + enable bracketed paste
    [[nodiscard]] auto enter_alt_screen(this Terminal<Raw, Backend> self)
        -> Result<Terminal<AltScreen, Backend>>
    {
        // ANSI sequences — work on ALL modern terminals (POSIX + Windows VT)
        std::string seq;
        seq.reserve(128);
        seq += ansi::alt_screen_enter;
        seq += ansi::hide_cursor;
        seq += ansi::enable_mouse;
        seq += ansi::enable_alt_scroll;
        seq += ansi::enable_focus;
        seq += ansi::enable_bracketed_paste;
        seq += ansi::kkp_push;          // disambiguate Shift+Enter etc.
        seq += ansi::clear_screen();
        seq += ansi::home();

        auto status = self.backend_.write_all(seq);
        if (!status) {
            // Self (Raw) will be destroyed, restoring raw -> cooked
            return err<Terminal<AltScreen, Backend>>(status.error());
        }

        // Emergency restore for alt-screen: terminate / atexit will
        // pop back to the primary buffer, restore mouse/focus/paste,
        // and show the cursor before we lose control.
        {
            std::string restore;
            restore.reserve(128);
            restore += ansi::kkp_pop;
            restore += ansi::disable_bracketed_paste;
            restore += ansi::disable_focus;
            restore += ansi::disable_alt_scroll;
            restore += ansi::disable_mouse;
            restore += ansi::show_cursor;
            restore += ansi::alt_screen_leave;
            detail::install_emergency_restore(restore);
#if MAYA_PLATFORM_POSIX || MAYA_PLATFORM_MACOS
            if constexpr (requires { self.backend_.cooked_termios(); })
                detail::install_emergency_termios(self.backend_.input_handle(),
                                                  self.backend_.cooked_termios());
#endif
        }

        Terminal<AltScreen, Backend> result{PrivateTag{}, std::move(self.backend_)};
        self.moved_from_ = true;
        return ok(std::move(result));
    }

    // ========================================================================
    // Operations available in all non-Cooked states
    // ========================================================================

    /// Write raw bytes to the terminal.
    auto write(std::string_view data) -> Status
        requires (!std::same_as<State, Cooked>)
    {
        return backend_.write_all(data);
    }

    /// Query the terminal dimensions.
    [[nodiscard]] auto size() const -> Size
        requires (!std::same_as<State, Cooked>)
    {
        return backend_.size();
    }

    /// Get the underlying input handle (for polling, etc.)
    [[nodiscard]] auto input_handle() const noexcept -> platform::NativeHandle
        requires (!std::same_as<State, Cooked>)
    {
        return backend_.input_handle();
    }

    /// Get the underlying output handle.
    [[nodiscard]] auto output_handle() const noexcept -> platform::NativeHandle
        requires (!std::same_as<State, Cooked>)
    {
        return backend_.output_handle();
    }

    /// Read raw bytes from the terminal.
    [[nodiscard]] auto read_raw() -> Result<std::string>
        requires (!std::same_as<State, Cooked>)
    {
        return backend_.read_raw();
    }

    // ========================================================================
    // Destructor - RAII cleanup in reverse order of state transitions
    // ========================================================================

    ~Terminal() {
        if (moved_from_) return;

        if constexpr (std::same_as<State, AltScreen>) {
            // Reverse of enter_alt_screen
            std::string seq;
            seq.reserve(128);
            seq += ansi::kkp_pop;       // restore terminal kbd protocol stack
            seq += ansi::disable_bracketed_paste;
            seq += ansi::disable_focus;
            seq += ansi::disable_alt_scroll;
            seq += ansi::disable_mouse;
            seq += ansi::show_cursor;
            seq += ansi::alt_screen_leave;
            (void)backend_.write_all(seq);
            (void)backend_.disable_raw();
            // Disarm the emergency handler — graceful path already
            // emitted the restore, so a subsequent unrelated terminate
            // shouldn't re-emit stale alt-screen-leave bytes.
            detail::clear_emergency_restore();
        }
        else if constexpr (std::same_as<State, InlineMode>) {
            // Reverse of enable_inline_mode. Disabling both keyboard
            // protocols BEFORE relinquishing raw mode prevents a misbehaving
            // shell from seeing CSI-u or CSI 27;… sequences it can't decode
            // after we hand control back.  Trailing \r\n moves the user's
            // shell prompt onto a fresh line below the last rendered frame.
            //
            // \x1b[?7h restores DECAWM. compose_inline_frame leaves DECAWM
            // off across frames as a byte-saving optimisation (avoids the
            // 10-byte \x1b[?7l\x1b[?7h bracket per frame on slow ttys); the
            // shell expects auto-wrap on, so we restore here. Emitted
            // unconditionally — the sequence is idempotent and the destructor
            // path doesn't know the InlineFrameState's flag.
            std::string seq;
            seq.reserve(64);
            seq += ansi::modify_other_keys_off;
            seq += ansi::kkp_pop;
            seq += ansi::disable_bracketed_paste;
            seq += "\x1b[?7h";   // DECAWM on
            seq += ansi::show_cursor;
            seq += ansi::reset;
            seq += "\r\n";
            (void)backend_.write_all(seq);
            (void)backend_.disable_raw();
            detail::clear_emergency_restore();
        }
        else if constexpr (std::same_as<State, Raw>) {
            (void)backend_.write_all(ansi::show_cursor);
            (void)backend_.disable_raw();
        }
        // Cooked: nothing to restore
    }

    // ========================================================================
    // Move operations
    // ========================================================================

    Terminal(Terminal&& other) noexcept
        : backend_(std::move(other.backend_))
        , moved_from_(std::exchange(other.moved_from_, true))
    {}

    Terminal& operator=(Terminal&& other) noexcept {
        if (this != &other) {
            this->~Terminal();
            backend_    = std::move(other.backend_);
            moved_from_ = std::exchange(other.moved_from_, true);
        }
        return *this;
    }
};

} // namespace maya
