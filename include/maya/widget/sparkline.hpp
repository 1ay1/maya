#pragma once
// maya::widget::sparkline — Inline mini chart using Unicode block characters
//
// Renders a single-line bar chart from numeric data.
// Maps each value to one of 8 Unicode block levels: ▁▂▃▄▅▆▇█
//
//   CPU  ▂▃▅▇█▆▄▃▂▁▃▅▇█▆▄  12% / 98%
//
// Usage:
//   Sparkline spark({0.1, 0.3, 0.5, 0.8, 1.0, 0.6});
//   spark.set_label("CPU");
//   auto ui = spark.build();

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../element/builder.hpp"
#include "../style/style.hpp"

namespace maya {

// ============================================================================
// SparklineConfig — appearance configuration
// ============================================================================

struct SparklineConfig {
    Color color           = Color::rgb(97, 175, 239);
    Style label_style     = Style{}.with_fg(Color::rgb(200, 204, 212));
    Style value_style     = Style{}.with_fg(Color::rgb(150, 156, 170));
    bool show_min_max     = false;
    bool show_last        = false;
};

// ============================================================================
// Sparkline — inline mini chart widget
// ============================================================================

class Sparkline {
    std::vector<float> data_;
    std::string label_;
    std::optional<float> min_override_;
    std::optional<float> max_override_;
    SparklineConfig cfg_;

    // Unicode block elements U+2581 through U+2588
    // Each is a 3-byte UTF-8 sequence: E2 96 81..88
    static constexpr const char* blocks[] = {
        "\xe2\x96\x81",  // ▁
        "\xe2\x96\x82",  // ▂
        "\xe2\x96\x83",  // ▃
        "\xe2\x96\x84",  // ▄
        "\xe2\x96\x85",  // ▅
        "\xe2\x96\x86",  // ▆
        "\xe2\x96\x87",  // ▇
        "\xe2\x96\x88",  // █
    };

public:
    explicit Sparkline(std::vector<float> data, SparklineConfig cfg = {})
        : data_(std::move(data)), cfg_(std::move(cfg)) {}

    // -- Mutators --
    void set_data(std::vector<float> data) { data_ = std::move(data); }
    void set_label(std::string_view lbl)   { label_ = std::string{lbl}; }
    void set_min(float v)                  { min_override_ = v; }
    void set_max(float v)                  { max_override_ = v; }
    void set_color(Color c)                { cfg_.color = c; }
    void set_show_min_max(bool b)          { cfg_.show_min_max = b; }
    void set_show_last(bool b)             { cfg_.show_last = b; }

    // -- Accessors --
    [[nodiscard]] const std::vector<float>& data() const { return data_; }

    // -- Node concept: build into Element --
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        if (data_.empty()) {
            return Element{TextElement{
                .content = label_.empty() ? "(no data)" : label_ + "  (no data)",
                .style = cfg_.value_style,
            }};
        }

        // Compute range
        float data_min = min_override_.value_or(*std::min_element(data_.begin(), data_.end()));
        float data_max = max_override_.value_or(*std::max_element(data_.begin(), data_.end()));
        float range = data_max - data_min;
        if (range < std::numeric_limits<float>::epsilon()) range = 1.0f;

        // Build the spark characters
        std::string spark;
        spark.reserve(data_.size() * 3);  // each block is 3 bytes UTF-8
        for (float v : data_) {
            float norm = (v - data_min) / range;
            norm = std::clamp(norm, 0.0f, 1.0f);
            int level = static_cast<int>(norm * 7.0f + 0.5f);
            level = std::clamp(level, 0, 7);
            spark += blocks[level];
        }

        // Compose the full line with runs
        std::string content;
        std::vector<StyledRun> runs;

        // Label
        if (!label_.empty()) {
            size_t lbl_start = content.size();
            content += label_;
            content += "  ";
            runs.push_back(StyledRun{lbl_start, label_.size(), cfg_.label_style});
        }

        // Spark data
        size_t spark_start = content.size();
        content += spark;
        runs.push_back(StyledRun{spark_start, spark.size(),
                                  Style{}.with_fg(cfg_.color)});

        // Min/max/last values
        std::string suffix;
        if (cfg_.show_min_max) {
            suffix += "  " + format_float(data_min) + " / " + format_float(data_max);
        }
        if (cfg_.show_last) {
            float last = data_.back();
            if (!suffix.empty()) suffix += "  ";
            else suffix += "  ";
            suffix += format_float(last);
        }
        if (!suffix.empty()) {
            size_t suffix_start = content.size();
            content += suffix;
            runs.push_back(StyledRun{suffix_start, suffix.size(), cfg_.value_style});
        }

        return Element{TextElement{
            .content = std::move(content),
            .style = {},
            .wrap = TextWrap::NoWrap,
            .runs = std::move(runs),
        }};
    }

private:
    static std::string format_float(float v) {
        // Simple formatting: up to 1 decimal place
        if (v == static_cast<float>(static_cast<int>(v))) {
            return std::to_string(static_cast<int>(v));
        }
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(v));
        return buf;
    }
};

} // namespace maya
