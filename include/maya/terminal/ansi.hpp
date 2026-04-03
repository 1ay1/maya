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
[[gnu::always_inline]] inline void append_int(std::string& out, int n) {
    char buf[12];
    auto [end, _] = std::to_chars(buf, buf + sizeof(buf), n);
    out.append(buf, end);
}

} // namespace detail

/// Zero-alloc ANSI CUP cursor positioning: ESC [ row ; col H
/// col and row are 1-based as per ANSI convention.
[[gnu::always_inline]] inline void write_move_to(std::string& out, int col, int row) {
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

[[nodiscard]] inline std::string move_up(int n = 1) {
    if (n == 0) return {};
    return "\x1b[" + std::to_string(n) + "A";
}

[[nodiscard]] inline std::string move_down(int n = 1) {
    if (n == 0) return {};
    return "\x1b[" + std::to_string(n) + "B";
}

[[nodiscard]] inline std::string move_right(int n = 1) {
    if (n == 0) return {};
    return "\x1b[" + std::to_string(n) + "C";
}

[[nodiscard]] inline std::string move_left(int n = 1) {
    if (n == 0) return {};
    return "\x1b[" + std::to_string(n) + "D";
}

// move_to: 1-based (col, row) as per ANSI CUP convention
[[nodiscard]] inline std::string move_to(int col, int row) {
    return "\x1b[" + std::to_string(row) + ";" + std::to_string(col) + "H";
}

[[nodiscard]] inline std::string move_to_col(int col) {
    return "\x1b[" + std::to_string(col) + "G";
}

[[nodiscard]] inline std::string save_pos() {
    return "\x1b[s";
}

[[nodiscard]] inline std::string restore_pos() {
    return "\x1b[u";
}

[[nodiscard]] inline std::string home() {
    return "\x1b[H";
}

// ============================================================================
// Screen / line clearing
// ============================================================================

// 0 = cursor to end, 1 = cursor to start, 2 = entire screen
[[nodiscard]] inline std::string clear_screen() {
    return "\x1b[2J";
}

[[nodiscard]] inline std::string clear_line() {
    return "\x1b[2K";
}

[[nodiscard]] inline std::string clear_to_end() {
    return "\x1b[0K";
}

[[nodiscard]] inline std::string clear_to_start() {
    return "\x1b[1K";
}

/// Erase `n` lines upward from the current cursor row (inclusive), then move
/// the cursor to column 1 of the top erased line. Matches Ink's eraseLines().
inline void erase_lines(int n, std::string& out) {
    for (int i = 0; i < n; ++i) {
        out += "\x1b[2K";            // erase entire current line
        if (i < n - 1) out += "\x1b[A"; // cursor up (not on last iteration)
    }
    if (n > 0) out += "\x1b[G";     // cursor to column 1
}

// ============================================================================
// Color SGR sequences
// ============================================================================

// Foreground color sequence (without CSI/m wrapper - just the SGR parameter)
[[nodiscard]] inline std::string fg(const Color& c) {
    return "\x1b[" + c.fg_sgr() + "m";
}

// Background color sequence
[[nodiscard]] inline std::string bg(const Color& c) {
    return "\x1b[" + c.bg_sgr() + "m";
}

// ============================================================================
// StyleApplier - diff-based SGR transition
// ============================================================================
// Generates the minimal escape sequence to transition from one Style to
// another. This is critical for rendering performance: instead of resetting
// and re-applying every attribute on every cell, we only emit what changed.

class StyleApplier {
public:
    // Apply a style from scratch (previous style unknown / reset state)
    [[nodiscard]] static std::string apply(const Style& s) {
        std::string out;
        out.reserve(32);

        // Collect SGR parameters into a single sequence to avoid
        // emitting multiple CSI..m runs.
        std::string params;

        if (s.bold)          append_param(params, "1");
        if (s.dim)           append_param(params, "2");
        if (s.italic)        append_param(params, "3");
        if (s.underline)     append_param(params, "4");
        if (s.inverse)       append_param(params, "7");
        if (s.strikethrough) append_param(params, "9");

        if (s.fg) append_param(params, s.fg->fg_sgr());
        if (s.bg) append_param(params, s.bg->bg_sgr());

        if (!params.empty()) {
            out += "\x1b[";
            out += params;
            out += 'm';
        }

        return out;
    }

    // Transition from `prev` to `next`, emitting only what changed.
    // Returns empty string if the styles are identical.
    [[nodiscard]] static std::string transition(const Style& prev, const Style& next) {
        if (prev == next) return {};

        // If any attribute was turned off that was previously on, we must
        // reset and re-apply. SGR doesn't have individual "off" codes for
        // bold vs dim (both are code 22), so a full reset is the safe path
        // when attributes are removed.
        bool needs_reset = (prev.bold          && !next.bold)
                        || (prev.dim           && !next.dim)
                        || (prev.italic        && !next.italic)
                        || (prev.underline     && !next.underline)
                        || (prev.inverse       && !next.inverse)
                        || (prev.strikethrough && !next.strikethrough)
                        || (prev.fg.has_value() && !next.fg.has_value())
                        || (prev.bg.has_value() && !next.bg.has_value());

        if (needs_reset) {
            // Reset and re-apply from scratch
            std::string out;
            out.reserve(48);
            out += "\x1b[0m";

            std::string params;
            if (next.bold)          append_param(params, "1");
            if (next.dim)           append_param(params, "2");
            if (next.italic)        append_param(params, "3");
            if (next.underline)     append_param(params, "4");
            if (next.inverse)       append_param(params, "7");
            if (next.strikethrough) append_param(params, "9");
            if (next.fg) append_param(params, next.fg->fg_sgr());
            if (next.bg) append_param(params, next.bg->bg_sgr());

            if (!params.empty()) {
                out += "\x1b[";
                out += params;
                out += 'm';
            }
            return out;
        }

        // Only additions/changes - emit incremental SGR
        std::string params;

        if (next.bold          && !prev.bold)          append_param(params, "1");
        if (next.dim           && !prev.dim)           append_param(params, "2");
        if (next.italic        && !prev.italic)        append_param(params, "3");
        if (next.underline     && !prev.underline)     append_param(params, "4");
        if (next.inverse       && !prev.inverse)       append_param(params, "7");
        if (next.strikethrough && !prev.strikethrough)  append_param(params, "9");

        if (next.fg != prev.fg && next.fg) {
            append_param(params, next.fg->fg_sgr());
        }
        if (next.bg != prev.bg && next.bg) {
            append_param(params, next.bg->bg_sgr());
        }

        if (params.empty()) return {};

        std::string out;
        out.reserve(params.size() + 4);
        out += "\x1b[";
        out += params;
        out += 'm';
        return out;
    }

    // -------------------------------------------------------------------------
    // Zero-allocation variants — write directly into an existing string.
    // Hot path: called once per style change in the diff loop.
    // -------------------------------------------------------------------------

    /// Apply a complete style directly to `out` (no intermediate allocations).
    static void apply_to(const Style& s, std::string& out) {
        if (s.empty()) return;
        out += "\x1b[";
        bool first = true;
        auto sep = [&]() { if (!first) out += ';'; first = false; };
        if (s.bold)          { sep(); out += '1'; }
        if (s.dim)           { sep(); out += '2'; }
        if (s.italic)        { sep(); out += '3'; }
        if (s.underline)     { sep(); out += '4'; }
        if (s.inverse)       { sep(); out += '7'; }
        if (s.strikethrough) { sep(); out += '9'; }
        if (s.fg) { sep(); s.fg->append_fg_sgr(out); }
        if (s.bg) { sep(); s.bg->append_bg_sgr(out); }
        out += 'm';
    }

    /// Transition from `prev` to `next`, writing directly to `out`.
    /// Emits nothing when styles are identical.
    static void transition_to(const Style& prev, const Style& next, std::string& out) {
        if (prev == next) return;

        bool needs_reset = (prev.bold          && !next.bold)
                        || (prev.dim           && !next.dim)
                        || (prev.italic        && !next.italic)
                        || (prev.underline     && !next.underline)
                        || (prev.inverse       && !next.inverse)
                        || (prev.strikethrough && !next.strikethrough)
                        || (prev.fg.has_value() && !next.fg.has_value())
                        || (prev.bg.has_value() && !next.bg.has_value());

        if (needs_reset) {
            out += "\x1b[0m";
            apply_to(next, out);
            return;
        }

        // Only additions — emit incremental SGR.
        bool any = false;
        auto mark = [&]() { if (!any) { out += "\x1b["; any = true; } else out += ';'; };

        if (next.bold          && !prev.bold)          { mark(); out += '1'; }
        if (next.dim           && !prev.dim)           { mark(); out += '2'; }
        if (next.italic        && !prev.italic)        { mark(); out += '3'; }
        if (next.underline     && !prev.underline)     { mark(); out += '4'; }
        if (next.inverse       && !prev.inverse)       { mark(); out += '7'; }
        if (next.strikethrough && !prev.strikethrough) { mark(); out += '9'; }
        if (next.fg && next.fg != prev.fg) { mark(); next.fg->append_fg_sgr(out); }
        if (next.bg && next.bg != prev.bg) { mark(); next.bg->append_bg_sgr(out); }

        if (any) out += 'm';
    }

private:
    static void append_param(std::string& params, std::string_view p) {
        if (!params.empty()) params += ';';
        params += p;
    }
};

// ============================================================================
// OSC sequences
// ============================================================================

[[nodiscard]] inline std::string set_title(std::string_view title) {
    std::string out;
    out.reserve(title.size() + 6);
    out += "\x1b]2;";
    out += title;
    out += '\x07';
    return out;
}

[[nodiscard]] inline std::string hyperlink_start(std::string_view uri) {
    std::string out;
    out.reserve(uri.size() + 8);
    out += "\x1b]8;;";
    out += uri;
    out += '\x07';
    return out;
}

[[nodiscard]] inline std::string hyperlink_end() {
    return "\x1b]8;;\x07";
}

[[nodiscard]] inline std::string set_clipboard(std::string_view base64_data) {
    std::string out;
    out.reserve(base64_data.size() + 12);
    out += "\x1b]52;c;";
    out += base64_data;
    out += '\x07';
    return out;
}

} // namespace ansi
} // namespace maya
