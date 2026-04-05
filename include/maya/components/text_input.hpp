#pragma once
// maya::components::TextInput — Multi-line text editor with cursor
//
// Stateful component — create once, call update() per event, render() per frame.
//
//   TextInput input({.placeholder = "Type a message..."});
//
//   // In event handler:
//   input.update(ev);
//
//   // In render:
//   input.render()
//
// Supports: cursor movement, insert/delete, selection (shift+arrows),
// home/end, word jump (ctrl+left/right), multi-line, paste, scroll.

#include "core.hpp"

namespace maya::components {

struct TextInputProps {
    std::string placeholder   = "";
    int         max_height    = 8;     // max visible lines (0 = unlimited)
    int         min_height    = 1;
    bool        focused       = true;
    bool        readonly      = false;
    Color       cursor_color  = palette().primary;
    Color       text_color    = palette().text;
    Color       border_color  = palette().border;
    bool        show_border   = true;
    std::string initial_value = "";
};

class TextInput {
    std::string buf_;
    int cursor_  = 0;    // byte offset into buf_
    int scroll_  = 0;    // first visible line
    bool focused_ = true;
    TextInputProps props_;

    // Helpers
    int line_count() const;
    int line_start(int pos) const;
    int line_end(int pos) const;
    int current_line() const;
    int col_in_line() const;
    int pos_at_line_col(int target_line, int col) const;
    void ensure_cursor_visible();
    bool is_word_char(char c) const;
    int word_left(int pos) const;
    int word_right(int pos) const;

public:
    explicit TextInput(TextInputProps props = {})
        : buf_(std::move(props.initial_value))
        , cursor_(static_cast<int>(buf_.size()))
        , focused_(props.focused)
        , props_(std::move(props)) {}

    // ── State access ─────────────────────────────────────────────────────────

    [[nodiscard]] const std::string& value() const { return buf_; }
    void set_value(std::string v) { buf_ = std::move(v); cursor_ = static_cast<int>(buf_.size()); }
    void clear() { buf_.clear(); cursor_ = 0; scroll_ = 0; }
    [[nodiscard]] bool empty() const { return buf_.empty(); }
    void focus() { focused_ = true; }
    void blur() { focused_ = false; }
    [[nodiscard]] bool focused() const { return focused_; }

    // ── Event handling ───────────────────────────────────────────────────────

    bool update(const Event& ev);

    // ── Render ───────────────────────────────────────────────────────────────

    [[nodiscard]] Element render() const;
};

} // namespace maya::components
