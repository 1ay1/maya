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

#include "../../../style/color.hpp"
#include "../../../style/style.hpp"
#include "../context.hpp"

namespace maya::panel {

struct Meter {
    // 0..1. Clamped rather than trusted: a share computed from live
    // telemetry can exceed 1 on a rounding edge, and a bar that overruns
    // its own track reads as a rendering fault.
    double share = 0.0;

    // The filled hue. Defaults to the theme's value colour.
    std::optional<Color> hue{};
};

[[nodiscard]] inline std::pair<std::string, Style>
render(const Meter& c, const ItemCtx& ctx) {
    // 24 preferred, because a meter's resolution IS its width and a stats
    // row can afford more than a settings row's control cell. 8 is the
    // floor below which a bar stops reading as a scale; 40 the ceiling past
    // which the eye starts measuring cells instead of comparing lengths.
    const int cells = ctx.drawn_cells(/*pref=*/24, /*floor=*/8, /*ceiling=*/40);

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

} // namespace maya::panel
