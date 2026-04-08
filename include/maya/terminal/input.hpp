#pragma once
// maya::input - Input parser FSM
//
// Converts raw terminal bytes into structured Event values. The parser is
// a state machine that handles partial escape sequences, CSI parameters,
// SS3 function keys, SGR mouse reports, bracketed paste, and focus events.
//
// The ambiguous-escape problem: a bare ESC byte could be the start of a
// CSI sequence or the user pressing Escape. We resolve this with a 50ms
// timeout - if no follow-up byte arrives within that window, we emit
// an Escape key event.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "../core/types.hpp"

namespace maya {

// ============================================================================
// Key - all recognized key values
// ============================================================================

struct CharKey { char32_t codepoint; };

enum class SpecialKey : uint8_t {
    Up, Down, Left, Right,
    Home, End,
    PageUp, PageDown,
    Tab, BackTab,
    Backspace, Delete, Insert,
    Enter, Escape,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
};

using Key = std::variant<CharKey, SpecialKey>;

// ============================================================================
// Modifiers
// ============================================================================

struct Modifiers {
    bool ctrl  = false;
    bool alt   = false;
    bool shift = false;
    bool super_ = false;

    constexpr bool none() const noexcept {
        return !ctrl && !alt && !shift && !super_;
    }

    constexpr bool operator==(const Modifiers&) const = default;

    // Parse the ANSI modifier parameter (1-based bitmask, value = 1 + bitmask)
    [[nodiscard]] static constexpr Modifiers from_param(int param) noexcept {
        int bits = param - 1;
        return {
            .ctrl  = (bits & 0x04) != 0,
            .alt   = (bits & 0x02) != 0,
            .shift = (bits & 0x01) != 0,
            .super_ = (bits & 0x08) != 0,
        };
    }
};

// ============================================================================
// KeyEvent
// ============================================================================

struct KeyEvent {
    Key       key;
    Modifiers mods;
    std::string raw_sequence; // original bytes that produced this event
};

// ============================================================================
// Mouse types
// ============================================================================

enum class MouseButton : uint8_t {
    Left, Right, Middle,
    ScrollUp, ScrollDown,
    None,
};

enum class MouseEventKind : uint8_t {
    Press,
    Release,
    Move,
};

struct MouseEvent {
    MouseButton   button;
    MouseEventKind kind;
    Columns       x;
    Rows          y;
    Modifiers     mods;
};

// ============================================================================
// Other event types
// ============================================================================

struct PasteEvent {
    std::string content;
};

struct FocusEvent {
    bool focused;
};

struct ResizeEvent {
    Columns width;
    Rows    height;
};

// ============================================================================
// Event - the unified event variant
// ============================================================================

using Event = std::variant<KeyEvent, MouseEvent, PasteEvent, FocusEvent, ResizeEvent>;

// ============================================================================
// InputParser - stateful FSM for converting raw bytes to events
// ============================================================================
// Feed raw bytes via feed(). The parser may buffer partial sequences
// internally. Call flush_timeout() periodically (e.g. after a poll timeout)
// to resolve ambiguous escapes.

class InputParser {
public:
    InputParser() = default;

    /// Feed raw bytes and extract all complete events.
    [[nodiscard]] auto feed(std::string_view bytes) -> std::vector<Event>;

    /// Call after a poll/read timeout to resolve ambiguous escape.
    /// If we have a pending bare ESC that has timed out, emit it.
    [[nodiscard]] auto flush_timeout() -> std::vector<Event>;

    /// Check if the parser has a partial sequence buffered.
    [[nodiscard]] bool has_pending() const noexcept;

    /// Reset the parser to ground state, discarding any partial sequence.
    void reset() noexcept;

private:
    using clock = std::chrono::steady_clock;
    static constexpr auto escape_timeout_ = std::chrono::milliseconds(50);

    enum class State : uint8_t {
        Ground,
        Escape,         // saw ESC, waiting for second byte
        Csi,            // inside CSI sequence (ESC [)
        Ss3,            // inside SS3 sequence (ESC O)
        Osc,            // inside OSC sequence (ESC ])
        BracketedPaste, // inside bracketed paste content
        Utf8,           // buffering UTF-8 continuation bytes
    };

    State       state_ = State::Ground;
    std::string buf_;
    clock::time_point escape_start_;
    char32_t    utf8_cp_ = 0;      // accumulated codepoint
    uint8_t     utf8_remaining_ = 0; // continuation bytes left
    std::string utf8_raw_;         // raw bytes for the sequence

    // Character classification
    static constexpr bool is_csi_final(uint8_t ch) noexcept {
        return ch >= 0x40 && ch <= 0x7e;
    }

    // Ground state: plain bytes
    void ground_byte(uint8_t ch, std::vector<Event>& events);

    // Alt + key
    void emit_alt_key(uint8_t ch, std::vector<Event>& events);

    // CSI sequence parser
    void parse_csi(std::vector<Event>& events);

    // Tilde-terminated special keys (CSI number ~)
    void handle_tilde_key(int code, int modifier_param, std::vector<Event>& events);

    // SGR mouse report: CSI < Pb ; Px ; Py M/m
    void parse_sgr_mouse(std::string_view params_str, uint8_t final_byte,
                         std::vector<Event>& events);

    // SS3 function keys (ESC O P/Q/R/S)
    void parse_ss3(uint8_t ch, std::vector<Event>& events);

    // OSC parser (currently a no-op; can be extended for clipboard, etc.)
    void parse_osc(std::vector<Event>& events);

    // Parameter parsing utility
    [[nodiscard]] static auto parse_params(std::string_view s) -> std::vector<int>;
};

} // namespace maya
