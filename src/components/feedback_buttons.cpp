#include "maya/components/feedback_buttons.hpp"

namespace maya::components {

void FeedbackButtons::update(const Event& ev) {
    if (editing_feedback_) return;  // ignore while editing text
    if (key(ev, 'y') || key(ev, '+')) {
        rating_ = Rating::ThumbsUp;
        return;
    }
    if (key(ev, 'n') || key(ev, '-')) {
        rating_ = Rating::ThumbsDown;
        return;
    }
}

Element FeedbackButtons::render() const {
    using namespace maya::dsl;
    auto& p = palette();

    bool up   = rating_ == Rating::ThumbsUp;
    bool down = rating_ == Rating::ThumbsDown;
    bool none = rating_ == Rating::None;

    Color up_col   = up   ? props_.up_color   : p.dim;
    Color down_col = down ? props_.down_color  : p.dim;

    Style up_style   = Style{}.with_fg(up_col).with_bold();
    Style down_style = Style{}.with_fg(down_col).with_bold();

    std::vector<Element> items;
    items.push_back(text(up ? "👍" : "👍", up_style));
    items.push_back(text("  ", Style{}));
    items.push_back(text(down ? "👎" : "👎", down_style));

    if (none) {
        items.push_back(text("  ", Style{}));
        items.push_back(text("y/n to rate", Style{}.with_fg(p.dim)));
    } else if (up) {
        items.push_back(text("  ", Style{}));
        items.push_back(text("Thanks!", Style{}.with_fg(p.success)));
    }

    return hstack().gap(0)(std::move(items));
}

} // namespace maya::components
