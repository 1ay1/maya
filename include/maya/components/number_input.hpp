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

    void clamp();

    [[nodiscard]] bool at_min() const { return value_ <= props_.min; }
    [[nodiscard]] bool at_max() const { return value_ >= props_.max; }

    [[nodiscard]] std::string format_value() const;

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

    bool update(const Event& ev);

    [[nodiscard]] Element render() const;
};

} // namespace maya::components
