#pragma once
// maya::components::Spinner — Animated loading indicator
//
//   Spinner({.frame = tick})
//   Spinner({.frame = tick, .label = "Loading..."})
//   Spinner({.frame = tick, .style = SpinnerStyle::Dots, .color = Color::cyan()})

#include "core.hpp"

namespace maya::components {

enum class SpinnerStyle { Braille, Dots, Line, Bounce };

struct SpinnerProps {
    int          frame = 0;
    std::string  label = "";
    SpinnerStyle style = SpinnerStyle::Braille;
    Color        color = palette().primary;
};

Element Spinner(SpinnerProps props = {});

} // namespace maya::components
