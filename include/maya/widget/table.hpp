#pragma once
// maya::widget::table — Formatted data table with optional bordered card
//
// Zed's bordered card presentation + Claude Code's clean data display.
//
//   ╭─ Files ──────────────────────────────╮
//   │  Name          Status     Size       │
//   │  ─────────────────────────────────   │
//   │  main.cpp      modified   2.3k       │
//   │  README.md     staged     1.1k       │
//   ╰──────────────────────────────────────╯
//
// Or without card:
//    Name          Status     Size
//    ─────────────────────────────────
//    main.cpp      modified   2.3k
//    README.md     staged     1.1k
//
// Usage:
//   Table tbl({{"Name", 20}, {"Status", 10}});
//   tbl.add_row({"main.cpp", "modified"});
//   auto ui = tbl.build();

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../dsl.hpp"
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
    Style separator_style = Style{}.with_fg(Color::bright_black());
    bool stripe_rows     = true;
    int cell_padding     = 1;
    bool show_border     = false;
    std::string title;           // shown in border text when bordered
    Color border_color   = Color::bright_black();
};

class Table {
    std::vector<ColumnDef> columns_;
    std::vector<std::vector<std::string>> rows_;
    TableConfig cfg_;

public:
    explicit Table(std::vector<ColumnDef> columns)
        : columns_(std::move(columns)), cfg_{} {}

    Table(std::vector<ColumnDef> columns, TableConfig cfg)
        : columns_(std::move(columns)), cfg_(std::move(cfg)) {}

    void set_rows(std::vector<std::vector<std::string>> rows) { rows_ = std::move(rows); }
    void add_row(std::vector<std::string> row) { rows_.push_back(std::move(row)); }
    void clear_rows() { rows_.clear(); }
    [[nodiscard]] int row_count() const { return static_cast<int>(rows_.size()); }
    void set_title(std::string_view t) { cfg_.title = std::string{t}; }
    void set_bordered(bool b) { cfg_.show_border = b; }

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
        output.push_back(build_header_row(widths, pad));

        // Separator
        output.push_back(build_separator(widths, pad));

        // Data rows
        for (size_t r = 0; r < rows_.size(); ++r) {
            bool alt = cfg_.stripe_rows && (r % 2 == 1);
            output.push_back(build_data_row(rows_[r], widths, pad, alt));
        }

        auto content = dsl::v(std::move(output));

        if (cfg_.show_border) {
            auto bordered = std::move(content)
                | dsl::border(BorderStyle::Round)
                | dsl::bcolor(cfg_.border_color)
                | dsl::padding(0, 1, 0, 1);
            if (!cfg_.title.empty()) {
                bordered = std::move(bordered)
                    | dsl::btext(" " + cfg_.title + " ",
                        BorderTextPos::Top, BorderTextAlign::Start);
            }
            return std::move(bordered).build();
        }

        return content.build();
    }

private:
    [[nodiscard]] Element build_header_row(const std::vector<int>& widths, int pad) const {
        std::string content;
        std::vector<StyledRun> runs;

        for (size_t c = 0; c < columns_.size(); ++c) {
            std::string cell = pad_cell(columns_[c].header, widths[c], columns_[c].align, pad);
            runs.push_back(StyledRun{content.size(), cell.size(), cfg_.header_style});
            content += cell;
        }

        return Element{TextElement{
            .content = std::move(content),
            .style = {},
            .wrap = TextWrap::NoWrap,
            .runs = std::move(runs),
        }};
    }

    [[nodiscard]] Element build_data_row(const std::vector<std::string>& row,
                                          const std::vector<int>& widths,
                                          int pad, bool alt) const {
        std::string content;
        std::vector<StyledRun> runs;
        Style style = alt ? cfg_.alt_row_style : cfg_.row_style;

        for (size_t c = 0; c < columns_.size(); ++c) {
            std::string val = c < row.size() ? row[c] : "";
            std::string cell = pad_cell(val, widths[c], columns_[c].align, pad);
            runs.push_back(StyledRun{content.size(), cell.size(), style});
            content += cell;
        }

        return Element{TextElement{
            .content = std::move(content),
            .style = {},
            .wrap = TextWrap::NoWrap,
            .runs = std::move(runs),
        }};
    }

    [[nodiscard]] Element build_separator(const std::vector<int>& widths, int pad) const {
        std::string line;
        for (size_t c = 0; c < widths.size(); ++c) {
            int total = widths[c] + pad * 2;
            for (int i = 0; i < total; ++i) line += "\xe2\x94\x80";  // ─
        }
        return Element{TextElement{
            .content = std::move(line),
            .style = cfg_.separator_style,
        }};
    }

    static std::string pad_cell(const std::string& content, int width,
                                 ColumnAlign align, int padding) {
        int content_w = string_width(content);
        int total = width + padding * 2;

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
