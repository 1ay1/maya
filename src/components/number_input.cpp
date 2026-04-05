#include "maya/components/number_input.hpp"

namespace maya::components {

void NumberInput::clamp() {
    if (value_ < props_.min) value_ = props_.min;
    if (value_ > props_.max) value_ = props_.max;
}

std::string NumberInput::format_value() const {
    return fmt(("%." + std::to_string(props_.precision) + "f").c_str(), value_);
}

bool NumberInput::update(const Event& ev) {
    if (!focused_) return false;

    // Increment: Up, Right, +, k
    if (key(ev, SpecialKey::Up) || key(ev, SpecialKey::Right) ||
        key(ev, '+') || key(ev, 'k')) {
        increment(); return true;
    }
    // Decrement: Down, Left, -, j
    if (key(ev, SpecialKey::Down) || key(ev, SpecialKey::Left) ||
        key(ev, '-') || key(ev, 'j')) {
        decrement(); return true;
    }
    // Home: jump to min
    if (key(ev, SpecialKey::Home)) {
        set_value(props_.min); return true;
    }
    // End: jump to max
    if (key(ev, SpecialKey::End)) {
        set_value(props_.max); return true;
    }
    // PageUp/PageDown: step * 10
    if (key(ev, SpecialKey::PageUp)) {
        set_value(value_ + props_.step * 10); return true;
    }
    if (key(ev, SpecialKey::PageDown)) {
        set_value(value_ - props_.step * 10); return true;
    }

    return false;
}

Element NumberInput::render() const {
    using namespace maya::dsl;

    auto& p = palette();
    auto val_str = format_value();

    // Arrow styles: primary when active, dim when at limit
    auto left_arrow  = text("◀", Style{}.with_fg(at_min() ? p.dim : p.primary));
    auto right_arrow = text("▶", Style{}.with_fg(at_max() ? p.dim : p.primary));

    // Value style: bold, colored by focus state
    Color val_color = focused_ ? p.primary : p.muted;
    auto val_text = text(" " + val_str + " ", Style{}.with_fg(val_color).with_bold());

    // Build inner content
    std::vector<Element> parts;
    if (!props_.label.empty())
        parts.push_back(text(props_.label + ": ", Style{}.with_fg(p.text)));
    parts.push_back(std::move(left_arrow));
    parts.push_back(std::move(val_text));
    parts.push_back(std::move(right_arrow));

    return hstack()
        .border(BorderStyle::Round)
        .padding(0, 1, 0, 1)(std::move(parts));
}

} // namespace maya::components
