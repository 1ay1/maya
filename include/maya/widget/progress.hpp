#pragma once
// maya::widget::progress — Progress bar indicator
//
// Renders a horizontal progress bar with configurable fill, width, and colors.
//
// Usage:
//   ProgressBar bar;
//   bar.set(0.75f);
//   auto ui = h(bar, text(" 75%"));

#include <algorithm>
#include <cmath>
#include <string>

#include "../element/builder.hpp"
#include "../style/style.hpp"

namespace maya {

// ============================================================================
// ProgressConfig — appearance configuration
// ============================================================================

struct ProgressConfig {
    int   width      = 40;      // bar width in columns
    Color fill_color = Color::rgb(100, 200, 255);
    Color bg_color   = Color::rgb(60, 60, 80);
    bool  show_track = true;    // show empty track
};

// ============================================================================
// ProgressBar — horizontal progress indicator
// ============================================================================

class ProgressBar {
    float value_ = 0.0f;
    ProgressConfig cfg_;

public:
    ProgressBar() = default;
    explicit ProgressBar(ProgressConfig cfg) : cfg_(cfg) {}

    void set(float v) { value_ = std::clamp(v, 0.0f, 1.0f); }
    [[nodiscard]] float value() const { return value_; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        int w = cfg_.width;
        int filled = static_cast<int>(std::round(value_ * w));

        // Use block elements for sub-character precision
        // Full blocks for filled portion, spaces for empty
        // Last partial block uses fractional block chars: ▏▎▍▌▋▊▉█
        static constexpr const char* partials[] = {
            " ", "▏", "▎", "▍", "▌", "▋", "▊", "▉"
        };

        float exact = value_ * w;
        int full = static_cast<int>(exact);
        int frac = static_cast<int>((exact - full) * 8.0f);
        if (full >= w) { full = w; frac = 0; }

        std::string bar;
        bar.reserve(static_cast<size_t>(w * 4));

        // Filled portion
        for (int i = 0; i < full; ++i) bar += "█";

        // Partial block
        if (full < w && frac > 0) {
            bar += partials[frac];
            ++full;
        }

        // Remaining empty
        int empty = w - full;

        std::vector<Element> parts;
        if (!bar.empty()) {
            parts.push_back(Element{TextElement{
                .content = std::move(bar),
                .style = Style{}.with_fg(cfg_.fill_color),
            }});
        }
        if (empty > 0 && cfg_.show_track) {
            std::string track;
            track.reserve(static_cast<size_t>(empty * 3));
            for (int i = 0; i < empty; ++i) track += "─";
            parts.push_back(Element{TextElement{
                .content = std::move(track),
                .style = Style{}.with_fg(cfg_.bg_color),
            }});
        }

        return detail::hstack()(std::move(parts));
    }
};

} // namespace maya
