#pragma once
// maya::panel item widget: Meter — a read-only proportion, drawn as a bar.
//
// Slider's read-only sibling. The distinction is not interactivity for its
// own sake: a Slider is a value you are SETTING, so it wants a step size, a
// range and a printed number. A Meter is a value you are READING, so it
// wants resolution and a track — the number, if there is one, is the item's
// own trailing cell.
//
// Kept separate from Spark for the same reason. A meter FILLS its budget:
// give it more columns and it draws a finer scale of the same fact. A spark
// has a natural width — one cell per sample — and padding it to a budget
// pads with blank. Conflating them is what pushed a value column off the
// end of its row when the two shared one width.

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

struct Meter {
    // 0..1. Clamped rather than trusted: a share computed from live
    // telemetry can exceed 1 on a rounding edge, and a bar that overruns
    // its own track reads as a rendering fault.
    double share = 0.0;

    // The number the bar annotates, drawn beside it.
    //
    // Here rather than in the item's `trailing` cell because a control
    // REPLACES that cell rather than sitting beside it — panel.cpp only
    // falls back to `trailing` when the control rendered nothing. A row
    // that sets both silently loses the value, which is a trap worth
    // closing in the type: a bar without its number is a shape with no
    // scale, so the two travel together.
    std::string value;

    // The filled hue. Defaults to the theme's value colour.
    std::optional<Color> hue{};
};

[[nodiscard]] inline std::pair<std::string, Style>
render(const Meter& c, const ItemCtx& ctx) {
    // 24 preferred, because a meter's resolution IS its width and a stats
    // row can afford more than a settings row's control cell. 8 is the
    // floor below which a bar stops reading as a scale; 28 the ceiling.
    //
    // The ceiling is the interesting number. A meter that grows without
    // bound looks like it is using the space well and is not: at 40 cells
    // on a wide pane the bar detaches from its label, the eye has to
    // travel the width of the pane to pair a name with its length, and
    // comparing two rows means comparing two lines that no longer share a
    // visual edge. Past roughly 28 cells the extra resolution answers a
    // question nobody asked — a bar is a COMPARISON, not a measurement,
    // and 1% of difference was never going to be read off it.
    const int cells = ctx.drawn_cells(/*pref=*/24, /*floor=*/8, /*ceiling=*/28);

    const double t = std::clamp(c.share, 0.0, 1.0);
    const int on = static_cast<int>(t * cells + 0.5);

    // The unfilled remainder is DRAWN, not left blank.
    //
    // An empty tail makes a short bar read as a MISSING bar — the row looks
    // broken rather than low. A visible track says "this is the scale, and
    // you are here on it", which is the entire question a meter answers.
    std::string out;
    for (int i = 0; i < on; ++i)     out += "\xe2\x96\x88";   // █
    for (int i = on; i < cells; ++i) out += "\xe2\x94\x80";   // ─

    const Style fill  = Style{}.with_fg(c.hue.value_or(ctx.theme.value));
    const Style track = Style{}.with_fg(ctx.theme.help);

    // Two hues, via the runs channel. A bar painted in one colour is a
    // rectangle: the boundary between filled and unfilled is the datum.
    if (ctx.runs_out) {
        const std::size_t split = static_cast<std::size_t>(on) * 3;  // █ is 3 bytes
        ctx.runs_out->clear();
        if (split > 0)          ctx.runs_out->push_back({0, split, fill});
        if (split < out.size()) ctx.runs_out->push_back({split, out.size() - split, track});
    }
    // The single style is the FILL, so a host that ignores runs_out still
    // gets a bar in the right colour rather than a grey smear.
    return {out, fill};
}

// The laid-out form — what the panel uses when it can.
//
// Two SIBLINGS, not one padded string: a bar that grows into the row's
// slack, and a value pinned to a shared width so every row's number ends in
// the same column. maya's flex engine does the growing, the clamping and
// the alignment, all of which this file would otherwise hand-roll.
//
// The difference is not stylistic. When the bar and the value were one
// concatenated cell, growing the bar consumed the value's characters and
// the number silently vanished — twice, in two separate attempts. As
// siblings that is not expressible: the engine allocates both before either
// renders, so a bar cannot take a cell that was never offered to it.
[[nodiscard]] inline Element render_element(const Meter& c,
                                            const ItemCtx& ctx) {
    const double t    = std::clamp(c.share, 0.0, 1.0);
    const Color  fill = c.hue.value_or(ctx.theme.value);
    const Color  trk  = ctx.theme.help;

    // The bar's width, from the budget Panel already sized for this row —
    // MINUS the gap the row below asks for.
    //
    // drawn_cells() answers "how many cells may the picture use", and the
    // row then requests that many PLUS a two-column gap PLUS the value. At
    // the floor that is 8 + 2 + 4 = 14 asked of a 13-column cell, over by
    // exactly one — and because the value is pinned at shrink(0) so it
    // cannot be squeezed, the one column comes off its END. "40ms" painted
    // as "40m", then "40", then nothing, losing two more columns for every
    // section that tightened the row further. The flex trace shows it
    // plainly: avail=13 hypo=12 gap=2 free=-1.
    //
    // Deducting the gap here keeps the promise drawn_cells() makes. A very
    // narrow row now gives up bar cells rather than giving up the number.
    static constexpr int kValueGap = 2;
    const int cells = std::max(1, ctx.drawn_cells(/*pref=*/24, /*floor=*/8,
                                                  /*ceiling=*/28) - kValueGap);

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

    if (c.value.empty()) return bar | dsl::width(cells);

    // The value's cell is FIXED at the table's shared width, so every row's
    // number ends in the same column — alignment by a shared number rather
    // than by each row padding a string to the same length. Its own width
    // is the floor, so a longer-than-average value is never clipped by a
    // basis measured against shorter siblings.
    const int vw = static_cast<int>(unicode::str_width(c.value));
    Element val{TextElement{.content = c.value,
                            .style   = Style{}.with_fg(ctx.theme.value),
                            .wrap    = TextWrap::TruncateEnd}};

    // The bar takes a FIXED width, not grow(1) — but it SHRINKS.
    //
    // grow() has no ceiling in this DSL, so a growing bar consumes every
    // spare column and starves the label beside it — "Average confidence"
    // truncated to "Avera…" while the bar ran eighty cells. The budget is
    // already the row's fair share (Panel deducts the label lane, the gaps
    // and the value cell before handing it over), so asking for exactly it
    // is both simpler and correct.
    //
    // Shrink is the other half of that bargain, and it was missing. On a
    // narrow terminal the row cannot hold label + bar + value, so something
    // has to yield; with the bar refusing and the value pinned at
    // shrink(0), the overflow came off the END of the row and the number
    // was simply not drawn. A bar that is a few cells shorter still reads
    // as a proportion. A number that is absent reads as nothing at all, and
    // the reader cannot even tell it is missing.
    //
    // So the order of sacrifice is now stated rather than emergent: the
    // LABEL truncates first (row_line weights it 3x), then the BAR gives
    // cells, and the VALUE is last — which is the order row_line's own
    // comment already claimed and only two thirds of the row honoured.
    return dsl::h(std::move(bar) | dsl::width(cells) | dsl::shrink(1.0f),
                  std::move(val) | dsl::width(std::max(ctx.value_basis, vw))
                                 | dsl::shrink(0.0f)
                                 | dsl::justify(Justify::End))
           | dsl::gap(2);
}

} // namespace maya::panel
