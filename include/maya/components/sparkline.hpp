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

inline Element Sparkline(SparklineProps props) {
    using namespace maya::dsl;

    if (props.data.empty()) return text("");

    auto [mn_it, mx_it] = std::minmax_element(props.data.begin(), props.data.end());
    float mn = *mn_it, mx = *mx_it;

    // Resample if width specified and differs from data size
    int w = props.width > 0 ? props.width : static_cast<int>(props.data.size());
    std::vector<float> samples(w);
    for (int i = 0; i < w; ++i) {
        int idx = static_cast<int>(static_cast<float>(i) / w * props.data.size());
        idx = std::clamp(idx, 0, static_cast<int>(props.data.size()) - 1);
        samples[i] = props.data[idx];
    }

    // Build bar string
    static constexpr const char* blocks[] = {
        "\u2581", "\u2582", "\u2583", "\u2584",
        "\u2585", "\u2586", "\u2587", "\u2588"
    };

    std::string bar;
    bar.reserve(w * 3);
    for (float v : samples) {
        float norm = (mn == mx) ? 0.5f : (v - mn) / (mx - mn);
        int idx = std::clamp(static_cast<int>(norm * 7), 0, 7);
        bar += blocks[idx];
    }

    // Resolve color: default-constructed Color is Named white (r=7, g=0, b=0)
    Color c = props.color;
    if (c.kind() == Color::Kind::Named && c.r() == 7 && c.g() == 0 && c.b() == 0)
        c = palette().primary;

    std::vector<Element> parts;
    parts.push_back(text(std::move(bar), Style{}.with_fg(c)));

    if (props.show_range) {
        parts.push_back(text(fmt(" %.1f-%.1f",
            static_cast<double>(mn), static_cast<double>(mx)),
            Style{}.with_fg(palette().muted)));
    }

    return hstack()(std::move(parts));
}

} // namespace maya::components
