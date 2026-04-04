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

inline Element Gauge(GaugeProps props = {}) {
    using namespace maya::dsl;

    float range = props.max - props.min;
    float norm  = (range > 0.f) ? std::clamp((props.value - props.min) / range, 0.f, 1.f) : 0.f;

    // Determine bar color
    Color bar_color;
    if (!props.thresholds.empty()) {
        // Find highest threshold <= value
        bar_color = palette().primary;
        for (auto& t : props.thresholds)
            if (props.value >= t.value) bar_color = t.color;
    } else {
        if (norm > 0.9f)       bar_color = palette().error;
        else if (norm >= 0.7f) bar_color = palette().warning;
        else                   bar_color = palette().primary;
    }

    // Build bar with sub-character precision
    int w = std::max(4, props.width);
    int filled = static_cast<int>(norm * w);
    int partial_8 = static_cast<int>((norm * w - filled) * 8);

    std::string filled_str;
    for (int i = 0; i < filled; ++i) filled_str += "█";

    std::string partial_str;
    if (filled < w && partial_8 > 0) {
        static constexpr const char* partials[] = {
            " ", "▏", "▎", "▍", "▌", "▋", "▊", "▉"
        };
        partial_str = partials[partial_8];
    }

    int remaining = w - filled - (partial_8 > 0 ? 1 : 0);
    std::string empty_str;
    for (int i = 0; i < remaining; ++i) empty_str += "░";

    // Assemble parts
    std::vector<Element> parts;

    if (!props.label.empty())
        parts.push_back(text(props.label + " ", Style{}.with_fg(palette().text)));

    if (!filled_str.empty())
        parts.push_back(text(std::move(filled_str), Style{}.with_fg(bar_color)));
    if (!partial_str.empty())
        parts.push_back(text(std::move(partial_str), Style{}.with_fg(bar_color)));
    if (!empty_str.empty())
        parts.push_back(text(std::move(empty_str), Style{}.with_fg(palette().dim)));

    if (props.show_value) {
        std::string val_str;
        if (props.format) {
            val_str = " " + props.format(props.value);
        } else if (props.min == 0.f && props.max == 1.f) {
            val_str = fmt(" %3.0f%%", static_cast<double>(norm * 100));
        } else {
            val_str = fmt(" %.1f/%.1f", static_cast<double>(props.value),
                                         static_cast<double>(props.max));
        }
        parts.push_back(text(std::move(val_str), Style{}.with_fg(palette().muted)));
    }

    return hstack()(std::move(parts));
}

} // namespace maya::components
