#pragma once
// maya::components::StreamingText — Text that reveals character by character
//
// Stateful component — caller drives reveal speed via advance().
//
//   StreamingText st({.text = "Hello, world!", .show_cursor = true});
//
//   // On a timer:
//   st.advance();
//
//   // In render:
//   st.render()

#include "core.hpp"

namespace maya::components {

struct StreamingTextProps {
    std::string text        = "";
    bool        show_cursor = true;
    std::string cursor_char = "█";
};

class StreamingText {
    std::string text_;
    int         reveal_pos_ = 0;
    bool        show_cursor_;
    std::string cursor_char_;

    static bool is_continuation(char b) {
        return (static_cast<unsigned char>(b) & 0xC0) == 0x80;
    }

public:
    explicit StreamingText(StreamingTextProps props = {})
        : text_(std::move(props.text))
        , show_cursor_(props.show_cursor)
        , cursor_char_(std::move(props.cursor_char)) {}

    void set_text(std::string t) { text_ = std::move(t); reveal_pos_ = 0; }

    void advance(int chars = 1);

    void reveal_all() { reveal_pos_ = static_cast<int>(text_.size()); }

    [[nodiscard]] bool fully_revealed() const {
        return reveal_pos_ >= static_cast<int>(text_.size());
    }

    [[nodiscard]] int reveal_pos() const { return reveal_pos_; }

    /// Return the currently-revealed substring.
    [[nodiscard]] std::string revealed_text() const;

    void set_reveal_pos(int pos);

    [[nodiscard]] Element render() const;
};

} // namespace maya::components
