#pragma once
// maya::components::BarChart — Horizontal bar chart with smooth sub-character fills
//
//   BarChart({.bars = {{"Memory", 65}, {"CPU", 100}, {"Disk", 32}}})
//   BarChart({.bars = {{"A", 3.7f, Color::green()}, {"B", 1.2f}}, .max_width = 40})

#include "core.hpp"

namespace maya::components {

struct BarEntry {
    std::string label;
    float       value;
    Color       color = {};  // default = palette().primary
};

struct BarChartProps {
    std::vector<BarEntry> bars        = {};
    int                   max_width   = 30;
    bool                  show_values = true;
    std::function<std::string(float)> format = nullptr;
};

Element BarChart(BarChartProps props);

} // namespace maya::components
