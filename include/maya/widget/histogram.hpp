#pragma once
// maya::Histogram — a distribution drawn as block columns.
//
// A picture: it owns its row and paints several lines. Block columns rather
// than a braille curve, because a distribution is a set of DISCRETE buckets
// and the eye should land on one at a time. A smooth line through them would
// claim the data is continuous.
//
// EVERY bucket is shown. The bar width adapts to the space instead --
// dropping the tail loses data, and the tail of a latency distribution is
// exactly where the interesting outliers are.
//
// Usage:
//   Histogram h;
//   h.bucket("0", 3).bucket("1", 9).bucket("2", 5)
//    .rows(5).caption("latency");
//   Element ui = h.build();
//
// Standalone: sizes to whatever width the layout gives it (via fill()).

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"
#include "../style/theme.hpp"
#include "../text/unicode_width.hpp"

namespace maya {

class Histogram {
public:
    struct Bucket {
        std::string label;   // axis tick, drawn under the column
        double      value = 0;
    };

    Histogram() = default;
    explicit Histogram(std::vector<Bucket> bks) : buckets_(std::move(bks)) {}

    Histogram& bucket(std::string label, double value) {
        buckets_.push_back({std::move(label), value});
        return *this;
    }
    Histogram& rows(int r)             { rows_ = r; return *this; }
    Histogram& col_width(int w)        { col_width_ = w; return *this; }
    Histogram& caption(std::string c)  { caption_ = std::move(c); return *this; }
    Histogram& y_labels(std::vector<std::string> y) { y_labels_ = std::move(y); return *this; }
    Histogram& bar_color(Color c)      { bar_color_ = c; return *this; }
    Histogram& label_color(Color c)    { label_color_ = c; return *this; }

    operator Element() const { return build(); }

    // Rows this figure paints (columns + baseline + optional caption/ticks).
    [[nodiscard]] int height() const {
        if (buckets_.empty()) return 1;
        int n = std::clamp(rows_, 2, 16);
        if (!caption_.empty()) ++n;
        ++n;                                   // the baseline rule
        for (const auto& b : buckets_)
            if (!b.label.empty()) { ++n; break; }   // the tick row
        return n;
    }

    [[nodiscard]] Element build() const {
        if (buckets_.empty()) return Element{TextElement{}};
        double hi = 0;
        for (const auto& b : buckets_) hi = std::max(hi, b.value);
        if (hi <= 0) return Element{TextElement{}};

        const int rows = std::clamp(rows_, 2, 16);
        const Color hue  = bar_color_;
        const Color help = label_color_;

        std::vector<Element> lines;

        if (!caption_.empty())
            lines.push_back(Element{TextElement{
                .content = caption_,
                .style   = Style{}.with_fg(help),
                .wrap    = TextWrap::TruncateEnd}});

        auto buckets = buckets_;
        auto ylabels = y_labels_;
        const int want_w = std::max(1, col_width_);
        // Whether a tick row will be emitted — needed for the box's height
        // before the render lambda runs.
        bool has_ticks = false;
        for (const auto& b : buckets_)
            if (!b.label.empty()) { has_ticks = true; break; }

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
            // The bar width follows the SPACE, in both directions.
            //
            // `col_width` is a MINIMUM, not a fixed size. Taking min() alone
            // only ever narrowed it, so a nine-bucket spread drew the same
            // 2-cell bars on a 150-column pane that it draws on a 60 — the
            // comparison the chart exists for, made harder than the width
            // requires. Capped so a three-bucket histogram on a very wide
            // terminal does not become three slabs: past a point extra width
            // stops aiding comparison and just spreads the data out.
            constexpr int kMaxCol = 8;
            const int afford = std::max(1, plot_w / std::max(1, n));
            int cw = std::clamp(afford, 1, std::max(want_w, kMaxCol));
            // Bars keep a gap only while there is width to spare for one. At
            // two columns per bucket the gap is half the chart, so below that
            // the bars run together and the ticks carry the boundaries.
            const int bar_w = cw >= 3 ? cw - 1 : cw;

            std::vector<Element> out;
            for (int cy = 0; cy < rows; ++cy) {
                std::string s;
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
        // The fixed height has to cover the AXIS, not just the bars.
        //
        // `rows` is the column height alone; the element also emits a
        // baseline rule and, when the buckets are labelled, a tick row.
        }).height(Dimension::fixed(rows + 1 + (has_ticks ? 1 : 0))).build());

        return dsl::v(std::move(lines)).build();
    }

private:
    std::vector<Bucket>      buckets_;
    int                      rows_ = 5;   // terminal rows of column height
    int                      col_width_ = 4;
    std::string              caption_;
    std::vector<std::string> y_labels_;   // top row first, one per row
    Color                    bar_color_   = Color::hex(0xCDD6F4);
    Color                    label_color_ = Color::bright_black();
};

} // namespace maya
