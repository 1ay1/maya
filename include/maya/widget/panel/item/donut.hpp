#pragma once
// maya::panel item widget: Donut — a composition, drawn as a ring.
//
// The same fact a Band shows, and worth having alongside it because the two
// fail in opposite directions. A band is exact and compact -- you can read
// the proportions off the segment lengths -- but a one-column sliver in a
// seventy-column bar is easy to miss entirely. A ring puts the parts around
// a closed loop where the eye compares ANGLES, and a thin wedge against a
// circle is obvious in a way a thin segment against a line is not.
//
// Use the ring when the composition IS the answer, the band when it is one
// fact among several.
//
// Braille rather than half-blocks: a braille cell is 2 dots wide and 4
// tall, and against a terminal cell's roughly 1:2 aspect that makes the
// dots very nearly SQUARE. A circle in dot space is then a circle on
// screen with no correction, and the ring is four times denser.

#include <algorithm>
#include <cmath>
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

struct Donut {
    struct Seg { std::string label; double value = 0; Color hue; };
    std::vector<Seg> segments;
    // Rows the figure occupies. 7 gives a 14-pixel-tall ring, the smallest
    // that still reads as a circle rather than an octagon.
    int              rows = 7;
    std::string      caption;
    // Centre text -- the headline the ring decorates. A donut with an empty
    // middle wastes the one place a reader is already looking.
    std::string      center;
};

[[nodiscard]] inline int rows_of(const Donut& d, const ItemCtx&) {
    if (d.segments.empty()) return 1;
    const int r = std::clamp(d.rows, 3, 16);
    return d.caption.empty() ? r : r + 1;
}

[[nodiscard]] inline std::pair<std::string, Style>
render(const Donut&, const ItemCtx& ctx) {
    return {std::string{}, Style{}.with_fg(ctx.theme.value)};
}

