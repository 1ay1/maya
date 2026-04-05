#pragma once
// maya::components::Heatmap — Grid heatmap like GitHub's contribution graph
//
//   Heatmap({.data = {{0.1, 0.5, 0.9}, {0.0, 0.3, 1.0}},
//            .row_labels = {"Mon", "Tue"},
//            .col_labels = {"W1", "W2", "W3"}})

#include "core.hpp"

#include <algorithm>
#include <cmath>

namespace maya::components {

struct HeatmapProps {
    std::vector<std::vector<float>> data = {};  // rows of values, 0.0-1.0
    std::vector<std::string> row_labels = {};   // optional row labels
    std::vector<std::string> col_labels = {};   // optional column labels
    Color low_color  = {};    // color for 0.0 (default: palette().dim)
    Color high_color = {};    // color for 1.0 (default: palette().success)
    bool  show_values = false; // show numeric value in each cell
};

Element Heatmap(HeatmapProps props = {});

} // namespace maya::components
