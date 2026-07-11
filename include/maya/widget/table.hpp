#pragma once
// maya::widget::table — the data table.
//
// Zed's bordered card presentation + Claude Code's clean data display +
// htop's working set: selection, keyboard navigation, height-aware
// windowing, a scrollbar, sort indicators, weighted columns — all
// MEASURED, none of it estimated.
//
//   ╭─ Processes ────────────────────────────────╮
//   │   PID  NAME             CPU ▾   MEM     S  │
//   │  ──────────────────────────────────────────│
//   │ ▎ 937  rb               16.4%   6.3M    ●  │
//   │   793  agentty           8.2%   226M    ·  █
//   │   161  kitty             4.1%   188M    ·  │
//   ╰────────────────────────────────────────────╯
//
// RESPONSIVE (width). One column plan (maya::solve_columns) is solved
// from the width the table is actually given and feeds the header, the
// separator, and every row — they can never drift apart or shear
// mid-cell. When everything fits at natural width the output is the
// classic layout; when the slot is too narrow, whole columns shed
// lowest-`keep` first (default: rightmost first, the first column
// never). A column with `weight > 0` is FLEXIBLE: it shrinks toward
// `min_width` (cells truncate with …) and absorbs surplus when wide.
//
// RESPONSIVE (height). The table's natural height is all of its rows —
// in an auto-height context nothing is windowed and nothing changes.
// In a definite slot that is SHORTER than natural (or with
// `visible_rows` set), the body windows around the cursor and a
// scrollbar gutter appears at the right edge. Fill a pane with
// `tbl.build() | grow(1)` and the row count falls out of the layout.
//
// INTERACTIVE. `selectable = true` adds a cursor: a ▎ bar + row
// highlight, ↑↓/j/k, PgUp/PgDn, Home/End/g/G, Enter → on_activate.
// For mouse, set `row_hit_kind` / `header_hit_kind` and every row /
// header cell registers a maya::hit_id(kind, index) rect at paint —
// resolve clicks with maya::hit_test, zero coordinate math (the same
// contract as any hit-tagged chrome).
//
// SORT. The table doesn't sort your data (the host owns the data and
// the comparator); it SHOWS the sort: `sort_col` + `sort_desc` render
// a ▾/▴ arrow and an accent on that header. Wire header clicks (via
// header_hit_kind) or a key to your sort state and re-fill the rows.
//
// Usage — static card (unchanged classic API):
//   Table tbl({{"Name", 20}, {"Status", 10}});
//   tbl.add_row({"main.cpp", "modified"});
//   auto ui = tbl.build();
//
// Usage — the rockbottom-style proc list:
//   TableConfig cfg;
//   cfg.selectable  = true;
//   cfg.sort_col    = 2;             // CPU
//   cfg.sort_desc   = true;
//   cfg.row_hit_kind    = HK_ProcRow;    // clicks resolve via hit_test
//   cfg.header_hit_kind = HK_SortCol;
//   Table tbl({{"PID", 0, ColumnAlign::Right},
//              {"NAME", 0, ColumnAlign::Left, 0, /*weight=*/1.0f, 12},
//              {"CPU", 0, ColumnAlign::Right},
//              {"MEM", 0, ColumnAlign::Right, 2},
//              {"S",   0, ColumnAlign::Left,  1}}, cfg);
//   tbl.set_rows(rows_sorted_by_host);
//   auto ui = tbl.build() | grow(1);     // rows = whatever fits the pane

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../core/focus.hpp"
#include "../core/hit.hpp"
#include "../core/overload.hpp"
#include "../core/signal.hpp"
#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../layout/columns.hpp"
#include "../style/border.hpp"
#include "../style/style.hpp"
#include "../terminal/input.hpp"

