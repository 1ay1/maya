#pragma once
// maya::panel item widget: Hist — a distribution, drawn as columns.
//
// A picture kind: it owns its row and paints several lines. Block columns
// rather than a braille curve, because a distribution is a set of DISCRETE
// buckets and the eye should land on one at a time. A smooth line through
// them would claim the data is continuous.
//
// EVERY bucket is shown. The bar width adapts to the space instead --
// dropping the tail loses data, and the tail of a latency distribution is
// exactly where the interesting outliers are. A chart that silently omits
// its slowest bucket answers the wrong question.

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../../../dsl.hpp"
#include "../../../element/element.hpp"
#include "../../../element/text.hpp"
#include "../../../style/color.hpp"
#include "../../../style/style.hpp"
#include "../../../text/unicode_width.hpp"
#include "../context.hpp"

namespace maya::panel {

struct Hist {
    struct Bucket {
        std::string label;   // axis tick, drawn under the column
        double      value = 0;
    };
    std::vector<Bucket>      buckets;
    int                      rows = 5;   // terminal rows of column height
    int                      col_width = 4;
    std::string              caption;
    // Y-axis ticks, TOP ROW FIRST, one per row.
    std::vector<std::string> y_labels;
    std::optional<Color>     hue{};
};

[[nodiscard]] inline int rows_of(const Hist& h, const ItemCtx&) {
    if (h.buckets.empty()) return 1;
    int n = std::clamp(h.rows, 2, 16);
    // Two columns of indent, because every TEXT row has them.
    //
    // A row's leading cell opens with the marker lane -- one cell for the
    // cursor bar and one space after it -- so labels begin at column 2. A
    // picture that starts at column 0 sits two columns left of everything
    // around it, and on a tab that mixes figures with tables the result is
    // visibly ragged. The lane is empty here (a figure has no cursor to
    // mark) but the SPACE it occupies is what makes a column a column.
    static constexpr const char* kLane = "  ";

    if (!h.caption.empty()) ++n;
    ++n;                                   // the baseline rule
    for (const auto& b : h.buckets)
        if (!b.label.empty()) { ++n; break; }   // the tick row
    return n;
}

[[nodiscard]] inline std::pair<std::string, Style>
render(const Hist&, const ItemCtx& ctx) {
    return {std::string{}, Style{}.with_fg(ctx.theme.value)};
}

[[nodiscard]] inline Element render_element(const Hist& h, const ItemCtx& ctx) {
    if (h.buckets.empty()) return Element{TextElement{}};
    double hi = 0;
    for (const auto& b : h.buckets) hi = std::max(hi, b.value);
    if (hi <= 0) return Element{TextElement{}};

    const int rows = std::clamp(h.rows, 2, 16);
    const Color hue = h.hue.value_or(ctx.theme.value);
    const Color help = ctx.theme.help;

    std::vector<Element> lines;

    // Two columns of indent, because every TEXT row has them. A row's
    // leading cell opens with the marker lane -- one cell for the cursor
    // bar and one space after it -- so labels begin at column 2. A picture
    // starting at column 0 sits two columns left of everything around it.
    static constexpr const char* kLane = "  ";

    if (!h.caption.empty())
        lines.push_back(Element{TextElement{
            .content = kLane + h.caption,
            .style   = Style{}.with_fg(help),
            .wrap    = TextWrap::TruncateEnd}});

    auto buckets = h.buckets;
    auto ylabels = h.y_labels;
    const int want_w = std::max(1, h.col_width);

    lines.push_back(dsl::fill([buckets, ylabels, rows, want_w, hi, hue, help]
                              (int w, int) {
        // The y-axis gutter, reserved before anything is sized. Sized to
        // the WIDEST tick so every label right-aligns against the axis; a
        // ragged gutter reads as a second, meaningless column of text.
        int lab_w = 0;
        for (const auto& l : ylabels) lab_w = std::max(lab_w, unicode::str_width(l));
        const int gutter = lab_w > 0 ? lab_w + 1 : 0;
        const int plot_w = std::max(0, w - gutter);
        if (plot_w < 1) return Element{TextElement{}};

        const int n = static_cast<int>(buckets.size());
        int cw = std::min(want_w, std::max(1, plot_w / std::max(1, n)));
        // Bars keep a gap only while there is width to spare for one. At
        // two columns per bucket the gap is half the chart, so below that
        // the bars run together and the ticks carry the boundaries.
        const int bar_w = cw >= 3 ? cw - 1 : cw;

        std::vector<Element> out;
        for (int cy = 0; cy < rows; ++cy) {
            std::string s = "  ";
            std::vector<StyledRun> runs;
            if (gutter > 0) {
                const std::string tick =
                    cy < static_cast<int>(ylabels.size()) ? ylabels[cy] : std::string{};
                const int pad = std::max(0, gutter - unicode::str_width(tick) - 1);
                s.append(static_cast<std::size_t>(pad), ' ');
                const std::size_t at = s.size();
                s += tick + " ";
                runs.push_back({at, s.size() - at, Style{}.with_fg(help)});
            }
            const std::size_t bars_at = s.size();
            for (int i = 0; i < n; ++i) {
                // A bucket reaches this row when its value clears the row's
                // share of the peak. Top row first, so row 0 is the peak.
                const double need = static_cast<double>(rows - cy) / rows;
                const bool on = buckets[static_cast<std::size_t>(i)].value / hi >= need;
                for (int c = 0; c < bar_w; ++c) s += on ? "\xe2\x96\x88" : " ";
                for (int c = bar_w; c < cw; ++c) s += ' ';
            }
            runs.push_back({bars_at, s.size() - bars_at, Style{}.with_fg(hue)});
            out.push_back(Element{TextElement{.content = std::move(s),
                                              .wrap    = TextWrap::TruncateEnd,
                                              .runs    = std::move(runs)}});
        }

        // The baseline. A histogram floating with no axis has no zero, and
        // "short bar" then means nothing in particular.
        {
            std::string s(static_cast<std::size_t>(gutter), ' ');
            const std::size_t at = s.size();
            for (int i = 0; i < n * cw; ++i) s += "\xe2\x94\x80";   // ─
            out.push_back(Element{TextElement{
                .content = std::move(s),
                .wrap    = TextWrap::TruncateEnd,
                .runs    = {{at, static_cast<std::size_t>(n * cw) * 3,
                             Style{}.with_fg(help)}}}});
        }

        // Tick labels, at whatever stride keeps them from colliding.
        bool any = false;
        int widest = 0;
        for (const auto& b : buckets)
            if (!b.label.empty()) { any = true; widest = std::max(widest, unicode::str_width(b.label)); }
        if (any) {
            const int stride = std::max(1, (widest + 1 + cw - 1) / cw);
            std::string s(static_cast<std::size_t>(gutter), ' ');
            int col = 0;
            for (int i = 0; i < n; i += stride) {
                const int at = i * cw;
                while (col < at) { s += ' '; ++col; }
                const std::string& lb = buckets[static_cast<std::size_t>(i)].label;
                s += lb;
                col += unicode::str_width(lb);
            }
            out.push_back(Element{TextElement{
                .content = std::move(s),
                .style   = Style{}.with_fg(help),
                .wrap    = TextWrap::TruncateEnd}});
        }
        return dsl::v(std::move(out)).build();
    }).height(Dimension::fixed(rows)).build());

    return dsl::v(std::move(lines)).build();
}

} // namespace maya::panel
