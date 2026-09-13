#pragma once
// maya::Meter — a read-only proportion, drawn as a bar.
//
// A value you are READING (as opposed to a Slider, which you SET). It wants
// resolution and a track — the number, if there is one, is drawn beside it.
//
// A meter FILLS its budget: give it more columns and it draws a finer scale
// of the same fact (that is what separates it from a Spark, which is one
// cell per sample).
//
// Usage:
//   Meter m;
//   m.share(0.72).value("72%");
//   Element ui = m.build();
//
// Standalone: sizes to `width` cells (default 24), and lays the value beside
// the bar as a sibling so the number can never be eaten by the bar growing.

#include <algorithm>
#include <optional>
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

class Meter {
public:
    Meter() = default;
    explicit Meter(double share) : share_(share) {}

    Meter& share(double s)       { share_ = s; return *this; }
    Meter& value(std::string v)  { value_ = std::move(v); return *this; }
    Meter& width(int cells)      { width_ = cells; return *this; }
    Meter& fill_color(Color c)   { fill_color_ = c; return *this; }
    Meter& track_color(Color c)  { track_color_ = c; return *this; }
    Meter& value_color(Color c)  { value_color_ = c; return *this; }

    // Kept so callers that read the raw value (as panel did via `.value`)
    // still work.
    [[nodiscard]] const std::string& value() const { return value_; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        const double t    = std::clamp(share_, 0.0, 1.0);
        const Color  fill = fill_color_;
        const Color  trk  = track_color_;

        // The bar's width. drawn_cells() in the panel deducted a value gap;
        // standalone we just clamp the requested width to the same range.
        static constexpr int kValueGap = 2;
        const int budget = std::clamp(width_, 8, 28);
        const int cells  = std::max(1, budget - kValueGap);

        // The bar paints to whatever width the engine settles on. It does not
        // decide its own size — which is the entire reason this kind needs no
        // shed ladder, no grow arm and no clamp of its own.
        Element bar = dsl::fill([t, fill, trk](int w, int) {
            const int cells = w > 0 ? w : 0;
            const int on    = static_cast<int>(t * cells + 0.5);
            std::string s;
            for (int i = 0; i < on; ++i)     s += "\xe2\x96\x88";   // █
            // The unfilled remainder is DRAWN. An empty tail makes a low bar
            // read as a MISSING bar — the row looks broken rather than small.
            for (int i = on; i < cells; ++i) s += "\xe2\x94\x80";   // ─
            std::vector<StyledRun> runs;
            const std::size_t split = static_cast<std::size_t>(on) * 3;  // █ = 3 bytes
            if (split > 0)        runs.push_back({0, split, Style{}.with_fg(fill)});
            if (split < s.size()) runs.push_back({split, s.size() - split,
                                                  Style{}.with_fg(trk)});
            return Element{TextElement{.content = std::move(s),
                                       .wrap    = TextWrap::TruncateEnd,
                                       .runs    = std::move(runs)}};
        }).build();

        if (value_.empty()) return bar | dsl::width(cells);

        // The value's cell is FIXED at its own width. Its own width is the
        // floor, so a value is never clipped.
        const int vw = static_cast<int>(unicode::str_width(value_));
        Element val{TextElement{.content = value_,
                                .style   = Style{}.with_fg(value_color_),
                                .wrap    = TextWrap::TruncateEnd}};

        // The bar takes a FIXED width, but it SHRINKS: on a narrow surface the
        // bar gives cells and the VALUE is last to yield — a number that is
        // absent reads as nothing at all, and the reader cannot even tell it
        // is missing.
        return dsl::h(std::move(bar) | dsl::width(cells) | dsl::shrink(1.0f),
                      std::move(val) | dsl::width(vw)
                                     | dsl::shrink(0.0f)
                                     | dsl::justify(Justify::End))
               | dsl::gap(2);
    }

private:
    // 0..1. Clamped rather than trusted: a share computed from live
    // telemetry can exceed 1 on a rounding edge, and a bar that overruns its
    // own track reads as a rendering fault.
    double      share_ = 0.0;
    std::string value_;               // the number the bar annotates
    int         width_ = 24;          // preferred cells (clamped 8..28)
    Color       fill_color_  = Color::hex(0xCDD6F4);
    Color       track_color_ = Color::bright_black();
    Color       value_color_ = Color::hex(0xCDD6F4);
};

} // namespace maya