namespace maya {

enum class ColumnAlign : uint8_t { Left, Center, Right };

struct ColumnDef {
    std::string header;
    int width         = 0;       // fixed width; 0 = auto-fit content
    ColumnAlign align = ColumnAlign::Left;
    // Drop order when the table is too narrow: LOWER sheds first;
    // kKeepAlways never sheds. 0 = auto — first column never sheds, the
    // rest shed rightmost-first.
    int keep          = 0;
    // > 0 makes the column FLEXIBLE: it takes `weight`'s share of any
    // surplus width (growing past natural) and may shrink below natural
    // toward `min_width` when the table is tight — cells truncate with …
    float weight      = 0.0f;
    // Shrink floor for a weighted column (cells; 0 = a small default).
    int min_width     = 0;
};

struct TableConfig {
    Style header_style    = Style{}.with_bold();
    Style row_style       = Style{};
    Style alt_row_style   = Style{}.with_dim();
    Style separator_style = Style{}.with_fg(Color::bright_black());
    bool stripe_rows      = true;
    int cell_padding      = 1;
    bool show_border      = false;
    std::string title;            // shown in border text when bordered
    Color border_color    = Color::bright_black();

    // ── Selection (the htop half) ──
    bool selectable        = false;             // cursor + keyboard nav
    Style selected_style   = Style{}.with_bold();
    Color cursor_bar_color = Color::blue();     // the ▎ edge bar
    std::string cursor_glyph = "\xe2\x96\x8e";  // ▎

    // ── Sort indicator (host owns the actual sort) ──
    int  sort_col  = -1;                        // -1 = none
    bool sort_desc = true;                      // ▾ vs ▴
    Style sort_header_style = Style{}.with_bold().with_fg(Color::blue());

    // ── Windowing ──
    int  visible_rows   = 0;      // fixed body height; 0 = fill the slot
    bool show_scrollbar = true;   // gutter appears only when windowed
    bool show_status    = false;  // "cursor/total" footer row

    // ── Mouse (hit registry) ──
    // 0 = off. Rows register hit_id(row_hit_kind, data_row_index);
    // header cells register hit_id(header_hit_kind, column_index).
    std::uint32_t row_hit_kind    = 0;
    std::uint32_t header_hit_kind = 0;
};

class Table {
    // All render inputs live in one value type so the non-interactive
    // build path can SNAPSHOT them into the component lambda — a Table
    // temporary (dsl::v(Table({…}))) must not dangle when the deferred
    // render runs after the temporary died.
    struct Data {
        std::vector<ColumnDef> columns;
        std::vector<std::vector<std::string>> rows;
        TableConfig cfg;
    };
    Data d_;

    Signal<int> cursor_{0};
    FocusNode focus_;
    std::move_only_function<void(int)> on_change_;
    std::move_only_function<void(int)> on_activate_;
    // Body rows in the last render — read by PgUp/PgDn. Shared with the
    // deferred render lambda so build() never has to capture `this`:
    // a Table built fresh every view() (the TEA idiom) stays safe even
    // though the Element outlives the temporary.
    std::shared_ptr<int> page_ = std::make_shared<int>(10);

public:
    explicit Table(std::vector<ColumnDef> columns)
        : d_{std::move(columns), {}, {}} {}

    Table(std::vector<ColumnDef> columns, TableConfig cfg)
        : d_{std::move(columns), {}, std::move(cfg)} {}

    // ── Data ──
    void set_rows(std::vector<std::vector<std::string>> rows) {
        d_.rows = std::move(rows);
        clamp_cursor();
    }
    void add_row(std::vector<std::string> row) { d_.rows.push_back(std::move(row)); }
    void clear_rows() { d_.rows.clear(); cursor_.set(0); }
    [[nodiscard]] int row_count() const { return static_cast<int>(d_.rows.size()); }

    // ── Config ──
    void set_title(std::string_view t) { d_.cfg.title = std::string{t}; }
    void set_bordered(bool b) { d_.cfg.show_border = b; }
    void set_sort(int col, bool desc) { d_.cfg.sort_col = col; d_.cfg.sort_desc = desc; }
    [[nodiscard]] TableConfig& config() { return d_.cfg; }
    [[nodiscard]] const TableConfig& config() const { return d_.cfg; }

