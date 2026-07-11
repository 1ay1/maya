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
// Responsive: the table solves ONE column plan (maya::solve_columns) from the
// width it is actually given. When everything fits at natural width the
// output is exactly the classic layout; when the slot is too narrow, whole
// columns are shed lowest-`keep` first (default: rightmost goes first, the
// first column never goes) — the header, separator, and every row read the
// same plan, so they can never drift apart or shear mid-cell.
//
// Usage:
//   Table tbl({{"Name", 20}, {"Status", 10}});
//   tbl.add_row({"main.cpp", "modified"});
//   auto ui = tbl.build();
//
//   // Explicit drop order: keep Name always, shed Size before Status.
//   Table t2({{"Name"}, {"Status", 0, ColumnAlign::Left, 2},
//             {"Size", 0, ColumnAlign::Right, 1}});

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../layout/columns.hpp"
#include "../style/border.hpp"
#include "../style/style.hpp"

namespace maya {

enum class ColumnAlign : uint8_t { Left, Center, Right };

struct ColumnDef {
    std::string header;
    int width         = 0;       // 0 = auto-fit content
    ColumnAlign align = ColumnAlign::Left;
    // Drop order when the table is too narrow: LOWER sheds first;
    // kKeepAlways never sheds. 0 = auto — first column never sheds, the
    // rest shed rightmost-first.
    int keep          = 0;
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
        if (columns_.empty()) return Element{TextElement{}};

        // Width-aware: the REAL slot width picks the column plan. When the
        // natural widths fit, the plan is all-visible and the output is the
        // classic byte-identical layout; when they don't, whole columns shed
        // lowest-keep first instead of every row shearing at the right edge.
        return detail::adapt(
            [cols = columns_, rows = rows_, cfg = cfg_](int w) -> Element {
                return render_at(cols, rows, cfg, w);
            });
    }

private:
    [[nodiscard]] static Element render_at(
        const std::vector<ColumnDef>& columns,
        const std::vector<std::vector<std::string>>& rows,
        const TableConfig& cfg, int avail)
    {
        const int ncols = static_cast<int>(columns.size());

        // Natural column widths (explicit width respected, else content fit).
        std::vector<int> widths(static_cast<size_t>(ncols));
        for (int c = 0; c < ncols; ++c) {
            if (columns[static_cast<size_t>(c)].width > 0) {
                widths[static_cast<size_t>(c)] = columns[static_cast<size_t>(c)].width;
            } else {
                int max_w = string_width(columns[static_cast<size_t>(c)].header);
                for (const auto& row : rows) {
                    if (c < static_cast<int>(row.size())) {
                        max_w = std::max(max_w, string_width(row[static_cast<size_t>(c)]));
                    }
                }
                widths[static_cast<size_t>(c)] = max_w;
            }
        }

        const int pad = cfg.cell_padding;

        // ONE column plan for the header, the separator, and every row —
        // solved from the width the layout engine actually gave us. Chrome
        // (border + horizontal padding) eats 4 cells of the slot.
        const int chrome = cfg.show_border ? 4 : 0;
        std::vector<ColSpec> spec(static_cast<size_t>(ncols));
        for (int c = 0; c < ncols; ++c) {
            spec[static_cast<size_t>(c)].min = widths[static_cast<size_t>(c)] + pad * 2;
            const int k = columns[static_cast<size_t>(c)].keep;
            // 0 = auto drop order: first column is the identity — never
            // sheds; the rest shed rightmost-first.
            spec[static_cast<size_t>(c)].keep =
                k != 0 ? k : (c == 0 ? kKeepAlways : ncols - c);
        }
        ColPlan plan = solve_columns(spec, avail > 0 ? avail - chrome : 1 << 14,
                                     /*gap=*/0);

        std::vector<Element> output;
        output.push_back(build_header_row(columns, cfg, plan, widths, pad));
        output.push_back(build_separator(cfg, plan, widths, pad));
        for (size_t r = 0; r < rows.size(); ++r) {
            bool alt = cfg.stripe_rows && (r % 2 == 1);
            output.push_back(build_data_row(columns, cfg, plan, rows[r], widths, pad, alt));
        }

        auto content = dsl::v(std::move(output));

        if (cfg.show_border) {
            auto bordered = std::move(content)
                | dsl::border(BorderStyle::Round)
                | dsl::bcolor(cfg.border_color)
                | dsl::padding(0, 1, 0, 1);
            if (!cfg.title.empty()) {
                bordered = std::move(bordered)
                    | dsl::btext(" " + cfg.title + " ",
                        BorderTextPos::Top, BorderTextAlign::Start);
            }
            return std::move(bordered).build();
        }

        return content.build();
    }

    [[nodiscard]] static Element build_header_row(
        const std::vector<ColumnDef>& columns, const TableConfig& cfg,
        const ColPlan& plan, const std::vector<int>& widths, int pad)
    {
        std::string content;
        std::vector<StyledRun> runs;

        for (size_t c = 0; c < columns.size(); ++c) {
            if (!plan.has(c)) continue;
            std::string cell = pad_cell(columns[c].header, widths[c], columns[c].align, pad);
            runs.push_back(StyledRun{content.size(), cell.size(), cfg.header_style});
            content += cell;
        }

        return Element{TextElement{
            .content = std::move(content),
            .style = {},
            .wrap = TextWrap::NoWrap,
            .runs = std::move(runs),
        }};
    }

    [[nodiscard]] static Element build_data_row(
        const std::vector<ColumnDef>& columns, const TableConfig& cfg,
        const ColPlan& plan, const std::vector<std::string>& row,
        const std::vector<int>& widths, int pad, bool alt)
    {
        std::string content;
        std::vector<StyledRun> runs;
        Style style = alt ? cfg.alt_row_style : cfg.row_style;

        for (size_t c = 0; c < columns.size(); ++c) {
            if (!plan.has(c)) continue;
            std::string val = c < row.size() ? row[c] : "";
            std::string cell = pad_cell(val, widths[c], columns[c].align, pad);
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

    [[nodiscard]] static Element build_separator(
        const TableConfig& cfg, const ColPlan& plan,
        const std::vector<int>& widths, int pad)
    {
        std::string line;
        for (size_t c = 0; c < widths.size(); ++c) {
            if (!plan.has(c)) continue;
            int total = widths[c] + pad * 2;
            for (int i = 0; i < total; ++i) line += "\xe2\x94\x80";  // ─
        }
        return Element{TextElement{
            .content = std::move(line),
            .style = cfg.separator_style,
            .wrap = TextWrap::NoWrap,
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
