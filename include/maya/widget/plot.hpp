#pragma once
// maya::Plot — a series drawn as a braille curve.
//
// A picture: it owns its row and paints several lines. 2×4 braille dots per
// cell, so a 4-row plot has 16 vertical levels and the curve keeps its shape
// instead of quantising into a staircase.
//
// Braille rather than block columns because a block column has a flat top
// edge one eighth of a row tall; at this size that reads as a bar chart of
// the samples rather than as a line through them.
//
// Usage:
//   Plot p;
//   p.series({1,3,2,5,4,6}).rows(4)
//    .peak_label("6").base_label("0").caption("tokens/turn");
//   Element ui = p.build();
//
// Standalone: sizes to whatever width the layout gives it (via fill()).

#include <algorithm>
#include <cstdint>
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

class Plot {
public:
    Plot() = default;
    explicit Plot(std::vector<double> s) : series_(std::move(s)) {}

    Plot& series(std::vector<double> s) { series_ = std::move(s); return *this; }
    Plot& push(double v)                { series_.push_back(v); return *this; }
    Plot& rows(int r)                   { rows_ = r; return *this; }
    Plot& caption(std::string c)        { caption_ = std::move(c); return *this; }
    Plot& peak_label(std::string s)     { peak_label_ = std::move(s); return *this; }
    Plot& base_label(std::string s)     { base_label_ = std::move(s); return *this; }
    Plot& filled(bool f)                { filled_ = f; return *this; }
    Plot& line_color(Color c)           { line_color_ = c; return *this; }
    Plot& label_color(Color c)          { label_color_ = c; return *this; }

    operator Element() const { return build(); }

    // Rows this figure paints (curve + optional caption).
    [[nodiscard]] int height() const {
        if (series_.empty()) return 1;
        const int r = std::clamp(rows_, 1, 16);
        return caption_.empty() ? r : r + 1;
    }