    // ── Selection ──
    [[nodiscard]] int selected() const { return cursor_(); }
    void set_selected(int idx) {
        cursor_.set(std::clamp(idx, 0, std::max(0, row_count() - 1)));
        if (on_change_) on_change_(cursor_());
    }
    [[nodiscard]] FocusNode& focus_node() { return focus_; }
    [[nodiscard]] const FocusNode& focus_node() const { return focus_; }

    template <std::invocable<int> F>
    void on_change(F&& fn) { on_change_ = std::forward<F>(fn); }
    template <std::invocable<int> F>
    void on_activate(F&& fn) { on_activate_ = std::forward<F>(fn); }

    // ── Keyboard: ↑↓/j/k · PgUp/PgDn · Home/End/g/G · Enter ──
    [[nodiscard]] bool handle(const KeyEvent& ev) {
        if (!d_.cfg.selectable || d_.rows.empty()) return false;
        const int n = row_count();
        auto move = [&](int delta) {
            int next = std::clamp(cursor_() + delta, 0, n - 1);
            if (next != cursor_()) {
                cursor_.set(next);
                if (on_change_) on_change_(next);
            }
            return true;
        };
        return std::visit(overload{
            [&](SpecialKey sk) -> bool {
                switch (sk) {
                    case SpecialKey::Up:       return move(-1);
                    case SpecialKey::Down:     return move(+1);
                    case SpecialKey::PageUp:   return move(-std::max(1, *page_));
                    case SpecialKey::PageDown: return move(+std::max(1, *page_));
                    case SpecialKey::Home:     return move(-n);
                    case SpecialKey::End:      return move(+n);
                    case SpecialKey::Enter:
                        if (on_activate_) on_activate_(cursor_());
                        return true;
                    default: return false;
                }
            },
            [&](CharKey ck) -> bool {
                switch (ck.codepoint) {
                    case 'k': return move(-1);
                    case 'j': return move(+1);
                    case 'g': return move(-n);
                    case 'G': return move(+n);
                    default:  return false;
                }
            },
            [](auto&&) -> bool { return false; },
        }, ev.key);
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        if (d_.columns.empty()) return Element{TextElement{}};

        // Height- AND width-aware: render at the REAL allocated size.
        // The measure callback reports the table's full natural size, so
        // an auto-height context renders everything (classic layout,
        // unchanged); a definite SHORTER slot flex-shrinks the
        // allocation and render_at windows the body around the cursor.
        //
        // Everything is captured by VALUE (data snapshot + cursor value
        // + a shared page counter), never `this` — so both long-lived
        // interactive Tables and per-frame TEA temporaries
        // (dsl::v(Table({…}))) are safe when the deferred render runs.
        auto b = detail::component(
            [d = d_, cur = cursor_(), page = page_](int w, int h) -> Element {
                return render_at(d, cur, page.get(), w, h);
            });
        b.measure([d = d_](int max_width) -> Size {
            return natural_size(d, max_width);
        });
        return b;
    }

private:
    // Chrome the border adds around the content grid.
    [[nodiscard]] static int chrome_w(const Data& d) { return d.cfg.show_border ? 4 : 0; }
    [[nodiscard]] static int chrome_h(const Data& d) { return d.cfg.show_border ? 2 : 0; }

    void clamp_cursor() {
        cursor_.set(std::clamp(cursor_(), 0, std::max(0, row_count() - 1)));
    }

