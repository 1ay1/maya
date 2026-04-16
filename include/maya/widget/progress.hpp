#pragma once
// maya::widget::progress — Progress bar with label, percentage, and elapsed time
//
// Zed's polished indicators + Claude Code's time tracking UX.
//
//   Indexing files...  ████████████████████────────────────────  68%  2.3s
//
// Or with bordered card:
//   ╭─ Progress ───────────────────────────╮
//   │ Indexing files...                    │
//   │ ████████████████████──────────  68%  │
//   │                              2.3s   │
//   ╰──────────────────────────────────────╯
//
// Usage:
//   ProgressBar bar;
//   bar.set(0.75f);
//   bar.set_label("Indexing files...");
//   auto ui = bar.build();

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../element/builder.hpp"
#include "../style/style.hpp"

namespace maya {

struct ProgressConfig {
    int   width      = 0;       // bar width in columns; 0 = fill available width
    Color fill_color = Color::blue();
    Color bg_color   = Color::bright_black();
    bool  show_track = true;
    bool  show_percentage = true;
};

class ProgressBar {
    float value_ = 0.0f;
    float elapsed_ = 0.0f;
    std::string label_;
    ProgressConfig cfg_;

public:
    ProgressBar() = default;
    explicit ProgressBar(ProgressConfig cfg) : cfg_(cfg) {}

    void set(float v) { value_ = std::clamp(v, 0.0f, 1.0f); }
    void set_elapsed(float seconds) { elapsed_ = seconds; }
    void set_label(std::string_view label) { label_ = std::string{label}; }

    [[nodiscard]] float value() const { return value_; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        if (cfg_.width > 0) {
            return build_bar(cfg_.width);
        }
        // Width 0 = fill available width via ComponentElement
        // Capture *this by value to avoid dangling pointer
        return Element{ComponentElement{
            .render = [self = *this](int w, int /*h*/) -> Element {
                int suffix_cols = 0;
                if (self.cfg_.show_percentage) suffix_cols += 6;
                if (self.elapsed_ > 0.0f) suffix_cols += 8;
                int bar_w = w - suffix_cols;
                if (!self.label_.empty()) bar_w -= static_cast<int>(self.label_.size()) + 2;
                if (bar_w < 10) bar_w = 10;
                return self.build_bar(bar_w);
            },
            .layout = {},
        }};
    }

    [[nodiscard]] Element build_bar(int w) const {
        // Use block elements for sub-character precision
        static constexpr const char* partials[] = {
            " ", "\xe2\x96\x8f", "\xe2\x96\x8e", "\xe2\x96\x8d",
            "\xe2\x96\x8c", "\xe2\x96\x8b", "\xe2\x96\x8a", "\xe2\x96\x89"
        };

        float exact = value_ * static_cast<float>(w);
        int full = static_cast<int>(exact);
        int frac = static_cast<int>((exact - static_cast<float>(full)) * 8.0f);
        if (full >= w) { full = w; frac = 0; }

        // Build bar string with styled runs
        std::string content;
        std::vector<StyledRun> runs;

        // Label
        if (!label_.empty()) {
            auto label_style = Style{};
            runs.push_back(StyledRun{content.size(), label_.size(), label_style});
            content += label_;
            content += "  ";
            runs.push_back(StyledRun{content.size() - 2, 2, Style{}});
        }

        // Filled portion
        std::string filled_str;
        for (int i = 0; i < full; ++i) filled_str += "\xe2\x96\x88";  // █

        // Partial block
        int full_count = full;
        if (full_count < w && frac > 0) {
            filled_str += partials[frac];
            ++full_count;
        }

        if (!filled_str.empty()) {
            runs.push_back(StyledRun{content.size(), filled_str.size(),
                Style{}.with_fg(cfg_.fill_color)});
            content += filled_str;
        }

        // Empty track
        int empty = w - full_count;
        if (empty > 0 && cfg_.show_track) {
            std::string track;
            for (int i = 0; i < empty; ++i) track += "\xe2\x94\x80";  // ─
            runs.push_back(StyledRun{content.size(), track.size(),
                Style{}.with_fg(cfg_.bg_color)});
            content += track;
        }

        // Percentage
        if (cfg_.show_percentage) {
            char pct_buf[8];
            std::snprintf(pct_buf, sizeof(pct_buf), "  %d%%",
                static_cast<int>(value_ * 100.0f));
            std::string pct = pct_buf;
            runs.push_back(StyledRun{content.size(), pct.size(),
                Style{}});
            content += pct;
        }

        // Elapsed time
        if (elapsed_ > 0.0f) {
            std::string ts = "  " + format_elapsed();
            runs.push_back(StyledRun{content.size(), 2, Style{}});
            runs.push_back(StyledRun{content.size() + 2, ts.size() - 2,
                Style{}.with_dim()});
            content += ts;
        }

        return Element{TextElement{
            .content = std::move(content),
            .style = {},
            .wrap = TextWrap::NoWrap,
            .runs = std::move(runs),
        }};
    }

private:
    [[nodiscard]] std::string format_elapsed() const {
        char buf[32];
        if (elapsed_ < 1.0f) {
            std::snprintf(buf, sizeof(buf), "%.0fms", static_cast<double>(elapsed_ * 1000.0f));
            return buf;
        }
        if (elapsed_ < 60.0f) {
            std::snprintf(buf, sizeof(buf), "%.1fs", static_cast<double>(elapsed_));
            return buf;
        }
        int mins = static_cast<int>(elapsed_) / 60;
        float secs = elapsed_ - static_cast<float>(mins * 60);
        std::snprintf(buf, sizeof(buf), "%dm%.0fs", mins, static_cast<double>(secs));
        return buf;
    }
};

} // namespace maya