[[nodiscard]] inline Element render_element(const Donut& d, const ItemCtx& ctx) {
    double total = 0;
    for (const auto& s : d.segments) total += s.value > 0 ? s.value : 0;
    if (d.segments.empty() || total <= 0) return Element{TextElement{}};

    const int rows = std::clamp(d.rows, 3, 16);
    const Color help = ctx.theme.help;
    const Color val  = ctx.theme.value;

    std::vector<Element> lines;

    // Two columns of indent, because every TEXT row has them.
    //
    // A row's leading cell opens with the marker lane -- one cell for the
    // cursor bar and one space after it -- so labels begin at column 2.
    // A picture that starts at column 0 therefore sits two columns left of
    // everything around it, and on a tab that mixes a figure with tables
    // the ring and its caption visibly fail to line up with the rows above
    // and below. The lane is empty here (a figure has no cursor to mark)
    // but the SPACE it occupies is what makes a column a column.
    static constexpr const char* kLane = "  ";

    if (!d.caption.empty())
        lines.push_back(Element{TextElement{
            .content = kLane + d.caption,
            .style   = Style{}.with_fg(help),
            .wrap    = TextWrap::TruncateEnd}});

    auto segs = d.segments;
    const std::string center = d.center;

    lines.push_back(dsl::fill([segs, total, rows, center, help, val](int w, int) {
        // Dots are square, so a `rows`-tall figure is 2*rows dots in radius
        // and therefore 2*rows CELLS across.
        int ring_rows = rows;
        int cw = ring_rows * 2;

        // The legend is not optional -- an unlabelled ring is a few coloured
        // arcs and no information. When the surface cannot hold both, the
        // RING gives up radius until it can: a smaller circle still shows
        // its angles, while a missing key removes the meaning entirely.
        int legend_min = 12;
        for (const auto& s : segs)
            legend_min = std::max(legend_min, unicode::str_width(s.label) + 8);
        while (cw + 3 + legend_min > w && ring_rows > 3) {
            --ring_rows;
            cw = ring_rows * 2;
        }
        const int legend_w = w - cw - 3;
        const bool with_legend = legend_w >= 12;

        // Cumulative segment edges as fractions of the circle.
        std::vector<double> edge;
        edge.reserve(segs.size() + 1);
        double acc = 0;
        edge.push_back(0.0);
        for (const auto& s : segs) {
            acc += (s.value > 0 ? s.value : 0) / total;
            edge.push_back(acc);
        }

        constexpr double kPi = 3.14159265358979323846;
        const int dot_w = cw * 2;
        const int dot_h = ring_rows * 4;
        const double cx = (dot_w - 1) / 2.0;
        const double cy = (dot_h - 1) / 2.0;
        const double r_out = static_cast<double>(dot_h) / 2.0;
        // A WIDE hole. The centre text has to clear the inner edge, and a
        // thin ring drawn in dots reads as a ring; a thick one reads as a
        // filled disc with a bite out of it.
        const double r_in = r_out * 0.62;

        auto seg_at = [&](int dx, int dy) -> int {
            const double x = dx - cx, y = dy - cy;
            const double dist = std::sqrt(x * x + y * y);
            if (dist < r_in || dist > r_out) return -1;
            // Angle from 12 o'clock, clockwise: the direction a reader
            // expects a proportion to start and travel.
            double a = std::atan2(x, -y) / (2 * kPi);
            if (a < 0) a += 1.0;
            for (std::size_t i = 0; i + 1 < edge.size(); ++i)
                if (a >= edge[i] && a < edge[i + 1]) return static_cast<int>(i);
            return static_cast<int>(segs.size()) - 1;
        };

        static constexpr std::uint8_t kDot[4][2] = {
            {0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80},
        };

        const int mid = ring_rows / 2;
        std::vector<Element> out;
        for (int cyc = 0; cyc < ring_rows; ++cyc) {
            // The same two-column marker lane the caption uses, so the ring
            // sits in the same column as every text row on the tab.
            std::string s = "  ";
            std::vector<StyledRun> runs;

            // Centre text, overlaid on the middle row of the hole.
            //
            // One column of air each side. Without it the label butts
            // straight against the inner edge of the ring and the two read
            // as one smear — "██80% hit██" rather than a headline sitting
            // in a hole. The hole exists to give the number somewhere
            // clean to sit, so the gap is part of the figure, not padding.
            const bool is_mid = (cyc == mid) && !center.empty();
            const int ctr_w = is_mid ? unicode::str_width(center) : 0;
            const int ctr_x = is_mid ? (cw - ctr_w) / 2 : -1;
            const int ctr_lo = is_mid ? ctr_x - 1 : -1;
            const int ctr_hi = is_mid ? ctr_x + ctr_w + 1 : -1;

            for (int cxc = 0; cxc < cw; ++cxc) {
                if (is_mid && cxc >= ctr_lo && cxc < ctr_hi) {
                    if (cxc == ctr_x) {
                        const std::size_t at = s.size();
                        s += center;
                        runs.push_back({at, s.size() - at,
                                        Style{}.with_fg(val).with_bold()});
                    } else if (cxc < ctr_x || cxc >= ctr_x + ctr_w) {
                        s += ' ';
                    }
                    continue;
                }
                std::uint8_t bits = 0;
                int votes[16] = {0};
                for (int dy = 0; dy < 4; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        const int sg = seg_at(cxc * 2 + dx, cyc * 4 + dy);
                        if (sg < 0) continue;
                        bits |= kDot[dy][dx];
                        if (sg < 16) ++votes[sg];
                    }
                if (!bits) { s += ' '; continue; }
                // One colour per cell is a braille constraint; taking the
                // MAJORITY is what keeps a wedge boundary from smearing
                // into whichever segment owned the first dot.
                int best = 0;
                for (int i = 1; i < 16; ++i) if (votes[i] > votes[best]) best = i;
                const char32_t cp = 0x2800u + bits;
                const std::size_t at = s.size();
                s += static_cast<char>(0xE0 | (cp >> 12));
                s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                s += static_cast<char>(0x80 | (cp & 0x3F));
                runs.push_back({at, s.size() - at,
                                Style{}.with_fg(segs[static_cast<std::size_t>(best)
                                                     % segs.size()].hue)});
            }

            // One legend entry per ring row, starting at the top.
            if (with_legend && cyc < static_cast<int>(segs.size())) {
                s += "   ";
                const auto& sg = segs[static_cast<std::size_t>(cyc)];
                const std::size_t at = s.size();
                s += "\xe2\x96\xa0 ";                                  // ■
                runs.push_back({at, s.size() - at, Style{}.with_fg(sg.hue)});
                // The label is the WHOLE legend entry. Callers format
                // their own share into it ("wait 8%"), and appending a
                // second percentage here printed "wait 8% 8%".
                const std::size_t tat = s.size();
                s += sg.label;
                runs.push_back({tat, s.size() - tat, Style{}.with_fg(help)});
            }

            out.push_back(Element{TextElement{.content = std::move(s),
                                              .wrap    = TextWrap::TruncateEnd,
                                              .runs    = std::move(runs)}});
        }
        return dsl::v(std::move(out)).build();
    }).height(Dimension::fixed(rows)).build());

    return dsl::v(std::move(lines)).build();
}

} // namespace maya::panel