    // Natural (unwindowed) content widths per column, including the sort
    // arrow on the sorted column.
    [[nodiscard]] static std::vector<int> natural_widths(const Data& d) {
        const int ncols = static_cast<int>(d.columns.size());
        std::vector<int> widths(static_cast<size_t>(ncols));
        for (int c = 0; c < ncols; ++c) {
            const auto& col = d.columns[static_cast<size_t>(c)];
            if (col.width > 0) {
                widths[static_cast<size_t>(c)] = col.width;
                continue;
            }
            int max_w = string_width(col.header) + (c == d.cfg.sort_col ? 2 : 0);
            for (const auto& row : d.rows) {
                if (c < static_cast<int>(row.size())) {
                    max_w = std::max(max_w, string_width(row[static_cast<size_t>(c)]));
                }
            }
            widths[static_cast<size_t>(c)] = max_w;
        }
        return widths;
    }

    [[nodiscard]] static std::vector<ColSpec> specs(const Data& d,
                                                    const std::vector<int>& widths) {
        const int ncols = static_cast<int>(d.columns.size());
        const int pad2 = d.cfg.cell_padding * 2;
        std::vector<ColSpec> spec(static_cast<size_t>(ncols));
        for (int c = 0; c < ncols; ++c) {
            const auto& col = d.columns[static_cast<size_t>(c)];
            auto& s = spec[static_cast<size_t>(c)];
            if (col.weight > 0.0f) {
                // Flexible: floor at min_width (default 8, never wider
                // than natural), grow unbounded by weight share.
                const int floor_w = std::min(
                    widths[static_cast<size_t>(c)],
                    std::max(col.min_width > 0 ? col.min_width : 8,
                             string_width(col.header)));
                s.min    = floor_w + pad2;
                s.max    = 0;
                s.weight = col.weight;
            } else {
                s.min = widths[static_cast<size_t>(c)] + pad2;
            }
            const int k = col.keep;
            s.keep = k != 0 ? k
                   : (c == 0 ? kKeepAlways : ncols - c);
        }
        return spec;
    }

    [[nodiscard]] static Size natural_size(const Data& d, int max_width) {
        // WIDTH: claim the full offered width (fill semantics) — a data
        // table spans its pane like htop's, so weighted columns have
        // surplus to absorb and the plan re-solves on every resize. The
        // natural sum only caps the claim when the offer is unbounded.
        auto widths = natural_widths(d);
        long long nat = d.cfg.selectable ? 2 : 0;   // cursor gutter "▎ "
        for (size_t c = 0; c < widths.size(); ++c)
            nat += widths[c] + d.cfg.cell_padding * 2;
        nat += chrome_w(d);
        long long w = (max_width > 0 && max_width < (1 << 14))
            ? max_width
            : nat;
        const int nrows = static_cast<int>(d.rows.size());
        const int body = d.cfg.visible_rows > 0
            ? std::min(d.cfg.visible_rows, nrows)
            : nrows;
        long long hh = 2 + body + (d.cfg.show_status ? 1 : 0) + chrome_h(d);
        return {Columns{static_cast<int>(w)}, Rows{static_cast<int>(hh)}};
    }

