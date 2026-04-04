#pragma once
// maya::components::ProgressBar — Linear progress with optional segments
//
//   ProgressBar({.value = 0.75f})
//   ProgressBar({.value = 0.5f, .width = 30, .show_percent = true})
//   ProgressBar({.segments = {{0.5, success}, {0.3, warning}, {0.2, error}}})

#include "core.hpp"

namespace maya::components {

struct Segment {
    float fraction;
    Color color;
};

struct ProgressBarProps {
    float value       = 0.f;     // 0.0 - 1.0 for simple mode
    int   width       = 20;
    bool  show_percent = false;
    Color filled      = palette().primary;
    Color empty       = palette().dim;
    std::vector<Segment> segments = {};  // multi-segment mode
};

inline Element ProgressBar(ProgressBarProps props = {}) {
    using namespace maya::dsl;

    int w = std::max(4, props.width);
    std::string bar;
    bar.reserve(w * 4);  // UTF-8 chars can be multi-byte

    if (props.segments.empty()) {
        // Simple single-color bar
        float pct = std::clamp(props.value, 0.f, 1.f);
        int filled = static_cast<int>(pct * w);
        int partial_8 = static_cast<int>((pct * w - filled) * 8);

        // We'll build Element children for colored sections
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

        std::vector<Element> parts;
        if (!filled_str.empty())
            parts.push_back(text(std::move(filled_str), Style{}.with_fg(props.filled)));
        if (!partial_str.empty())
            parts.push_back(text(std::move(partial_str), Style{}.with_fg(props.filled)));
        if (!empty_str.empty())
            parts.push_back(text(std::move(empty_str), Style{}.with_fg(props.empty)));

        if (props.show_percent) {
            parts.push_back(text(fmt(" %3.0f%%", static_cast<double>(pct * 100)),
                                 Style{}.with_fg(palette().muted)));
        }

        return hstack()(std::move(parts));
    }

    // Multi-segment mode
    std::vector<Element> parts;
    for (auto& seg : props.segments) {
        int seg_w = std::max(1, static_cast<int>(seg.fraction * w));
        std::string s;
        for (int i = 0; i < seg_w; ++i) s += "█";
        parts.push_back(text(std::move(s), Style{}.with_fg(seg.color)));
    }

    // Fill remainder with empty
    int used = 0;
    for (auto& seg : props.segments)
        used += std::max(1, static_cast<int>(seg.fraction * w));
    if (used < w) {
        std::string s;
        for (int i = 0; i < w - used; ++i) s += "░";
        parts.push_back(text(std::move(s), Style{}.with_fg(props.empty)));
    }

    if (props.show_percent) {
        float total = 0;
        for (auto& seg : props.segments) total += seg.fraction;
        parts.push_back(text(fmt(" %3.0f%%", static_cast<double>(total * 100)),
                             Style{}.with_fg(palette().muted)));
    }

    return hstack()(std::move(parts));
}

} // namespace maya::components
