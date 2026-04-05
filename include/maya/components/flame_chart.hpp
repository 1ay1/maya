#pragma once
// maya::components::FlameChart — Nested execution spans as a flame graph
//
//   FlameChart({.spans = {{"total", 0, 10.2, 0},
//                         {"thinking", 0, 4.2, 1},
//                         {"responding", 6.4, 3.8, 1},
//                         {"tool calls", 1.5, 3.5, 2},
//                         {"tests", 5.2, 1.2, 2}}})

#include "core.hpp"

namespace maya::components {

struct FlameSpan {
    std::string label;
    float       start    = 0.f;
    float       duration = 0.f;
    int         depth    = 0;      // nesting level (0 = root)
    Color       color    = {};     // default: auto from depth
};

struct FlameChartProps {
    std::vector<FlameSpan> spans = {};
    int   width      = 60;         // chart width in characters
    float time_scale = 0.f;        // 0 = auto
    bool  show_times = true;       // show duration in each bar
    int   max_depth  = 8;          // max visible depth
};

Element FlameChart(FlameChartProps props = {});

} // namespace maya::components
