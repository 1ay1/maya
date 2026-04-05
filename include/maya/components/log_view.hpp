#pragma once
// maya::components::LogView — Scrollable log/terminal output viewer
//
// A stateful component that displays log lines with ANSI color code support.
// Parses SGR escape sequences and renders them as styled text elements.
//
//   LogView log({.max_visible = 30, .tail_follow = true});
//
//   log.append("[\033[32mOK\033[0m] Server started on port 8080");
//   log.append_lines(captured_output);
//
//   // In event handler:
//   log.update(ev);
//
//   // In render:
//   auto ui = log.render();

#include "core.hpp"

#include <charconv>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace maya::components {

// ── ANSI SGR parser ─────────────────────────────────────────────────────────
// Parses a single line containing ANSI escape sequences and returns an hstack
// of styled text segments. Covers the most common SGR codes; non-SGR escape
// sequences are silently discarded.

Element parse_ansi_line(std::string_view line);

// ── LogView props ───────────────────────────────────────────────────────────

struct LogViewProps {
    int  max_visible    = 20;
    int  max_lines      = 10000;
    bool tail_follow    = true;
    bool show_scrollbar = true;
    bool show_line_nums = false;
};

// ── LogView ─────────────────────────────────────────────────────────────────

class LogView {
    std::deque<std::string> lines_;
    int  offset_    = 0;
    bool following_ = true;
    LogViewProps props_;

    void clamp();

public:
    explicit LogView(LogViewProps props = {})
        : following_(props.tail_follow), props_(std::move(props)) {}

    // ── Mutation ────────────────────────────────────────────────────────────

    void append(std::string line);
    void append_lines(std::vector<std::string> lines);
    void clear();

    // ── Queries ─────────────────────────────────────────────────────────────

    [[nodiscard]] int line_count() const { return static_cast<int>(lines_.size()); }

    [[nodiscard]] bool at_bottom() const;

    // ── Event handling ──────────────────────────────────────────────────────

    bool update(const Event& ev);

    // ── Rendering ───────────────────────────────────────────────────────────

    [[nodiscard]] Element render() const;
};

} // namespace maya::components
