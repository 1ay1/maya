#include "maya/terminal/ansi.hpp"

#include <array>
#include <format>
#include <string>
#include <string_view>

#include "maya/style/color.hpp"
#include "maya/style/style.hpp"

namespace maya {
namespace ansi {

// ============================================================================
// Data-driven SGR attribute table
// ============================================================================
// Maps each boolean Style member to its SGR parameter code.
// Used by StyleApplier to avoid repetitive if-chains.

struct AttrEntry {
    bool Style::* flag;
    char          code;
};

static constexpr std::array<AttrEntry, 6> sgr_attrs{{
    {&Style::bold,          '1'},
    {&Style::dim,           '2'},
    {&Style::italic,        '3'},
    {&Style::underline,     '4'},
    {&Style::inverse,       '7'},
    {&Style::strikethrough, '9'},
}};

// ============================================================================
// Cursor control
// ============================================================================

[[nodiscard]] std::string move_up(int n) {
    if (n == 0) return {};
    return std::format("\x1b[{}A", n);
}

[[nodiscard]] std::string move_down(int n) {
    if (n == 0) return {};
    return std::format("\x1b[{}B", n);
}

[[nodiscard]] std::string move_right(int n) {
    if (n == 0) return {};
    return std::format("\x1b[{}C", n);
}

[[nodiscard]] std::string move_left(int n) {
    if (n == 0) return {};
    return std::format("\x1b[{}D", n);
}

[[nodiscard]] std::string move_to(int col, int row) {
    return std::format("\x1b[{};{}H", row, col);
}

[[nodiscard]] std::string move_to_col(int col) {
    return std::format("\x1b[{}G", col);
}

[[nodiscard]] std::string save_pos() {
    return "\x1b[s";
}

[[nodiscard]] std::string restore_pos() {
    return "\x1b[u";
}

[[nodiscard]] std::string home() {
    return "\x1b[H";
}

// ============================================================================
// Screen / line clearing
// ============================================================================

[[nodiscard]] std::string clear_screen() {
    return "\x1b[2J";
}

[[nodiscard]] std::string clear_line() {
    return "\x1b[2K";
}

[[nodiscard]] std::string clear_to_end() {
    return "\x1b[0K";
}

[[nodiscard]] std::string clear_to_start() {
    return "\x1b[1K";
}

void erase_lines(int n, std::string& out) {
    for (int i = 0; i < n; ++i) {
        out += "\x1b[2K";            // erase entire current line
        if (i < n - 1) out += "\x1b[A"; // cursor up (not on last iteration)
    }
    if (n > 0) out += "\x1b[G";     // cursor to column 1
}

// ============================================================================
// Color SGR sequences
// ============================================================================

[[nodiscard]] std::string fg(const Color& c) {
    return std::format("\x1b[{}m", c.fg_sgr());
}

[[nodiscard]] std::string bg(const Color& c) {
    return std::format("\x1b[{}m", c.bg_sgr());
}

// ============================================================================
// StyleApplier
// ============================================================================

std::string StyleApplier::apply(const Style& s) {
    std::string out;
    out.reserve(32);

    std::string params;

    for (const auto& [flag, code] : sgr_attrs) {
        if (s.*flag) {
            append_param(params, std::string_view(&code, 1));
        }
    }

    if (s.fg) append_param(params, s.fg->fg_sgr());
    if (s.bg) append_param(params, s.bg->bg_sgr());

    if (!params.empty()) {
        out += "\x1b[";
        out += params;
        out += 'm';
    }

    return out;
}

std::string StyleApplier::transition(const Style& prev, const Style& next) {
    if (prev == next) return {};

    // Check if any attribute was turned off — requires a full reset.
    bool needs_reset = false;
    for (const auto& [flag, code] : sgr_attrs) {
        if (prev.*flag && !(next.*flag)) {
            needs_reset = true;
            break;
        }
    }
    needs_reset = needs_reset
               || (prev.fg.has_value() && !next.fg.has_value())
               || (prev.bg.has_value() && !next.bg.has_value());

    if (needs_reset) {
        std::string out;
        out.reserve(48);
        out += "\x1b[0m";

        std::string params;
        for (const auto& [flag, code] : sgr_attrs) {
            if (next.*flag) {
                append_param(params, std::string_view(&code, 1));
            }
        }
        if (next.fg) append_param(params, next.fg->fg_sgr());
        if (next.bg) append_param(params, next.bg->bg_sgr());

        if (!params.empty()) {
            out += "\x1b[";
            out += params;
            out += 'm';
        }
        return out;
    }

    // Only additions/changes — emit incremental SGR
    std::string params;

    for (const auto& [flag, code] : sgr_attrs) {
        if (next.*flag && !(prev.*flag)) {
            append_param(params, std::string_view(&code, 1));
        }
    }

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

void StyleApplier::apply_to(const Style& s, std::string& out) {
    if (s.empty()) return;
    out += "\x1b[";
    bool first = true;
    auto sep = [&]() { if (!first) out += ';'; first = false; };

    for (const auto& [flag, code] : sgr_attrs) {
        if (s.*flag) { sep(); out += code; }
    }

    if (s.fg) { sep(); s.fg->append_fg_sgr(out); }
    if (s.bg) { sep(); s.bg->append_bg_sgr(out); }
    out += 'm';
}

void StyleApplier::transition_to(const Style& prev, const Style& next, std::string& out) {
    if (prev == next) return;

    bool needs_reset = false;
    for (const auto& [flag, code] : sgr_attrs) {
        if (prev.*flag && !(next.*flag)) {
            needs_reset = true;
            break;
        }
    }
    needs_reset = needs_reset
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

    for (const auto& [flag, code] : sgr_attrs) {
        if (next.*flag && !(prev.*flag)) { mark(); out += code; }
    }
    if (next.fg && next.fg != prev.fg) { mark(); next.fg->append_fg_sgr(out); }
    if (next.bg && next.bg != prev.bg) { mark(); next.bg->append_bg_sgr(out); }

    if (any) out += 'm';
}

void StyleApplier::append_param(std::string& params, std::string_view p) {
    if (!params.empty()) params += ';';
    params += p;
}

// ============================================================================
// OSC sequences
// ============================================================================

[[nodiscard]] std::string set_title(std::string_view title) {
    return std::format("\x1b]2;{}\x07", title);
}

[[nodiscard]] std::string hyperlink_start(std::string_view uri) {
    return std::format("\x1b]8;;{}\x07", uri);
}

[[nodiscard]] std::string hyperlink_end() {
    return "\x1b]8;;\x07";
}

[[nodiscard]] std::string set_clipboard(std::string_view base64_data) {
    return std::format("\x1b]52;c;{}\x07", base64_data);
}

} // namespace ansi
} // namespace maya
