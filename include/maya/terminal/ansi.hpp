#pragma once
// maya::ansi - Compile-time ANSI escape sequence builder
//
// Generates and validates ANSI escape codes at compile time where possible,
// with runtime std::string builders for parameterized sequences. This is
// the lowest layer of terminal output - everything above talks in Colors
// and Styles; this layer talks in bytes on the wire.

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../platform/detect.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {
namespace ansi {

// ============================================================================
// Zero-allocation helpers (write directly into an existing std::string)
// ============================================================================
// These are the speed-critical paths. Every allocation eliminated here
// reduces pressure in the diff loop that runs for every changed cell.

namespace detail {

/// Append a decimal integer into `out` using std::to_chars (no allocation).
MAYA_FORCEINLINE void append_int(std::string& out, int n) {
    char buf[12];
    auto [end, _] = std::to_chars(buf, buf + sizeof(buf), n);
    out.append(buf, end);
}

} // namespace detail

/// Zero-alloc cursor-up: ESC [ n A — avoids std::format overhead.
MAYA_FORCEINLINE void write_cursor_up(std::string& out, int n) {
    if (n <= 0) return;
    out += "\x1b[";
    detail::append_int(out, n);
    out += 'A';
}

/// Ink-style eraseLines: erase N lines from bottom to top.
/// For each line: \x1b[2K (erase entire line) + \x1b[1A (cursor up).
/// Finishes with \x1b[G (cursor to column 0).
/// After this call, cursor is at column 0 of the line (N-1) rows above
/// where the cursor started, and all N lines have been erased.
MAYA_FORCEINLINE void write_erase_lines(std::string& out, int n) {
    for (int i = 0; i < n; ++i) {
        out += "\x1b[2K";                    // erase entire line
        if (i < n - 1) out += "\x1b[1A";     // cursor up (not after last)
    }
    if (n > 0) out += "\x1b[G";              // cursor to column 0
}

/// Zero-alloc cursor-down: ESC [ n B
MAYA_FORCEINLINE void write_cursor_down(std::string& out, int n) {
    if (n <= 0) return;
    out += "\x1b[";
    detail::append_int(out, n);
    out += 'B';
}

/// Zero-alloc ANSI CUP cursor positioning: ESC [ row ; col H
/// col and row are 1-based as per ANSI convention.
MAYA_FORCEINLINE void write_move_to(std::string& out, int col, int row) {
    out += "\x1b[";
    detail::append_int(out, row);
    out += ';';
    detail::append_int(out, col);
    out += 'H';
}

// ============================================================================
// Constants - CSI / OSC / ST prefixes
// ============================================================================

inline constexpr std::string_view csi = "\x1b[";
inline constexpr std::string_view osc = "\x1b]";
inline constexpr std::string_view st  = "\x07";

// ============================================================================
// Mode control - constexpr string_view constants
// ============================================================================

inline constexpr std::string_view show_cursor              = "\x1b[?25h";
inline constexpr std::string_view hide_cursor              = "\x1b[?25l";
inline constexpr std::string_view alt_screen_enter         = "\x1b[?1049h";
inline constexpr std::string_view alt_screen_leave         = "\x1b[?1049l";
inline constexpr std::string_view enable_mouse             = "\x1b[?1000h\x1b[?1006h";
inline constexpr std::string_view disable_mouse            = "\x1b[?1006l\x1b[?1000l";
inline constexpr std::string_view enable_alt_scroll        = "\x1b[?1007h";
inline constexpr std::string_view disable_alt_scroll       = "\x1b[?1007l";
inline constexpr std::string_view enable_focus             = "\x1b[?1004h";
inline constexpr std::string_view disable_focus            = "\x1b[?1004l";
inline constexpr std::string_view enable_bracketed_paste   = "\x1b[?2004h";
inline constexpr std::string_view disable_bracketed_paste  = "\x1b[?2004l";
inline constexpr std::string_view sync_start               = "\x1b[?2026h";
inline constexpr std::string_view sync_end                 = "\x1b[?2026l";

// ============================================================================
// SGR attribute constants
// ============================================================================

inline constexpr std::string_view reset         = "\x1b[0m";
inline constexpr std::string_view bold          = "\x1b[1m";
inline constexpr std::string_view dim           = "\x1b[2m";
inline constexpr std::string_view italic        = "\x1b[3m";
inline constexpr std::string_view underline     = "\x1b[4m";
inline constexpr std::string_view inverse       = "\x1b[7m";
inline constexpr std::string_view strikethrough = "\x1b[9m";
inline constexpr std::string_view bold_off          = "\x1b[22m";
inline constexpr std::string_view dim_off           = "\x1b[22m";
inline constexpr std::string_view italic_off        = "\x1b[23m";
inline constexpr std::string_view underline_off     = "\x1b[24m";
inline constexpr std::string_view inverse_off       = "\x1b[27m";
inline constexpr std::string_view strikethrough_off = "\x1b[29m";
inline constexpr std::string_view fg_reset          = "\x1b[39m";
inline constexpr std::string_view bg_reset          = "\x1b[49m";

// ============================================================================
// Cursor control
// ============================================================================

[[nodiscard]] std::string move_up(int n = 1);
[[nodiscard]] std::string move_down(int n = 1);
[[nodiscard]] std::string move_right(int n = 1);
[[nodiscard]] std::string move_left(int n = 1);

// move_to: 1-based (col, row) as per ANSI CUP convention
[[nodiscard]] std::string move_to(int col, int row);
[[nodiscard]] std::string move_to_col(int col);

[[nodiscard]] std::string save_pos();
[[nodiscard]] std::string restore_pos();
[[nodiscard]] std::string home();

// ============================================================================
// Screen / line clearing
// ============================================================================

// 0 = cursor to end, 1 = cursor to start, 2 = entire screen
[[nodiscard]] std::string clear_screen();
[[nodiscard]] std::string clear_line();
[[nodiscard]] std::string clear_to_end();
[[nodiscard]] std::string clear_to_start();

/// Erase `n` lines upward from the current cursor row (inclusive), then move
/// the cursor to column 1 of the top erased line. Matches Ink's eraseLines().
void erase_lines(int n, std::string& out);

// ============================================================================
// Color SGR sequences
// ============================================================================

// Foreground color sequence (without CSI/m wrapper - just the SGR parameter)
[[nodiscard]] std::string fg(const Color& c);

// Background color sequence
[[nodiscard]] std::string bg(const Color& c);

// ============================================================================
// StyleApplier - diff-based SGR transition
// ============================================================================
// Generates the minimal escape sequence to transition from one Style to
// another. This is critical for rendering performance: instead of resetting
// and re-applying every attribute on every cell, we only emit what changed.

class StyleApplier {
public:
    // Apply a style from scratch (previous style unknown / reset state)
    [[nodiscard]] static std::string apply(const Style& s);

    // Transition from `prev` to `next`, emitting only what changed.
    // Returns empty string if the styles are identical.
    [[nodiscard]] static std::string transition(const Style& prev, const Style& next);

    // -------------------------------------------------------------------------
    // Zero-allocation variants --- write directly into an existing string.
    // Hot path: called once per style change in the diff loop.
    // -------------------------------------------------------------------------

    /// Apply a complete style directly to `out` (no intermediate allocations).
    static void apply_to(const Style& s, std::string& out);

    /// Transition from `prev` to `next`, writing directly to `out`.
    /// Emits nothing when styles are identical.
    static void transition_to(const Style& prev, const Style& next, std::string& out);

private:
    static void append_param(std::string& params, std::string_view p);
};

// ============================================================================
// OSC sequences
// ============================================================================

[[nodiscard]] std::string set_title(std::string_view title);
[[nodiscard]] std::string hyperlink_start(std::string_view uri);
[[nodiscard]] std::string hyperlink_end();
[[nodiscard]] std::string set_clipboard(std::string_view base64_data);

} // namespace ansi
} // namespace maya
