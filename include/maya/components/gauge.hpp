#pragma once
// maya::components::Gauge — Labeled value meter with visual bar
//
//   Gauge({.value = 75.f, .max = 100.f, .label = "CPU"})
//   Gauge({.value = 3.2f, .min = 0.f, .max = 5.f, .label = "Load",
//          .thresholds = {{2.f, success}, {4.f, warning}, {4.5f, error}}})

#include "core.hpp"

namespace maya::components {

struct GaugeThreshold {
    float value;
    Color color;
};

struct GaugeProps {
    float       value      = 0.f;
    float       min        = 0.f;
    float       max        = 1.f;
    std::string label      = "";
    int         width      = 20;
    bool        show_value = true;
    std::vector<GaugeThreshold> thresholds = {};
    std::function<std::string(float)> format = nullptr;
};

Element Gauge(GaugeProps props = {});

} // namespace maya::components
