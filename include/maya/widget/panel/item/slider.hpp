#pragma once
// maya::panel item widget: Slider — a bounded real, drawn as a bar.
//
// For ratios judged visually (an MMR lambda, a temperature): ←/→ step the
// value, the bar shows roughly-where-in-range at a glance. A quantity you'd
// rather TYPE is a Number, not a Slider.

#include <algorithm>
#include <string>
#include <utility>

#include "../../../style/style.hpp"
#include "../context.hpp"

namespace maya::panel {

struct Slider {
    double value = 0.0, min = 0.0, max = 1.0;
    int    decimals = 2;
};

[[nodiscard]] inline std::pair<std::string, Style>
render(const Slider& c, const ItemCtx& ctx) {
    constexpr int kCells = 12;
    const double span = (c.max > c.min) ? (c.max - c.min) : 1.0;
    const double t    = std::clamp((c.value - c.min) / span, 0.0, 1.0);
    const int    on   = static_cast<int>(t * kCells + 0.5);
    std::string bar;
    for (int i = 0; i < kCells; ++i)
        bar += (i < on) ? "\xe2\x96\x86" : "\xe2\x96\x81";       // ▆ / ▁

    // Fixed-decimal without <format>/<sstream>: these are bounded ratios,
    // so integer scaling is exact and allocation-free.
    const int dec = std::clamp(c.decimals, 0, 4);
    int scale = 1;
    for (int i = 0; i < dec; ++i) scale *= 10;
    const long long scaled =
        static_cast<long long>(c.value * scale + (c.value < 0 ? -0.5 : 0.5));
    std::string num = std::to_string(scaled / scale);
    if (dec > 0) {
        long long frac = scaled % scale;
        if (frac < 0) frac = -frac;
        std::string fs = std::to_string(frac);
        fs.insert(fs.begin(), static_cast<std::size_t>(dec) - fs.size(), '0');
        num += "." + fs;
    }
    return {bar + "  " + num, Style{}.with_fg(ctx.theme.value)};
}

} // namespace maya::panel
