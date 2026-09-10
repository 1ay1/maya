#pragma once
// maya::panel item widget: Plot — a series drawn as a braille curve.
//
// A picture kind, like Band: it owns its row and paints several lines. The
// difference from Spark is resolution, and it is the reason both exist. A
// Spark is one cell per sample on a single line — a shape you read at a
// glance beside a number. A Plot is a figure: 2×4 braille dots per cell, so
// a 4-row plot has 16 vertical levels against a sparkline's 8, and the
// curve keeps its shape instead of quantising into a staircase.
//
// Braille rather than block columns because a block column has a flat top
// edge one eighth of a row tall. At this size that reads as a bar chart of
// the samples rather than as a line through them.

#include <algorithm>
#include <cstdint>
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

struct Plot {
    std::vector<double> series;
    // Terminal rows the figure occupies, so 4x this many dot rows.
    int                 rows = 4;
    std::string         caption;
    // Scale ticks: the peak against the top row, the base against the
    // bottom. A curve with no scale is a shape with no magnitude.
    std::string         peak_label;
    std::string         base_label;
    std::optional<Color> hue{};
};

[[nodiscard]] inline int rows_of(const Plot& p, const ItemCtx&) {
    if (p.series.empty()) return 1;
    const int r = std::clamp(p.rows, 1, 16);
    return p.caption.empty() ? r : r + 1;
}

[[nodiscard]] inline std::pair<std::string, Style>
render(const Plot&, const ItemCtx& ctx) {
    // Pictures answer through render_element; this exists so every kind
    // satisfies the string overload.
    return {std::string{}, Style{}.with_fg(ctx.theme.value)};
}

[[nodiscard]] inline Element render_element(const Plot& p, const ItemCtx& ctx) {
    if (p.series.empty()) return Element{TextElement{}};

    const int rows = std::clamp(p.rows, 1, 16);
    const Color hue = p.hue.value_or(ctx.theme.value);

    std::vector<Element> lines;
    if (!p.caption.empty())
        lines.push_back(Element{TextElement{
            .content = p.caption,
            .style   = Style{}.with_fg(ctx.theme.help),
            .wrap    = TextWrap::TruncateEnd}});

    // The scale gutter is reserved before the curve is sized: a plot that
    // overruns its own labels is worse than one two columns narrower.
    const int lab_w = std::max(unicode::str_width(p.peak_label),
                               unicode::str_width(p.base_label));
    const int gutter = lab_w > 0 ? lab_w + 1 : 0;

    auto series = p.series;
    const std::string peak = p.peak_label;
    const std::string base = p.base_label;
    const Color help = ctx.theme.help;

    lines.push_back(dsl::fill([series, rows, hue, gutter, peak, base, help]
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
            const std::size_t idx = std::min(
                n - 1, static_cast<std::size_t>(
                           static_cast<double>(x) / dot_w * n));
            const double t = std::clamp(series[idx] / hi, 0.0, 1.0);
            const int y = dot_h - 1 - static_cast<int>(t * (dot_h - 1) + 0.5);
            plot_dot(x, y);
            // Join to the previous sample so the line is continuous. A
            // curve drawn as isolated dots reads as scatter, and the
            // question a trend answers is about direction.
            if (prev_y >= 0)
                for (int yy = std::min(prev_y, y); yy <= std::max(prev_y, y); ++yy)
                    plot_dot(x, yy);
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

} // namespace maya::panel
