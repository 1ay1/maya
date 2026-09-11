// test_render_sweep — the properties every panel row must have at every
// width, asserted on painted pixels rather than on the tree.
//
// This file exists because a specific class of bug kept shipping through a
// green suite. Each case below is a real defect that was found by dumping
// a canvas and counting columns by hand, and that no existing test saw:
//
//   * a meter's VALUE pushed off the end of its row by a bar that grew
//     into the space — at every width, silently
//   * rows painting under the scrollbar in two-column mode
//   * a layout that used LESS of a 90-column terminal than of a 76-column
//     one, because a responsive decision was measured at one width and
//     painted at another
//
// The assertions are deliberately about OUTCOMES a reader would notice,
// not about internal numbers: a number must not disappear, a row must not
// overrun its slot, a wider screen must not render less. Widget internals
// are free to change; these are not.

#include "render_sweep.hpp"

#include <maya/widget/panel.hpp>
#include <maya/dsl.hpp>

#include <string>
#include <vector>

using namespace maya;

namespace {

// A panel shaped like a real readout: sections of label→meter→value rows.
// The value is a distinctive string so the harness can prove it survived.
// Every real panel is scrollable, and that is load-bearing here rather
// than incidental: `scroll` is what makes build() reserve a scrollbar
// gutter beside the body. A fixture without it lays out in a slot one
// column wider than the real one and passes while the app is broken —
// which is exactly how the first version of this file went green against
// a live bug.
inline ScrollState g_scroll{};

panel::Config readout(int width, bool flow) {
    panel::Config cfg;
    cfg.title    = "Readout";
    cfg.selected = -1;                 // a document, not a picker
    cfg.min_width = width;
    cfg.scroll   = &g_scroll;
    if (flow) {
        cfg.col_max_width = 64;
        cfg.col_min_width = 52;
    }
    for (int s = 0; s < 3; ++s) {
        panel::Item head;
        head.leading = "SECTION " + std::to_string(s);
        head.control = panel::Header{};
        cfg.items.push_back(std::move(head));

        for (int i = 0; i < 3; ++i) {
            panel::Item row;
            row.leading = "A reasonably long label";
            panel::Meter m;
            m.value = "4" + std::to_string(i) + "ms";
            m.share = 0.25 * (i + 1);
            row.control = m;
            cfg.items.push_back(std::move(row));
        }
    }
    return cfg;
}

} // namespace

// A panel with ONE row and no section header — the shape a hand probe found
// failing when the multi-section fixture above did not.
//
// Kept as its own case rather than folded into `readout` because the
// difference is the POINT: a layout that works with three sections and
// fails with one is a layout whose behaviour depends on something other
// than its slot, and that is worth naming.
namespace {
panel::Config bare_row(int width, bool flow, const char* value = "40ms") {
    panel::Config cfg;
    cfg.title     = "Bare";
    cfg.selected  = -1;
    cfg.min_width = width;
    cfg.scroll    = &g_scroll;
    if (flow) {
        cfg.col_max_width = 64;
        cfg.col_min_width = 52;
    }
    panel::Item row;
    row.leading = "You asked";
    panel::Meter m;
    m.value = value;
    m.share = 0.5;
    row.control = m;
    cfg.items.push_back(std::move(row));
    return cfg;
}
} // namespace

TEST_CASE("a lone meter row keeps its value, flowed or not") {
    sweep::Sweep plain{[](int w) {
        return Element{Panel{bare_row(w, false)}.build()};
    }};
    plain.keeps("40ms");

    sweep::Sweep flowed{[](int w) {
        return Element{Panel{bare_row(w, true)}.build()};
    }};
    flowed.keeps("40ms");

    // A ONE-CHARACTER value, which is where a hand probe first saw this
    // fail. A short value has the least width of its own to defend with,
    // so it is the first thing a greedy neighbour pushes off the row —
    // exactly the case a fixture with comfortable values never reaches.
    sweep::Sweep tiny{[](int w) {
        return Element{Panel{bare_row(w, true, "4")}.build()};
    }};
    tiny.keeps("4");
}