    [[nodiscard]] static Element render_at(const Data& d, int cursor,
                                           int* last_page, int w, int h) {
        const int pad   = d.cfg.cell_padding;
        const int n     = static_cast<int>(d.rows.size());
        const int cur   = std::clamp(cursor, 0, std::max(0, n - 1));

        // ── Window: how many body rows does the slot afford? ──
        const int chrome_rows = 2 + (d.cfg.show_status ? 1 : 0) + chrome_h(d);
        int count = n;
        if (d.cfg.visible_rows > 0) count = std::min(d.cfg.visible_rows, n);
        if (h > 0) count = std::min(count, std::max(1, h - chrome_rows));
        const bool windowed = count < n;
        if (last_page) *last_page = std::max(1, count);
        int start = 0;
        if (windowed) {
            start = d.cfg.selectable
                ? std::clamp(cur - count / 2, 0, n - count)
                : 0;
        }

        // ── One column plan for everything below ──
        const int gut = d.cfg.selectable ? 2 : 0;                    // "▎ "
        const int bar = (windowed && d.cfg.show_scrollbar) ? 2 : 0;  // " ▐"
        const int avail = w > 0 ? w - chrome_w(d) - gut - bar : 1 << 14;
        auto widths = natural_widths(d);
        ColPlan plan = solve_columns(specs(d, widths), std::max(1, avail), 0);

        // Solved CONTENT width per visible column (minus padding).
        auto content_w = [&](size_t c) {
            return std::max(1, plan.at(c) - pad * 2);
        };

        // ── Scrollbar thumb ──
        int thumb_start = 0, thumb_len = 0;
        if (bar > 0) {
            thumb_len = std::max(1, count * count / std::max(1, n));
            thumb_start = (n - count) > 0
                ? (start * (count - thumb_len) + (n - count) / 2) / (n - count)
                : 0;
        }

        std::vector<Element> output;
        output.reserve(static_cast<size_t>(count) + 3);
        output.push_back(build_header(d, plan, content_w, gut, bar, pad));
        output.push_back(build_separator(d, plan, gut, bar));

        for (int i = start; i < start + count; ++i) {
            const bool on = d.cfg.selectable && i == cur;
            const bool alt = d.cfg.stripe_rows && !on && (i % 2 == 1);
            const char* bar_g = bar > 0
                ? ((i - start >= thumb_start && i - start < thumb_start + thumb_len)
                       ? "\xe2\x96\x90"    // ▐ thumb
                       : "\xe2\x94\x82")   // │ track
                : "";
            Element row = build_row(d, d.rows[static_cast<size_t>(i)], plan,
                                    content_w, gut, pad, on, alt, bar_g);
            if (d.cfg.row_hit_kind != 0)
                row = std::move(row)
                    | dsl::hit(hit_id(d.cfg.row_hit_kind,
                                      static_cast<std::uint32_t>(i)));
            output.push_back(std::move(row));
        }

        if (d.cfg.show_status) {
            std::string status = d.cfg.selectable
                ? std::to_string(cur + 1) + "/" + std::to_string(n)
                : std::to_string(n) + " rows";
            if (windowed) status += "  \xc2\xb7  \xe2\x86\x91\xe2\x86\x93";  // · ↑↓
            output.push_back(Element{TextElement{
                .content = std::string(static_cast<size_t>(gut), ' ')
                           + std::string(static_cast<size_t>(pad), ' ') + status,
                .style = Style{}.with_dim(),
                .wrap = TextWrap::NoWrap,
            }});
        }

        auto content = dsl::v(std::move(output));

        if (d.cfg.show_border) {
            auto bordered = std::move(content)
                | dsl::border(BorderStyle::Round)
                | dsl::bcolor(d.cfg.border_color)
                | dsl::padding(0, 1, 0, 1);
            if (!d.cfg.title.empty()) {
                bordered = std::move(bordered)
                    | dsl::btext(" " + d.cfg.title + " ",
                        BorderTextPos::Top, BorderTextAlign::Start);
            }
            return std::move(bordered).build();
        }
        return content.build();
    }

