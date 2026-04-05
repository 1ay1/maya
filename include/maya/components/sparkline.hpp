#pragma once
// maya::components::Sparkline — Mini inline chart using Unicode block characters
//
//   Sparkline({.data = {1, 3, 7, 4, 2}})
//   Sparkline({.data = values, .width = 20, .show_range = true})
//   Sparkline({.data = values, .color = Color::green()})

#include "core.hpp"

namespace maya::components {

struct SparklineProps {
    std::vector<float> data       = {};
    int                width      = 0;     // 0 = use data.size()
    Color              color      = {};    // default = palette().primary
    bool               show_range = false; // show "min-max" after
};

Element Sparkline(SparklineProps props = {});

} // namespace maya::components
