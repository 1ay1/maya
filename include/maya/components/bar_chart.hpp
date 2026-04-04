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

inline Element BarChart(BarChartProps props) {
    using namespace maya::dsl;

    if (props.bars.empty()) return text("");

    static constexpr const char* partials[] = {
        " ", "▏", "▎", "▍", "▌", "▋", "▊", "▉"
    };
    auto is_default_color = [](const Color& c) {
        return c.kind() == Color::Kind::Named && c.r() == 7 && c.g() == 0 && c.b() == 0;
    };

    // Find max value and max label width
    float max_val = 0.f;
    int max_label = 0;
    for (auto& b : props.bars) {
        if (b.value > max_val) max_val = b.value;
        int len = static_cast<int>(b.label.size());
        if (len > max_label) max_label = len;
    }
    if (max_val <= 0.f) max_val = 1.f;

    int w = std::max(4, props.max_width);
    std::vector<Element> rows;

    for (auto& b : props.bars) {
        float ratio = std::clamp(b.value / max_val, 0.f, 1.f);
        float cells = ratio * w;
        int full = static_cast<int>(cells);
        int frac = static_cast<int>((cells - full) * 8);

        Color bar_color = is_default_color(b.color) ? palette().primary : b.color;

        std::string filled_str;
        for (int i = 0; i < full; ++i) filled_str += "█";

        std::string partial_str;
        if (full < w && frac > 0) partial_str = partials[frac];

        int remaining = w - full - (frac > 0 ? 1 : 0);
        std::string empty_str;
        for (int i = 0; i < remaining; ++i) empty_str += "░";

        // Right-align label
        std::string label = b.label;
        while (static_cast<int>(label.size()) < max_label)
            label.insert(label.begin(), ' ');

        std::vector<Element> parts;
        parts.push_back(text(label + "  ", Style{}.with_fg(palette().text)));
        if (!filled_str.empty())
            parts.push_back(text(std::move(filled_str), Style{}.with_fg(bar_color)));
        if (!partial_str.empty())
            parts.push_back(text(std::move(partial_str), Style{}.with_fg(bar_color)));
        if (!empty_str.empty())
            parts.push_back(text(std::move(empty_str), Style{}.with_fg(palette().dim)));

        if (props.show_values) {
            std::string val = props.format
                ? props.format(b.value)
                : fmt("%.1f", static_cast<double>(b.value));
            parts.push_back(text(" " + val, Style{}.with_fg(palette().muted)));
        }

        rows.push_back(hstack()(std::move(parts)));
    }

    return vstack()(std::move(rows));
}

} // namespace maya::components
