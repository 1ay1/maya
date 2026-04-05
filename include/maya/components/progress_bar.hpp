#pragma once
// maya::components::ProgressBar — Linear progress with optional segments
//
//   ProgressBar({.value = 0.75f})
//   ProgressBar({.value = 0.5f, .width = 30, .show_percent = true})
//   ProgressBar({.segments = {{0.5, success}, {0.3, warning}, {0.2, error}}})

#include "core.hpp"

namespace maya::components {

struct Segment {
    float fraction;
    Color color;
};

struct ProgressBarProps {
    float value       = 0.f;     // 0.0 - 1.0 for simple mode
    int   width       = 20;
    bool  show_percent = false;
    Color filled      = palette().primary;
    Color empty       = palette().dim;
    std::vector<Segment> segments = {};  // multi-segment mode
};

Element ProgressBar(ProgressBarProps props = {});

} // namespace maya::components