// ── The value must never vanish ─────────────────────────────────────────
//
// The bug this is named for. A meter row is label + bar + value, and the
// three share the row by flex weights; when the bar took the slack, the
// value was pushed past the edge and simply stopped being drawn. A bar
// that is a few cells short is a worse picture. A number that is not there
// is missing information, and the reader cannot tell it is missing.
TEST_CASE("panel rows keep their values at every width") {
    sweep::Sweep plain{[](int w) {
        return Element{Panel{readout(w, /*flow=*/false)}.build()};
    }};
    plain.keeps("40ms");
    plain.keeps("41ms");
    plain.keeps("42ms");
}

// The same panel with column flow enabled must behave identically in this
// respect. Flow changes how many columns there are; it does not license
// dropping content.
//
// This was RED for several rounds and is the case the harness earned its
// keep on. It failed at every width, then at one, then not at all — each
// step a bug the sweep named and a probe confirmed:
//
//   * the unsplit budget must equal the unflowed one exactly
//   * the flowed body must be bounded by the panel's chrome
//   * a single bounded column must constrain its rows
//   * the scrollbar gutter comes off TWICE — it is both frame chrome and
//     a flex sibling
//   * the flowed component must REPORT the width it was bounded to, not
//     the width adapt() offered it; flex sizes a child from its report
//   * spacer_rows() set `basis`, which is the MAIN-AXIS size — height in
//     a column, WIDTH in a row. As build()'s scrollbar gutter it made a
//     one-column spacer claim `vh` columns, and being shrink(0) it took
//     the difference out of the body: two columns per viewport row, off
//     the end of every line.
//
// The last one is why this looked like "each section costs two columns".
// It was never about sections; more sections meant a taller viewport,
// and the gutter's width was the viewport's height.
TEST_CASE("panel rows keep their values when flowed") {
    sweep::Sweep flowed{[](int w) {
        return Element{Panel{readout(w, /*flow=*/true)}.build()};
    }};
    flowed.keeps("40ms");
    flowed.keeps("41ms");
    flowed.keeps("42ms");
}

// ── Nothing may paint past its slot ─────────────────────────────────────
//
// Overflow in a terminal is not clipping, it is corruption: the cells land
// wherever the next thing draws. This is the assertion that catches "text
// under the scrollbar".
TEST_CASE("panel never paints past its slot") {
    sweep::Sweep plain{[](int w) {
        return Element{Panel{readout(w, false)}.build()};
    }};
    plain.fits();

    sweep::Sweep flowed{[](int w) {
        return Element{Panel{readout(w, true)}.build()};
    }};
    flowed.fits();
}

// ── A wider screen must not render less ─────────────────────────────────
//
// The property that catches the CAUSE rather than a symptom. Reflowing,
// rebalancing and shedding are all legitimate and all monotonic. Using
// less of a bigger screen is not: it means a responsive layout decided its
// shape against one width and was painted at another.
TEST_CASE("a wider panel never uses less of its width") {
    sweep::Sweep plain{[](int w) {
        return Element{Panel{readout(w, false)}.build()};
    }};
    plain.monotonic();

    sweep::Sweep flowed{[](int w) {
        return Element{Panel{readout(w, true)}.build()};
    }};
    flowed.monotonic();
}

// ── The frame claims its width ──────────────────────────────────────────
//
// A panel draws a border, so the painted width should be the slot itself.
// Slack of 0 is the real contract here; anything less means the frame is
// not reaching its own edge.
TEST_CASE("a panel frame reaches both its edges") {
    sweep::Sweep plain{[](int w) {
        return Element{Panel{readout(w, false)}.build()};
    }};
    plain.fills(/*slack=*/0);
}

// ── The harness itself ──────────────────────────────────────────────────
//
// A property test that cannot fail is worth nothing, so prove each
// assertion actually bites. Cheap insurance against the harness quietly
// measuring the wrong thing — which it did once, by counting bytes instead
// of display columns and reporting a 21-column strip as 63.
TEST_CASE("render_sweep measures display columns, not bytes") {
    // A rule of box-drawing glyphs: 3 bytes each, 1 column each.
    sweep::Sweep rules{[](int w) {
        std::string bar;
        for (int i = 0; i < w; ++i) bar += "\xe2\x94\x80";   // ─
        return dsl::text(bar).build();
    }};
    // If this measured bytes it would see 3x the slot and fits() would
    // fail on every frame.
    rules.fits();
    rules.fills(0);
    rules.monotonic();
}
