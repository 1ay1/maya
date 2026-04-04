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

    void advance(int chars = 1) {
        int n = static_cast<int>(text_.size());
        for (int i = 0; i < chars && reveal_pos_ < n; ++i) {
            ++reveal_pos_;
            while (reveal_pos_ < n && is_continuation(text_[reveal_pos_]))
                ++reveal_pos_;
        }
    }

    void reveal_all() { reveal_pos_ = static_cast<int>(text_.size()); }

    [[nodiscard]] bool fully_revealed() const {
        return reveal_pos_ >= static_cast<int>(text_.size());
    }

    [[nodiscard]] int reveal_pos() const { return reveal_pos_; }

    /// Return the currently-revealed substring.
    [[nodiscard]] std::string revealed_text() const {
        return text_.substr(0, reveal_pos_);
    }

    void set_reveal_pos(int pos) {
        int n = static_cast<int>(text_.size());
        reveal_pos_ = std::clamp(pos, 0, n);
        // Snap forward past any continuation bytes
        while (reveal_pos_ < n && is_continuation(text_[reveal_pos_]))
            ++reveal_pos_;
    }

    [[nodiscard]] Element render() const {
        using namespace maya::dsl;

        std::string revealed = text_.substr(0, reveal_pos_);

        // Split by newlines
        std::vector<Element> rows;
        std::string_view sv = revealed;
        while (true) {
            auto pos = sv.find('\n');
            if (pos == std::string_view::npos) {
                rows.push_back(text(std::string(sv)));
                break;
            }
            rows.push_back(text(std::string(sv.substr(0, pos))));
            sv = sv.substr(pos + 1);
        }

        // Append cursor to last line
        if (!fully_revealed() && show_cursor_) {
            auto cursor = text(cursor_char_,
                               Style{}.with_fg(palette().primary).with_inverse());
            auto& last = rows.back();
            last = hstack()(std::move(last), std::move(cursor));
        }

        return vstack()(std::move(rows));
    }
};

} // namespace maya::components
