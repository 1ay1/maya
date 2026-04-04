#pragma once
// maya::components::NumberInput — Numeric stepper with keyboard control
//
// Stateful component — create once, call update() per event, render() per frame.
//
//   NumberInput input({.value = 50, .min = 0, .max = 100, .label = "Volume"});
//
//   // In event handler:
//   input.update(ev);
//
//   // In render:
//   input.render()   // "Volume: ◀ 50 ▶"

#include "core.hpp"

namespace maya::components {

struct NumberInputProps {
    double      value     = 0.0;
    double      min       = 0.0;
    double      max       = 100.0;
    double      step      = 1.0;
    int         precision = 0;
    std::string label     = "";
    bool        focused   = true;
};

class NumberInput {
    double value_;
    NumberInputProps props_;
    bool focused_;

    void clamp() {
        if (value_ < props_.min) value_ = props_.min;
        if (value_ > props_.max) value_ = props_.max;
    }

    [[nodiscard]] bool at_min() const { return value_ <= props_.min; }
    [[nodiscard]] bool at_max() const { return value_ >= props_.max; }

    [[nodiscard]] std::string format_value() const {
        return fmt(("%." + std::to_string(props_.precision) + "f").c_str(), value_);
    }

public:
    explicit NumberInput(NumberInputProps props = {})
        : value_(props.value), props_(std::move(props)), focused_(props_.focused) {
        clamp();
    }

    [[nodiscard]] double value() const { return value_; }

    void set_value(double v) { value_ = v; clamp(); }

    void increment() { set_value(value_ + props_.step); }
    void decrement() { set_value(value_ - props_.step); }

    void focus() { focused_ = true; }
    void blur()  { focused_ = false; }
    [[nodiscard]] bool focused() const { return focused_; }

    bool update(const Event& ev) {
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

    [[nodiscard]] Element render() const {
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
};

} // namespace maya::components
