#pragma once
// maya::Donut — a composition drawn as a braille ring (a pie/donut chart).
//
// The parts of a whole, drawn around a closed loop so the eye compares
// ANGLES rather than segment lengths — a thin wedge against a circle is
// obvious in a way a thin segment against a bar is not. Use it when the
// composition IS the answer.
//
// Braille rather than half-blocks: a braille cell is 2 dots wide and 4
// tall, and against a terminal cell's ~1:2 aspect that makes the dots very
// nearly SQUARE, so a circle in dot space is a circle on screen with no
// correction — and the ring is four times denser.
//
// Usage:
//   Donut d;
//   d.segment("cache", 72, Color::green())
//    .segment("miss",  28, Color::red())
//    .center("72%").caption("cache hits").rows(7);
//   Element ui = d.build();
//
// Standalone: sizes to whatever width the layout gives it (via fill()), and
// occupies `rows` terminal rows (+1 for a caption).

#include <algorithm>
#include <cmath>
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

class Donut {
public:
    struct Seg {
        std::string label;
        double      value = 0;
        Color       hue;
    };

    Donut() = default;
    explicit Donut(std::vector<Seg> segs) : segments_(std::move(segs)) {}

    Donut& segment(std::string label, double value, Color hue) {
        segments_.push_back({std::move(label), value, hue});
        return *this;
    }
    Donut& rows(int r)              { rows_ = r; return *this; }
    Donut& caption(std::string c)   { caption_ = std::move(c); return *this; }
    Donut& center(std::string c)    { center_ = std::move(c); return *this; }
    Donut& label_color(Color c)     { label_color_ = c; return *this; }
    Donut& center_color(Color c)    { center_color_ = c; return *this; }

    operator Element() const { return build(); }

    // Rows this figure paints — a caller sizing a slot can ask ahead.
    [[nodiscard]] int height() const {
        if (segments_.empty()) return 1;
        const int r = std::clamp(rows_, 3, 16);
        return caption_.empty() ? r : r + 1;
    }

    [[nodiscard]] Element build() const {
        double total = 0;
        for (const auto& s : segments_) total += s.value > 0 ? s.value : 0;
        if (segments_.empty() || total <= 0) return Element{TextElement{}};

        const int   rows = std::clamp(rows_, 3, 16);
        const Color help = label_color_;
        const Color val  = center_color_;

        std::vector<Element> lines;

        if (!caption_.empty())
            lines.push_back(Element{TextElement{
                .content = caption_,
                .style   = Style{}.with_fg(help),
                .wrap    = TextWrap::TruncateEnd}});

        auto segs = segments_;
        const std::string center = center_;

        lines.push_back(dsl::fill([segs, total, rows, center, help, val](int w, int) {
            // Dots are square, so a `rows`-tall figure is 2*rows dots in
            // radius and therefore 2*rows CELLS across.
            int ring_rows = rows;
            int cw = ring_rows * 2;

            // The legend is not optional — an unlabelled ring is a few
            // coloured arcs and no information. When the surface cannot hold
            // both, the RING gives up radius until it can.
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
            const double r_in = r_out * 0.62;   // a wide hole for the centre text

            auto seg_at = [&](int dx, int dy) -> int {
                const double x = dx - cx, y = dy - cy;
                const double dist = std::sqrt(x * x + y * y);
                if (dist < r_in || dist > r_out) return -1;
                double a = std::atan2(x, -y) / (2 * kPi);   // from 12 o'clock, CW
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
                std::string s;
                std::vector<StyledRun> runs;

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

                if (with_legend && cyc < static_cast<int>(segs.size())) {
                    s += "   ";
                    const auto& sg = segs[static_cast<std::size_t>(cyc)];
                    const std::size_t at = s.size();
                    s += "\xe2\x96\xa0 ";                                  // ■
                    runs.push_back({at, s.size() - at, Style{}.with_fg(sg.hue)});
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

private:
    std::vector<Seg> segments_;
    int              rows_ = 7;
    std::string      caption_;
    std::string      center_;
    Color            label_color_  = Color::bright_black();
    Color            center_color_ = Color::hex(0xCDD6F4);
};

} // namespace maya
