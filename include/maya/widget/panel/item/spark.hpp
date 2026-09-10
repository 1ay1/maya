#pragma once
// maya::panel item widget: Spark — a series, drawn as a strip.
//
// Meter's sibling, and separate from it on purpose.
//
// A Meter FILLS its budget: give it more columns and it draws a finer scale
// of the same fact. A Spark is one cell per SAMPLE — its natural width is
// the data, and a budget is a window onto it, not a size to stretch to.
// Padding a spark to a meter's width pads with blank, and that blank once
// pushed a value column off the end of its row. Two width rules, so two
// kinds: the difference is stated in the type rather than remembered.
//
// Eight levels (▁▂▃▄▅▆▇█) rather than braille: a strip that sits inline in
// a table row shares its baseline with text, and braille's 2x4 grid reads
// as a different typeface at that size.

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../../../style/color.hpp"
#include "../../../style/style.hpp"
#include "../context.hpp"

namespace maya::panel {

struct Spark {
    std::vector<double> series;
    std::optional<Color> hue{};
};

[[nodiscard]] inline std::pair<std::string, Style>
render(const Spark& c, const ItemCtx& ctx) {
    const Style style = Style{}.with_fg(c.hue.value_or(ctx.theme.value));
    if (c.series.empty()) return {std::string{}, style};

    // The budget is a WINDOW, not a size. The strip is as wide as its data
    // or as wide as the budget, whichever is smaller — never wider, because
    // the extra cells would be blank and blank is not information.
    const int room = ctx.drawn_cells(/*pref=*/24, /*floor=*/4, /*ceiling=*/40);
    const std::size_t take =
        c.series.size() > static_cast<std::size_t>(room)
            ? static_cast<std::size_t>(room) : c.series.size();

    // Show the most RECENT samples. A trend strip that shows the oldest
    // window is answering a question nobody asked — "what is happening" is
    // about the tail.
    const std::size_t from = c.series.size() - take;

    double hi = 0.0;
    double lo = 0.0;
    bool first = true;
    for (std::size_t i = from; i < c.series.size(); ++i) {
        const double v = c.series[i];
        if (first) { hi = lo = v; first = false; }
        else { hi = std::max(hi, v); lo = std::min(lo, v); }
    }

    static constexpr const char* kLevels[8] = {
        "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
        "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88",
    };

    // A FLAT series has no shape, and drawing one would be inventing a
    // reading.
    //
    // Scaling each sample against the window's max sends an all-equal
    // series to ratio 1.0 and paints a solid wall of █ — a strip claiming
    // a peak it never reached, at whatever value happens to be constant.
    // "Nothing is happening here" is the truth, and the floor glyph says
    // it. Tested against the span rather than against zero, because a
    // series flat at 5 and a series flat at 5000 are equally shapeless.
    const bool flat = (hi - lo) <= 0.0;

    std::string out;
    for (std::size_t i = from; i < c.series.size(); ++i) {
        // Scaled against the window's MAX, not its span: a spark shares a
        // baseline of zero with the reader's expectation, and rescaling
        // 90..100 to fill the full height would make a 10% wobble look
        // like a collapse.
        const int lvl = flat
            ? 0
            : std::clamp(static_cast<int>(c.series[i] / hi * 7.0 + 0.5), 0, 7);
        out += kLevels[lvl];
    }
    return {out, style};
}

} // namespace maya::panel
