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

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "../core/expected.hpp"
#include "../core/types.hpp"
#include "../platform/select.hpp"
#include "ansi.hpp"

namespace maya {

// Forward declarations - input parsing lives in input.hpp
class InputParser;

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
