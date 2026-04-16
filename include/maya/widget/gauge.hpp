#pragma once
// maya::widget::gauge — Arc/bar gauge meter for displaying a 0-1 value
//
// Non-interactive gauge that shows a value as a visual meter.
// Arc style: bordered box with filled/empty bar and centered percentage.
// Bar style: vertical bar using block elements ▁▂▃▄▅▆▇█.
//
// Usage:
//   Gauge g(0.45f, "CPU");
//   auto ui = g.build();

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <cstdint>

#include "../dsl.hpp"
#include "../style/style.hpp"

namespace maya {

// ============================================================================
// GaugeStyle — visual mode
// ============================================================================

enum class GaugeStyle : uint8_t {
    Arc,
    Bar,
};

// ============================================================================
// Gauge — non-interactive meter widget
// ============================================================================

class Gauge {
    float value_ = 0.0f;
    std::string label_;
    Color color_ = Color::blue();
    GaugeStyle style_ = GaugeStyle::Arc;

public:
    Gauge() = default;

    explicit Gauge(float value, std::string label = {},
                   Color color = Color::blue(),
                   GaugeStyle style = GaugeStyle::Arc)
        : value_(std::clamp(value, 0.0f, 1.0f))
        , label_(std::move(label))
        , color_(color)
        , style_(style) {}

    void set_value(float v) { value_ = std::clamp(v, 0.0f, 1.0f); }
    void set_label(std::string_view l) { label_ = std::string{l}; }
    void set_color(Color c) { color_ = c; }
    void set_style(GaugeStyle s) { style_ = s; }

    [[nodiscard]] float value() const { return value_; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        switch (style_) {
            case GaugeStyle::Arc: return build_arc();
            case GaugeStyle::Bar: return build_bar();
        }
        return build_arc();
    }

private:
    [[nodiscard]] Element build_arc() const {
        int pct = static_cast<int>(std::round(static_cast<double>(value_) * 100.0));

        // Percentage text centered
        char pct_buf[8];
        std::snprintf(pct_buf, sizeof(pct_buf), "%d%%", pct);
        std::string pct_str = pct_buf;

        auto pct_elem = Element{TextElement{
            .content = pct_str,
            .style = Style{}.with_bold(),
        }};

        // Label below percentage
        Element label_elem = Element{TextElement{.content = ""}};
        if (!label_.empty()) {
            label_elem = Element{TextElement{
                .content = label_,
                .style = Style{}.with_dim(),
            }};
        }

        // Bottom bar: filled ━ and empty ─ across the width
        // Use ComponentElement to get dynamic width
        auto bar_elem = Element{ComponentElement{
            .render = [val = value_, col = color_](int w, int /*h*/) -> Element {
                int inner_w = w;
                if (inner_w < 2) inner_w = 2;

                int filled = static_cast<int>(std::round(
                    static_cast<float>(inner_w) * val));
                filled = std::clamp(filled, 0, inner_w);
                int empty = inner_w - filled;

                std::string content;
                std::vector<StyledRun> runs;

                std::string filled_str;
                for (int i = 0; i < filled; ++i)
                    filled_str += "\xe2\x94\x81";  // ━
                if (!filled_str.empty()) {
                    runs.push_back(StyledRun{
                        content.size(), filled_str.size(),
                        Style{}.with_fg(col),
                    });
                    content += filled_str;
                }

                std::string empty_str;
                for (int i = 0; i < empty; ++i)
                    empty_str += "\xe2\x94\x80";  // ─
                if (!empty_str.empty()) {
                    runs.push_back(StyledRun{
                        content.size(), empty_str.size(),
                        Style{}.with_fg(Color::bright_black()),
                    });
                    content += empty_str;
                }

                return Element{TextElement{
                    .content = std::move(content),
                    .style = {},
                    .wrap = TextWrap::NoWrap,
                    .runs = std::move(runs),
                }};
            },
            .layout = {},
        }};

        auto inner = (dsl::v(std::move(pct_elem),
             std::move(label_elem),
             std::move(bar_elem))
            | dsl::align(Align::Center)
            | dsl::gap(0)).build();

        return (dsl::v(std::move(inner))
            | dsl::border(BorderStyle::Round)
            | dsl::bcolor(Color::bright_black())
            | dsl::padding(0, 1)).build();
    }

    [[nodiscard]] Element build_bar() const {
        // Vertical bar using block elements: ▁▂▃▄▅▆▇█ (bottom to top)
        // U+2581 through U+2588
        static constexpr const char* blocks[] = {
            " ",
            "\xe2\x96\x81",  // ▁
            "\xe2\x96\x82",  // ▂
            "\xe2\x96\x83",  // ▃
            "\xe2\x96\x84",  // ▄
            "\xe2\x96\x85",  // ▅
            "\xe2\x96\x86",  // ▆
            "\xe2\x96\x87",  // ▇
            "\xe2\x96\x88",  // █
        };

        int total_rows = 8;
        int filled_eighths = static_cast<int>(std::round(
            static_cast<double>(value_) * static_cast<double>(total_rows) * 8.0));
        filled_eighths = std::clamp(filled_eighths, 0, total_rows * 8);

        int full_rows = filled_eighths / 8;
        int partial = filled_eighths % 8;

        std::vector<Element> rows;
        rows.reserve(static_cast<size_t>(total_rows) + 2);

        // Percentage on top
        char pct_buf[8];
        std::snprintf(pct_buf, sizeof(pct_buf), "%d%%",
            static_cast<int>(std::round(static_cast<double>(value_) * 100.0)));
        rows.push_back(Element{TextElement{
            .content = std::string(pct_buf),
            .style = Style{}.with_bold(),
        }});

        // Bar rows from top to bottom
        for (int row = total_rows - 1; row >= 0; --row) {
            const char* block;
            Color fg;

            if (row < full_rows) {
                block = blocks[8];  // █
                fg = color_;
            } else if (row == full_rows && partial > 0) {
                block = blocks[partial];
                fg = color_;
            } else {
                block = "\xe2\x94\x80";  // ─
                fg = Color::bright_black();
            }

            rows.push_back(Element{TextElement{
                .content = std::string(block),
                .style = Style{}.with_fg(fg),
            }});
        }

        // Label at bottom
        if (!label_.empty()) {
            rows.push_back(Element{TextElement{
                .content = label_,
                .style = Style{}.with_dim(),
            }});
        }

        return (dsl::v(std::move(rows))
            | dsl::align(Align::Center)).build();
    }
};

} // namespace maya
