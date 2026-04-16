#pragma once
// maya::widget::bar_chart — Horizontal bar chart with proportional block bars
//
// Non-interactive chart that renders labeled horizontal bars using █ characters.
//
// Usage:
//   BarChart chart({
//       {"CPU",    0.75f},
//       {"Memory", 0.45f},
//       {"Disk",   0.90f, Color::red()},
//   });
//   auto ui = chart.build();

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "../dsl.hpp"
#include "../style/style.hpp"

namespace maya {

// ============================================================================
// Bar — a single data bar
// ============================================================================

struct Bar {
    std::string label;
    float value = 0.0f;
    std::optional<Color> color{};
};

// ============================================================================
// BarChart — non-interactive horizontal bar chart
// ============================================================================

class BarChart {
    std::vector<Bar> bars_;
    float max_value_ = 0.0f;  // 0 = auto-detect from data
    Color default_color_ = Color::blue();

public:
    BarChart() = default;

    explicit BarChart(std::vector<Bar> bars, float max_value = 0.0f)
        : bars_(std::move(bars)), max_value_(max_value) {}

    void set_bars(std::vector<Bar> bars) { bars_ = std::move(bars); }
    void set_max_value(float v) { max_value_ = v; }
    void set_default_color(Color c) { default_color_ = c; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        // Use ComponentElement for dynamic width
        return Element{ComponentElement{
            .render = [self = *this](int w, int /*h*/) -> Element {
                return self.build_chart(w);
            },
            .layout = {},
        }};
    }

private:
    [[nodiscard]] Element build_chart(int width) const {
        if (bars_.empty()) {
            return Element{TextElement{.content = ""}};
        }

        // Find max value for scaling
        float max_val = max_value_;
        if (max_val <= 0.0f) {
            for (const auto& b : bars_) {
                max_val = std::max(max_val, b.value);
            }
        }
        if (max_val <= 0.0f) max_val = 1.0f;

        // Find longest label for right-alignment
        std::size_t max_label = 0;
        for (const auto& b : bars_) {
            max_label = std::max(max_label, b.label.size());
        }

        // Value suffix width: "  123.4"
        constexpr int value_suffix_width = 8;
        constexpr int label_gap = 2;  // gap between label and bar

        int bar_width = width - static_cast<int>(max_label) - label_gap - value_suffix_width;
        if (bar_width < 4) bar_width = 4;

        std::vector<Element> rows;
        rows.reserve(bars_.size());

        for (const auto& b : bars_) {
            Color bar_color = b.color.value_or(default_color_);
            float ratio = std::clamp(b.value / max_val, 0.0f, 1.0f);
            int filled = static_cast<int>(std::round(ratio * static_cast<float>(bar_width)));

            // Right-align label
            std::string padded_label(max_label - b.label.size(), ' ');
            padded_label += b.label;

            // Build the line: "  label  ████████────  value"
            std::string content;
            std::vector<StyledRun> runs;

            // Label
            runs.push_back(StyledRun{
                content.size(), padded_label.size(),
                Style{},
            });
            content += padded_label;

            // Gap
            content += "  ";

            // Filled bar
            std::string bar_str;
            for (int i = 0; i < filled; ++i)
                bar_str += "\xe2\x96\x88";  // █
            if (!bar_str.empty()) {
                runs.push_back(StyledRun{
                    content.size(), bar_str.size(),
                    Style{}.with_fg(bar_color),
                });
                content += bar_str;
            }

            // Empty track
            int empty = bar_width - filled;
            if (empty > 0) {
                std::string track;
                for (int i = 0; i < empty; ++i)
                    track += "\xe2\x94\x80";  // ─
                runs.push_back(StyledRun{
                    content.size(), track.size(),
                    Style{}.with_fg(Color::bright_black()),
                });
                content += track;
            }

            // Value suffix
            char val_buf[16];
            std::snprintf(val_buf, sizeof(val_buf), "  %.1f", static_cast<double>(b.value));
            std::string val_str = val_buf;
            runs.push_back(StyledRun{
                content.size(), val_str.size(),
                Style{}.with_dim(),
            });
            content += val_str;

            rows.push_back(Element{TextElement{
                .content = std::move(content),
                .style = {},
                .wrap = TextWrap::NoWrap,
                .runs = std::move(runs),
            }});
        }

        return dsl::v(std::move(rows)).build();
    }
};

} // namespace maya
