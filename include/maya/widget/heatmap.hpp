#pragma once
// maya::widget::heatmap — 2D color grid with interpolated cell colors
//
// Non-interactive grid that maps float values (0.0-1.0) to colored blocks
// with color interpolation between a low and high color.
//
// Usage:
//   Heatmap hm({
//       {0.1f, 0.5f, 0.9f},
//       {0.3f, 0.7f, 0.2f},
//   });
//   hm.set_x_labels({"Mon", "Tue", "Wed"});
//   hm.set_y_labels({"AM", "PM"});
//   auto ui = hm.build();

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../style/style.hpp"

namespace maya {

// ============================================================================
// Heatmap — 2D colored grid
// ============================================================================

class Heatmap {
    std::vector<std::vector<float>> data_;
    std::vector<std::string> x_labels_;
    std::vector<std::string> y_labels_;
    Color low_color_  = Color::bright_black();
    Color high_color_ = Color::green();

public:
    Heatmap() = default;

    explicit Heatmap(std::vector<std::vector<float>> data)
        : data_(std::move(data)) {}

    void set_data(std::vector<std::vector<float>> data) { data_ = std::move(data); }
    void set_x_labels(std::vector<std::string> labels) { x_labels_ = std::move(labels); }
    void set_y_labels(std::vector<std::string> labels) { y_labels_ = std::move(labels); }
    void set_low_color(Color c) { low_color_ = c; }
    void set_high_color(Color c) { high_color_ = c; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        if (data_.empty()) {
            return Element{TextElement{.content = ""}};
        }

        int num_rows = static_cast<int>(data_.size());
        int num_cols = 0;
        for (const auto& row : data_) {
            num_cols = std::max(num_cols, static_cast<int>(row.size()));
        }
        if (num_cols == 0) {
            return Element{TextElement{.content = ""}};
        }

        // Find longest Y label for padding
        std::size_t y_label_width = 0;
        for (const auto& l : y_labels_) {
            y_label_width = std::max(y_label_width, l.size());
        }
        int y_pad = y_labels_.empty() ? 0 : static_cast<int>(y_label_width) + 1;

        // Each cell is 2 characters wide ("██") for better aspect ratio
        constexpr int cell_width = 2;
        constexpr const char* full_block = "\xe2\x96\x88\xe2\x96\x88";  // ██
        constexpr std::size_t block_bytes = 6;  // 2 × 3-byte UTF-8

        std::vector<Element> rows;
        rows.reserve(static_cast<size_t>(num_rows) + 1);

        for (int r = 0; r < num_rows; ++r) {
            std::string content;
            std::vector<StyledRun> runs;

            // Y-axis label
            if (!y_labels_.empty()) {
                std::string label;
                if (r < static_cast<int>(y_labels_.size())) {
                    label = y_labels_[static_cast<size_t>(r)];
                }
                // Right-pad to y_label_width
                while (label.size() < y_label_width) {
                    label = " " + label;
                }
                label += " ";

                runs.push_back(StyledRun{
                    content.size(), label.size(),
                    Style{}.with_dim(),
                });
                content += label;
            }

            // Data cells
            for (int c = 0; c < num_cols; ++c) {
                float val = 0.0f;
                if (r < static_cast<int>(data_.size()) &&
                    c < static_cast<int>(data_[static_cast<size_t>(r)].size())) {
                    val = std::clamp(
                        data_[static_cast<size_t>(r)][static_cast<size_t>(c)],
                        0.0f, 1.0f);
                }

                Color cell_color = interpolate(low_color_, high_color_, val);

                runs.push_back(StyledRun{
                    content.size(), block_bytes,
                    Style{}.with_fg(cell_color),
                });
                content += full_block;
            }

            rows.push_back(Element{TextElement{
                .content = std::move(content),
                .style = {},
                .wrap = TextWrap::NoWrap,
                .runs = std::move(runs),
            }});
        }

        // X-axis labels
        if (!x_labels_.empty()) {
            std::string content;
            std::vector<StyledRun> runs;

            // Pad for Y label column
            if (y_pad > 0) {
                content += std::string(static_cast<size_t>(y_pad), ' ');
            }

            std::size_t label_start = content.size();
            for (int c = 0; c < num_cols && c < static_cast<int>(x_labels_.size()); ++c) {
                std::string lbl = x_labels_[static_cast<size_t>(c)];
                // Truncate or pad to cell_width
                if (static_cast<int>(lbl.size()) > cell_width) {
                    lbl = lbl.substr(0, static_cast<size_t>(cell_width));
                }
                while (static_cast<int>(lbl.size()) < cell_width) {
                    lbl += " ";
                }
                content += lbl;
            }

            runs.push_back(StyledRun{
                label_start, content.size() - label_start,
                Style{}.with_dim(),
            });

            rows.push_back(Element{TextElement{
                .content = std::move(content),
                .style = {},
                .wrap = TextWrap::NoWrap,
                .runs = std::move(runs),
            }});
        }

        return dsl::v(std::move(rows)).build();
    }

private:
    /// Pick a cell color at intensity t in [0,1]: low (dim) for cold,
    /// high (bright) for hot. Avoids RGB blending so the terminal palette
    /// drives the actual hues.
    [[nodiscard]] static Color interpolate(Color a, Color b, float t) {
        return t < 0.5f ? a : b;
    }
};

} // namespace maya