    template <typename ContentW>
    [[nodiscard]] static Element build_header(const Data& d, const ColPlan& plan,
                                              ContentW&& content_w,
                                              int gut, int bar, int pad) {
        // With header hits each cell is its own element so every column
        // gets its own click rect; otherwise one TextElement (cheaper).
        const bool per_cell = d.cfg.header_hit_kind != 0;
        std::vector<Element> cells;
        std::string content(static_cast<size_t>(gut), ' ');
        std::vector<StyledRun> runs;
        if (per_cell && gut > 0)
            cells.push_back(Element{TextElement{
                .content = std::string(static_cast<size_t>(gut), ' '),
                .wrap = TextWrap::NoWrap}});

        for (size_t c = 0; c < d.columns.size(); ++c) {
            if (!plan.has(c)) continue;
            std::string label = d.columns[c].header;
            if (static_cast<int>(c) == d.cfg.sort_col)
                label += d.cfg.sort_desc ? " \xe2\x96\xbe" : " \xe2\x96\xb4";  // ▾ ▴
            const Style st = static_cast<int>(c) == d.cfg.sort_col
                ? d.cfg.sort_header_style : d.cfg.header_style;
            std::string cell = pad_cell(label, content_w(c), d.columns[c].align, pad);
            if (per_cell) {
                const std::size_t cell_len = cell.size();
                Element el{TextElement{
                    .content = std::move(cell),
                    .style = {},
                    .wrap = TextWrap::NoWrap,
                    .runs = {StyledRun{0, cell_len, st}},
                }};
                cells.push_back(std::move(el)
                    | dsl::hit(hit_id(d.cfg.header_hit_kind,
                                      static_cast<std::uint32_t>(c))));
            } else {
                runs.push_back(StyledRun{content.size(), cell.size(), st});
                content += cell;
            }
        }
        if (per_cell) {
            return dsl::h(std::move(cells)).build();
        }
        content.append(static_cast<size_t>(bar), ' ');
        return Element{TextElement{
            .content = std::move(content),
            .style = {},
            .wrap = TextWrap::NoWrap,
            .runs = std::move(runs),
        }};
    }

    template <typename ContentW>
    [[nodiscard]] static Element build_row(const Data& d,
                                    const std::vector<std::string>& row,
                                    const ColPlan& plan, ContentW&& content_w,
                                    int gut, int pad, bool selected, bool alt,
                                    const char* bar_glyph) {
        std::string content;
        std::vector<StyledRun> runs;
        const Style style = selected ? d.cfg.selected_style
                          : alt      ? d.cfg.alt_row_style
                                     : d.cfg.row_style;
        if (gut > 0) {
            if (selected) {
                runs.push_back(StyledRun{0, d.cfg.cursor_glyph.size(),
                    Style{}.with_fg(d.cfg.cursor_bar_color).with_bold()});
                content = d.cfg.cursor_glyph + " ";
            } else {
                content = "  ";
            }
        }
        for (size_t c = 0; c < d.columns.size(); ++c) {
            if (!plan.has(c)) continue;
            std::string val = c < row.size() ? row[c] : "";
            std::string cell = pad_cell(val, content_w(c), d.columns[c].align, pad);
            runs.push_back(StyledRun{content.size(), cell.size(), style});
            content += cell;
        }
        if (bar_glyph[0] != '\0') {
            std::string g = std::string(" ") + bar_glyph;
            runs.push_back(StyledRun{content.size(), g.size(),
                Style{}.with_fg(Color::bright_black())});
            content += g;
        }
        return Element{TextElement{
            .content = std::move(content),
            .style = {},
            .wrap = TextWrap::NoWrap,
            .runs = std::move(runs),
        }};
    }

    [[nodiscard]] static Element build_separator(const Data& d, const ColPlan& plan,
                                                 int gut, int bar) {
        std::string line(static_cast<size_t>(gut), ' ');
        for (size_t c = 0; c < d.columns.size(); ++c) {
            if (!plan.has(c)) continue;
            const int total = std::max(1, plan.at(c));
            for (int i = 0; i < total; ++i) line += "\xe2\x94\x80";  // ─
        }
        line.append(static_cast<size_t>(bar), ' ');
        return Element{TextElement{
            .content = std::move(line),
            .style = d.cfg.separator_style,
            .wrap = TextWrap::NoWrap,
        }};
    }

    // Pad (and now TRUNCATE) a cell to exactly `width` content cells.
    static std::string pad_cell(const std::string& content, int width,
                                 ColumnAlign align, int padding) {
        std::string body = content;
        int content_w = string_width(body);
        if (content_w > width) {
            body = truncate_end(body, width);
            content_w = string_width(body);
        }
        const int total = width + padding * 2;

        std::string result;
        result.reserve(static_cast<size_t>(total) + body.size());

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
        result += body;
        result.append(static_cast<size_t>(right_pad), ' ');

        return result;
    }
};

} // namespace maya
