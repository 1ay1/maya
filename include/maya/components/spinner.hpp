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

inline Element Spinner(SpinnerProps props = {}) {
    using namespace maya::dsl;

    const char* const* frames;
    int n;
    switch (props.style) {
        case SpinnerStyle::Braille: frames = spinners::braille; n = spinners::braille_n; break;
        case SpinnerStyle::Dots:    frames = spinners::dots;    n = spinners::dots_n;    break;
        case SpinnerStyle::Line:    frames = spinners::line;    n = spinners::line_n;    break;
        case SpinnerStyle::Bounce:  frames = spinners::bounce;  n = spinners::bounce_n;  break;
    }

    auto icon = text(spin(props.frame, frames, n),
                     Style{}.with_bold().with_fg(props.color));

    if (props.label.empty()) {
        return icon;
    }

    return hstack().gap(1)(
        icon,
        text(props.label, Style{}.with_fg(palette().text))
    );
}

} // namespace maya::components
