#pragma once
// maya::widget::table — Formatted data table
//
// Usage:
//   Table tbl({{"Name", 20}, {"Status", 10}});
//   tbl.add_row({"main.cpp", "modified"});
//   tbl.add_row({"README.md", "staged"});

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "../element/builder.hpp"
#include "../element/text.hpp"
#include "../style/border.hpp"
#include "../style/style.hpp"

namespace maya {

enum class ColumnAlign : uint8_t { Left, Center, Right };

struct ColumnDef {
    std::string header;
    int width         = 0;       // 0 = auto-fit content
    ColumnAlign align = ColumnAlign::Left;
};

struct TableConfig {
    Style header_style   = Style{}.with_bold();
    Style row_style      = Style{};
    Style alt_row_style  = Style{}.with_dim();
    Style border_style   = Style{}.with_fg(Color::rgb(60, 60, 80));
    bool stripe_rows     = false;
    int cell_padding     = 1;    // horizontal padding per cell
};

class Table {
    std::vector<ColumnDef> columns_;
    std::vector<std::vector<std::string>> rows_;
    TableConfig cfg_;

public:
    explicit Table(std::vector<ColumnDef> columns, TableConfig cfg = {})
        : columns_(std::move(columns)), cfg_(std::move(cfg)) {}

    void set_rows(std::vector<std::vector<std::string>> rows) { rows_ = std::move(rows); }
    void add_row(std::vector<std::string> row) { rows_.push_back(std::move(row)); }
    void clear_rows() { rows_.clear(); }
    [[nodiscard]] int row_count() const { return static_cast<int>(rows_.size()); }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        int ncols = static_cast<int>(columns_.size());
        if (ncols == 0) return Element{TextElement{}};

        // Compute column widths
        std::vector<int> widths(static_cast<size_t>(ncols));
        for (int c = 0; c < ncols; ++c) {
            if (columns_[static_cast<size_t>(c)].width > 0) {
                widths[static_cast<size_t>(c)] = columns_[static_cast<size_t>(c)].width;
            } else {
                // Auto: max of header and all row values
                int max_w = string_width(columns_[static_cast<size_t>(c)].header);
                for (const auto& row : rows_) {
                    if (c < static_cast<int>(row.size())) {
                        max_w = std::max(max_w, string_width(row[static_cast<size_t>(c)]));
                    }
                }
                widths[static_cast<size_t>(c)] = max_w;
            }
        }

        int pad = cfg_.cell_padding;
        std::vector<Element> output;

        // Header row
        output.push_back(build_row(columns_, widths, pad, true));

        // Header separator
        output.push_back(build_separator(widths, pad));

        // Data rows
        for (size_t r = 0; r < rows_.size(); ++r) {
            bool alt = cfg_.stripe_rows && (r % 2 == 1);
            output.push_back(build_data_row(rows_[r], widths, pad, alt));
        }

        return detail::vstack()(std::move(output));
    }

private:
    [[nodiscard]] Element build_row(const std::vector<ColumnDef>& cols,
                                     const std::vector<int>& widths,
                                     int pad, bool is_header) const {
        std::vector<Element> cells;
        for (size_t c = 0; c < cols.size(); ++c) {
            std::string cell = pad_cell(cols[c].header, widths[c], cols[c].align, pad);
            cells.push_back(Element{TextElement{
                .content = std::move(cell),
                .style = is_header ? cfg_.header_style : cfg_.row_style,
            }});
        }
        return detail::hstack()(std::move(cells));
    }

    [[nodiscard]] Element build_data_row(const std::vector<std::string>& row,
                                          const std::vector<int>& widths,
                                          int pad, bool alt) const {
        std::vector<Element> cells;
        for (size_t c = 0; c < columns_.size(); ++c) {
            std::string val = c < row.size() ? row[c] : "";
            std::string cell = pad_cell(val, widths[c], columns_[c].align, pad);
            cells.push_back(Element{TextElement{
                .content = std::move(cell),
                .style = alt ? cfg_.alt_row_style : cfg_.row_style,
            }});
        }
        return detail::hstack()(std::move(cells));
    }

    [[nodiscard]] Element build_separator(const std::vector<int>& widths, int pad) const {
        std::string line;
        for (size_t c = 0; c < widths.size(); ++c) {
            int total = widths[c] + pad * 2;
            for (int i = 0; i < total; ++i) line += "─";
        }
        return Element{TextElement{
            .content = std::move(line),
            .style = cfg_.border_style,
        }};
    }

    static std::string pad_cell(const std::string& content, int width,
                                 ColumnAlign align, int padding) {
        int content_w = string_width(content);
        int total = width + padding * 2;
        int space = total - content_w;
        if (space < 0) space = 0;

        std::string result;
        result.reserve(static_cast<size_t>(total));

        int left_pad = padding;
        int right_pad = padding;

        if (align == ColumnAlign::Right) {
            left_pad = padding + (width - content_w);
        } else if (align == ColumnAlign::Center) {
            int gap = width - content_w;
            left_pad = padding + gap / 2;
            right_pad = padding + gap - gap / 2;
        } else {
            right_pad = padding + (width - content_w);
        }

        if (left_pad < 0) left_pad = 0;
        if (right_pad < 0) right_pad = 0;

        result.append(static_cast<size_t>(left_pad), ' ');
        result += content;
        result.append(static_cast<size_t>(right_pad), ' ');

        return result;
    }
};

} // namespace maya