    [[nodiscard]] Element build() const {
        if (series_.empty()) return Element{TextElement{}};

        const int rows = std::clamp(rows_, 1, 16);
        const Color hue = line_color_;

        std::vector<Element> lines;

        if (!caption_.empty())
            lines.push_back(Element{TextElement{
                .content = caption_,
                .style   = Style{}.with_fg(label_color_),
                .wrap    = TextWrap::TruncateEnd}});

        // The scale gutter is reserved before the curve is sized: a plot that
        // overruns its own labels is worse than one two columns narrower.
        const int lab_w = std::max(unicode::str_width(peak_label_),
                                   unicode::str_width(base_label_));
        const int gutter = lab_w > 0 ? lab_w + 1 : 0;

        auto series = series_;
        const std::string peak = peak_label_;
        const std::string base = base_label_;
        const Color help = label_color_;

        lines.push_back(dsl::fill([series, rows, hue, gutter, peak, base, help,
                                   filled = filled_]
                                  (int w, int) {
            const int cells = std::max(0, w - gutter);
            if (cells <= 0) return Element{TextElement{}};

            const int dot_w = cells * 2;
            const int dot_h = rows * 4;
            std::vector<std::uint8_t> grid(
                static_cast<std::size_t>(rows) * static_cast<std::size_t>(cells), 0);

            // Braille dot bit per (row, col) within a cell: 2 wide, 4 tall.
            static constexpr std::uint8_t kDot[4][2] = {
                {0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80},
            };
            auto plot_dot = [&](int x, int y) {
                if (x < 0 || x >= dot_w || y < 0 || y >= dot_h) return;
                grid[static_cast<std::size_t>(y / 4)
                         * static_cast<std::size_t>(cells)
                     + static_cast<std::size_t>(x / 2)] |= kDot[y % 4][x % 2];
            };

            double hi = 0.0;
            for (double v : series) hi = std::max(hi, v);
            if (hi <= 0.0) hi = 1.0;

            // One column of dots per x, sampling the series across the width.
            // Sampled rather than interpolated: a plot of 400 turns in 60 cells
            // is a summary, and inventing points between real ones would draw a
            // curve the data does not have.
            const std::size_t n = series.size();
            int prev_y = -1;
            for (int x = 0; x < dot_w; ++x) {
                // MAX over the samples this dot column covers, not a point
                // sample. When 400 turns share 120 dot columns, a spike that
                // survives to the screen is the honest reduction — point
                // sampling drops whichever samples fall between the picks,
                // and the one it drops is as likely as not the outlier the
                // reader opened the tab to find.
                const std::size_t lo = n * static_cast<std::size_t>(x)
                                     / static_cast<std::size_t>(dot_w);
                std::size_t hi_i = n * static_cast<std::size_t>(x + 1)
                                 / static_cast<std::size_t>(dot_w);
                if (hi_i <= lo) hi_i = lo + 1;
                double peak_v = 0;
                for (std::size_t i = lo; i < hi_i && i < n; ++i)
                    peak_v = std::max(peak_v, series[i]);

                const double t = std::clamp(peak_v / hi, 0.0, 1.0);
                const int y = dot_h - 1 - static_cast<int>(t * (dot_h - 1) + 0.5);
                plot_dot(x, y);
                // Join to the previous sample so the line is continuous. A
                // curve drawn as isolated dots reads as scatter, and the
                // question a trend answers is about direction.
                int lo_y = y, hi_y = y;
                if (prev_y >= 0) {
                    lo_y = std::min(prev_y, y);
                    hi_y = std::max(prev_y, y);
                    for (int yy = lo_y; yy <= hi_y; ++yy) plot_dot(x, yy);
                }
                // The fill: a sparse stipple below the curve, one dot in four.
                // Solid was the first attempt and it produced a slab where the
                // SURFACE — the one thing a reader is looking for — vanished
                // into the mass; at one-in-two the texture still competed with
                // the line. One in four reads as shading and lets the curve
                // read as the curve.
                if (filled)
                    for (int yy = hi_y + 1; yy < dot_h; ++yy)
                        if ((x % 2) == 0 && (yy % 2) == 0) plot_dot(x, yy);
                prev_y = y;
            }

            std::vector<Element> rowels;
            for (int cy = 0; cy < rows; ++cy) {
                std::string line;
                // The gutter's tick, right-aligned so the numbers line up on
                // their last digit.
                std::string tick;
                if (cy == 0)            tick = peak;
                else if (cy == rows - 1) tick = base;
                std::string pad(static_cast<std::size_t>(
                    std::max(0, gutter - unicode::str_width(tick) - (gutter ? 1 : 0))), ' ');
                std::string gut = gutter ? pad + tick + " " : std::string{};

                for (int cx = 0; cx < cells; ++cx) {
                    const std::uint8_t bits =
                        grid[static_cast<std::size_t>(cy)
                                 * static_cast<std::size_t>(cells)
                             + static_cast<std::size_t>(cx)];
                    const char32_t cp = 0x2800u + bits;
                    line += static_cast<char>(0xE0 | (cp >> 12));
                    line += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    line += static_cast<char>(0x80 | (cp & 0x3F));
                }

                std::vector<StyledRun> runs;
                if (!gut.empty())
                    runs.push_back({0, gut.size(), Style{}.with_fg(help)});
                runs.push_back({gut.size(), line.size(), Style{}.with_fg(hue)});
                rowels.push_back(Element{TextElement{
                    .content = gut + line,
                    .wrap    = TextWrap::TruncateEnd,
                    .runs    = std::move(runs)}});
            }
            return dsl::v(std::move(rowels)).build();
        }).height(Dimension::fixed(rows)).build());

        return dsl::v(std::move(lines)).build();
    }

private:
    std::vector<double> series_;
    int                 rows_ = 4;   // terminal rows -> 4x dot rows
    std::string         caption_;
    std::string         peak_label_;  // scale tick against the top row
    std::string         base_label_;  // scale tick against the bottom row
    bool                filled_ = true;
    Color               line_color_  = Color::hex(0xCDD6F4);
    Color               label_color_ = Color::bright_black();
};

} // namespace maya
