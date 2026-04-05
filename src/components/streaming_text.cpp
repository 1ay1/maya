#include "maya/components/streaming_text.hpp"

namespace maya::components {

void StreamingText::advance(int chars) {
    int n = static_cast<int>(text_.size());
    for (int i = 0; i < chars && reveal_pos_ < n; ++i) {
        ++reveal_pos_;
        while (reveal_pos_ < n && is_continuation(text_[reveal_pos_]))
            ++reveal_pos_;
    }
}

std::string StreamingText::revealed_text() const {
    return text_.substr(0, reveal_pos_);
}

void StreamingText::set_reveal_pos(int pos) {
    int n = static_cast<int>(text_.size());
    reveal_pos_ = std::clamp(pos, 0, n);
    // Snap forward past any continuation bytes
    while (reveal_pos_ < n && is_continuation(text_[reveal_pos_]))
        ++reveal_pos_;
}

Element StreamingText::render() const {
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

} // namespace maya::components
