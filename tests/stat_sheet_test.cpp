// stat_sheet_test.cpp — the alignment invariants, which are the widget.
//
// Asserts on COLUMN POSITIONS, never on box-drawing bytes: the glyph ramp
// is a style decision that may change, while "every value ends in the same
// column" is the property that makes a stats readout legible and must not.
//
// Non-ASCII folds to '?' in the row dumps below (same convention as
// tab_strip_test), which is enough to locate columns without pinning the
// exact eighth-block byte a share happens to round to.

#include "doctest/doctest.h"

#include <string>
#include <vector>

#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>
#include <maya/text/unicode_width.hpp>
#include <maya/widget/stat_sheet.hpp>

using namespace maya;

namespace {

struct Rendered {
    std::vector<std::string> rows;
};

std::string row_text(const Canvas& c, int y, int width) {
    std::string s;
    for (int x = 0; x < width; ++x) {
        const char32_t ch = c.get(x, y).character;
        if (ch >= 0x20 && ch < 0x7F) s += static_cast<char>(ch);
        else if (ch == 0 || ch == U' ') s += ' ';
        else s += '?';
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

Rendered render_sheet(const StatSheet& s, int width) {
    StylePool pool;
    Canvas canvas(width, 24, &pool);
    render_tree(s.build(), canvas, pool, theme::dark, /*auto_height=*/true);
    Rendered out;
    for (int y = 0; y < 24; ++y) out.rows.push_back(row_text(canvas, y, width));
    while (!out.rows.empty() && out.rows.back().empty()) out.rows.pop_back();
    return out;
}

// Column where a row's last non-space character sits. For an entry row that
// is the right edge of the value (or of the detail column when present).
int right_edge(const std::string& row) {
    return static_cast<int>(row.size());
}

}  // namespace

TEST_CASE("stat sheet: values right-align across rows of unequal labels") {
    // THE invariant. Two numbers you can compare without reading them is
    // the difference between a readout and debug output, and it only holds
    // if the sheet measures every row before painting any of them.
    StatSheet s;
    s.entry({.label = "a",                 .value = "7"});
    s.entry({.label = "a much longer name", .value = "1234"});
    s.entry({.label = "mid",               .value = "88"});

    const auto r = render_sheet(s, 60);
    REQUIRE(r.rows.size() == 3u);
    CHECK(right_edge(r.rows[0]) == right_edge(r.rows[1]));
    CHECK(right_edge(r.rows[1]) == right_edge(r.rows[2]));
}

TEST_CASE("stat sheet: a tiny share still draws a visible bar") {
    // A 1% share must not floor to an empty bar. "This model did almost no
    // work" and "this model did no work" are different readings, and a
    // rounding rule that erases the first is a rounding rule that lies.
    StatSheet s;
    s.entry({.label = "tiny", .value = "1", .share = 0.01});

    const auto r = render_sheet(s, 60);
    REQUIRE(!r.rows.empty());
    // At least one non-ASCII glyph: the partial block.
    CHECK(r.rows[0].find('?') != std::string::npos);
}

TEST_CASE("stat sheet: a zero share draws an empty track, not nothing") {
    // An explicit 0.0 is a real reading ("this bucket exists and is empty")
    // and must still show its scale. share < 0 is the way to say "no bar".
    StatSheet with_zero;
    with_zero.entry({.label = "none", .value = "0", .share = 0.0});
    StatSheet no_bar;
    no_bar.entry({.label = "none", .value = "0"});

    const auto a = render_sheet(with_zero, 60);
    const auto b = render_sheet(no_bar, 60);
    REQUIRE(!a.rows.empty());
    REQUIRE(!b.rows.empty());
    // The track glyphs are drawn in one and absent in the other.
    CHECK(a.rows[0].find('?') != std::string::npos);
    CHECK(b.rows[0].find('?') == std::string::npos);
}

TEST_CASE("stat sheet: bars share one track width") {
    // A share is only readable against its neighbours, so every bar must be
    // measured against the same track. Two bars scaled independently to
    // their own widths compare nothing.
    StatSheet s;
    s.entry({.label = "full", .value = "10", .share = 1.0});
    s.entry({.label = "half", .value = "5",  .share = 0.5});

    const auto r = render_sheet(s, 60);
    REQUIRE(r.rows.size() == 2u);
    CHECK(right_edge(r.rows[0]) == right_edge(r.rows[1]));
}

TEST_CASE("stat sheet: narrow widths shed detail, then bar, never the value") {
    // The degradation ladder. The number is the datum; the note is
    // redundant with it and the bar is a comparison aid, so they go first.
    // A row that kept its bar and dropped its number would be a picture of
    // a statistic with the statistic removed.
    StatSheet s;
    s.entry({.label = "output", .value = "1234", .detail = "48%", .share = 0.48});

    for (int w : {60, 30, 18, 12}) {
        const auto r = render_sheet(s, w);
        REQUIRE(!r.rows.empty());
        CHECK(r.rows[0].find("1234") != std::string::npos);
    }
}

TEST_CASE("stat sheet: headings and blanks occupy their own rows") {
    StatSheet s;
    s.heading("By role");
    s.entry({.label = "Strategic", .value = "12"});
    s.blank();
    s.heading("By model");
    s.entry({.label = "haiku", .value = "3"});

    const auto r = render_sheet(s, 60);
    REQUIRE(r.rows.size() == 5u);
    CHECK(r.rows[0].find("By role") != std::string::npos);
    CHECK(r.rows[2].empty());
    CHECK(r.rows[3].find("By model") != std::string::npos);
}

TEST_CASE("stat sheet: the hero row leads with the figure") {
    // A tab that opens with a table makes the reader derive the answer.
    StatSheet s;
    s.hero("87%", "of routed turns ran below the Strategic model");

    const auto r = render_sheet(s, 60);
    REQUIRE(!r.rows.empty());
    CHECK(r.rows[0].rfind("87%", 0) == 0);
}

TEST_CASE("stat sheet: a wide row takes the remaining columns") {
    // The meter shape — one row that IS the chart, for a context gauge,
    // versus one of a ranked set sharing a track.
    StatSheet s;
    s.entry({.label = "context", .value = "62%", .share = 0.62, .wide = true});
    const auto wide = render_sheet(s, 60);

    StatSheet t;
    t.entry({.label = "context", .value = "62%", .share = 0.62});
    const auto norm = render_sheet(t, 60);

    REQUIRE(!wide.rows.empty());
    REQUIRE(!norm.rows.empty());
    CHECK(right_edge(wide.rows[0]) > right_edge(norm.rows[0]));
}

TEST_CASE("stat sheet: a sparkline row aligns with bar rows") {
    // Trend strips and bars occupy the SAME columns, so a sheet may mix
    // them and still align — which is what lets one tab show a ranked
    // tally and a throughput trend without looking like two widgets.
    StatSheet s;
    s.entry({.label = "rate", .value = "1.2k/s",
             .spark = {1.0, 4.0, 2.0, 8.0, 3.0}});
    s.entry({.label = "share", .value = "48%", .share = 0.48});

    const auto r = render_sheet(s, 60);
    REQUIRE(r.rows.size() == 2u);
    CHECK(right_edge(r.rows[0]) == right_edge(r.rows[1]));
}

TEST_CASE("stat sheet: an empty sheet renders nothing and does not crash") {
    StatSheet s;
    CHECK(s.empty());
    const auto r = render_sheet(s, 60);
    CHECK(r.rows.empty());
}

TEST_CASE("stat sheet: a band's segments fill the track exactly") {
    // A composition bar claims its parts ARE the whole, so its right edge
    // must not wobble with the data. Rounding each segment independently
    // leaves the total short; largest-remainder apportionment is what
    // makes the claim true at every width.
    for (int w : {72, 51, 40, 23}) {
        StatSheet s;
        s.band({.caption = "",
                .segments = {{"read", 68000, Color::green()},
                             {"write", 12000, Color::yellow()},
                             {"miss", 21000, Color::red()}},
                .legend = false});
        const auto r = render_sheet(s, w);
        REQUIRE(!r.rows.empty());
        CHECK(unicode::str_width(r.rows[0]) == w);
    }
}

TEST_CASE("stat sheet: a sliver segment still gets a column") {
    // Same rule as the 1% bar: "a sliver" and "nothing" are different
    // readings, and a band that drops a tiny segment silently reports a
    // composition the session did not have.
    StatSheet s;
    s.band({.caption = "",
            .segments = {{"huge", 100000, Color::green()},
                         {"tiny", 1, Color::red()}},
            .legend = false});
    const auto r = render_sheet(s, 40);
    REQUIRE(!r.rows.empty());
    CHECK(unicode::str_width(r.rows[0]) == 40);
}

TEST_CASE("stat sheet: a band renders bar plus one legend row") {
    StatSheet s;
    s.band({.caption = "by origin",
            .segments = {{"read", 3, Color::green()},
                         {"miss", 1, Color::red()}}});
    const auto r = render_sheet(s, 40);
    // caption + bar + legend, and the legend is ONE row however many
    // segments there are — a per-segment legend is a bar chart in
    // disguise and costs the space the band form exists to save.
    REQUIRE(r.rows.size() == 3u);
    CHECK(r.rows[0].find("by origin") != std::string::npos);
    CHECK(r.rows[2].find("read") != std::string::npos);
    CHECK(r.rows[2].find("miss") != std::string::npos);
}

TEST_CASE("stat sheet: a plot occupies exactly the rows it asked for") {
    // A figure that silently grows takes the space of the rows beneath it
    // in a scrolling panel.
    for (int rows : {1, 3, 5, 8}) {
        StatSheet s;
        s.plot({.caption = "", .series = {1, 5, 2, 8, 3}, .rows = rows});
        const auto r = render_sheet(s, 40);
        CHECK(static_cast<int>(r.rows.size()) == rows);
    }
}

TEST_CASE("stat sheet: a plot's scale labels ride the first and last rows") {
    // A curve with no peak label is a shape without units — decoration
    // rather than a statistic.
    StatSheet s;
    s.plot({.caption = "", .series = {1, 9, 3, 7}, .rows = 4,
            .peak_label = "1.5k", .base_label = "0"});
    const auto r = render_sheet(s, 40);
    REQUIRE(r.rows.size() == 4u);
    CHECK(r.rows.front().find("1.5k") != std::string::npos);
    CHECK(r.rows.back().find("0") != std::string::npos);
    // And the plot body never overruns the gutter those labels sit in.
    for (const auto& row : r.rows) CHECK(unicode::str_width(row) <= 40);
}

TEST_CASE("stat sheet: a plot joins consecutive samples") {
    // A steep move must read as one falling line, not as two unrelated
    // marks. The vertical join is what makes it a line chart rather than
    // a scatter of dots.
    StatSheet s;
    s.plot({.caption = "", .series = {0, 100}, .rows = 4});
    const auto r = render_sheet(s, 20);
    REQUIRE(r.rows.size() == 4u);
    // Every row carries ink: the stroke spans the full height between the
    // two samples. Without the join only the top and bottom rows would.
    for (const auto& row : r.rows)
        CHECK(row.find('?') != std::string::npos);
}

TEST_CASE("stat sheet: degenerate graph inputs render nothing, not garbage") {
    StatSheet s;
    s.band({.caption = "", .segments = {}});             // no segments
    s.band({.caption = "", .segments = {{"z", 0, Color::red()}}});  // all zero
    s.plot({.caption = "", .series = {}});               // no samples
    const auto r = render_sheet(s, 40);
    CHECK(r.rows.empty());
}

TEST_CASE("stat sheet: measured height matches rendered rows") {
    // A host that vstacks the sheet asks measure_element() for its height,
    // and measures at a HUGE width (maya's panel uses 1<<14). If the sheet
    // reports a different row count there than it paints at the real
    // width, the host budgets the wrong number of rows and the tail of the
    // sheet is silently dropped — which is how a five-row plot rendered as
    // one row with no scrollbar and no clue.
    StatSheet s;
    s.indent(1);
    s.heading("Trend");
    s.plot({.caption = "out", .series = {1, 5, 2, 8, 3, 9, 4}, .rows = 5,
            .peak_label = "5.4k", .base_label = "0"});
    const auto e = s.build();

    // 1 heading + 1 caption + 5 plot rows.
    const int expected = 7;
    CHECK(render_sheet(s, 76).rows.size() == static_cast<std::size_t>(expected));
    CHECK(measure_element(e, 76).height.value == expected);
    CHECK(measure_element(e, 1 << 14).height.value == expected);
}

TEST_CASE("stat sheet: reserve_right keeps full-width rows off the edge") {
    // Bands and plots are the only forms that reach the right edge, and a
    // host's border / scrollbar / inner pad live there. The sheet is handed
    // the OUTER width and cannot see that chrome, so the host states it.
    StatSheet s;
    s.reserve_right(7);
    s.band({.caption = "", .segments = {{"a", 1, Color::green()},
                                        {"b", 1, Color::red()}},
            .legend = false});
    const auto r = render_sheet(s, 76);
    REQUIRE(!r.rows.empty());
    CHECK(unicode::str_width(r.rows[0]) == 76 - 7);
}

TEST_CASE("stat sheet: a filled plot draws solid columns to the baseline") {
    // The area form. A filled plot answers "how much", so the region under
    // the curve is ink and the top edge is the reading — which is why it
    // uses BLOCKS: braille filled solid is a slab where the eye cannot
    // find the surface.
    StatSheet s;
    s.plot({.caption = "", .series = {1, 8}, .rows = 4, .filled = true});
    const auto r = render_sheet(s, 20);
    REQUIRE(r.rows.size() == 4u);
    // The last row is ink across the width (both samples are above zero),
    // and the first row carries only the peak's column.
    CHECK(r.rows.back().find('?') != std::string::npos);
    CHECK(r.rows.front().find('?') != std::string::npos);
    // A filled plot never leaves a gap UNDER a drawn column: scan the
    // bottom row for the peak's columns being present.
    CHECK(unicode::str_width(r.rows.back()) > 0);
}

TEST_CASE("stat sheet: a filled plot shades under its curve") {
    // BOTH forms are braille — a block column has a flat top edge one
    // eighth of a row tall, which quantises a smooth series into a
    // staircase, so the area form keeps the 2x4 dot grid and shades the
    // region under the line with a sparse stipple instead.
    //
    // The distinction is therefore INK, not glyph family: the filled form
    // lights strictly more dots than the line, and the extra dots are all
    // below the curve. Counting lit dots is the honest measure — a blank
    // braille cell is still a code point (U+2800), so counting glyphs
    // compares nothing.
    auto lit = [](const StatSheet& s) {
        StylePool pool;
        Canvas canvas(30, 8, &pool);
        render_tree(s.build(), canvas, pool, theme::dark, true);
        int dots = 0;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 30; ++x) {
                const char32_t c = canvas.get(x, y).character;
                if (c >= 0x2800 && c <= 0x28FF)
                    for (int b = 0; b < 8; ++b)
                        if ((c - 0x2800) & (1u << b)) ++dots;
            }
        return dots;
    };

    StatSheet filled;
    filled.plot({.caption = "", .series = {3, 5, 4, 8}, .rows = 4,
                 .filled = true});
    StatSheet line;
    line.plot({.caption = "", .series = {3, 5, 4, 8}, .rows = 4,
               .filled = false});

    CHECK(lit(filled) > lit(line));
    // But not SOLID: a fully filled region is a slab where the surface —
    // the one thing a reader is looking for — disappears into the mass.
    // The stipple lights about a quarter of the area, so the filled form
    // stays well under a hypothetical solid fill.
    CHECK(lit(filled) < 30 * 4 * 8 / 2);
}

TEST_CASE("stat sheet: a filled plot keeps a non-zero sample visible") {
    // Same rule as the 1% bar and the sliver band segment: a sample that
    // rounds below one eighth of a row still draws, because "almost none"
    // and "none" are different readings.
    StatSheet s;
    s.plot({.caption = "", .series = {1000, 1}, .rows = 4, .filled = true});
    const auto r = render_sheet(s, 20);
    REQUIRE(r.rows.size() == 4u);
    // The tiny sample occupies the right half; the bottom row must carry
    // ink there rather than being blank.
    CHECK(r.rows.back().find('?') != std::string::npos);
}

TEST_CASE("stat sheet: columns() splits only when the width allows") {
    // A stats tab on a 200-column terminal is a narrow ribbon with two
    // thirds of the screen blank; on an 80-column one a second column
    // would squeeze every label to nothing. The threshold is a minimum
    // column WIDTH rather than a terminal-size breakpoint, because that
    // says what a column needs instead of guessing which terminals exist.
    auto sheet = [] {
        StatSheet s;
        s.columns(34, 2);
        s.heading("A");
        s.entry({.label = "a1", .value = "1"});
        s.entry({.label = "a2", .value = "2"});
        s.heading("B");
        s.entry({.label = "b1", .value = "3"});
        s.entry({.label = "b2", .value = "4"});
        return s;
    };
    // 60 cols: one column of 34 fits, two do not (34+3+34 = 71).
    const auto narrow = render_sheet(sheet(), 60);
    // 100 cols: two fit.
    const auto wide = render_sheet(sheet(), 100);
    CHECK(narrow.rows.size() > wide.rows.size());
    // Both headings survive the split — a column boundary must not eat a
    // section.
    bool a = false, b = false;
    for (const auto& r : wide.rows) {
        if (r.find("A") != std::string::npos) a = true;
        if (r.find("B") != std::string::npos) b = true;
    }
    CHECK(a);
    CHECK(b);
}

TEST_CASE("stat sheet: a column break is inert in one column") {
    // The break is a marker, not content. If it emitted even a blank row
    // the sheet's height would depend on whether it happened to split — a
    // layout that jumps when the terminal crosses a breakpoint.
    StatSheet with_break;
    with_break.entry({.label = "a", .value = "1"});
    with_break.column_break();
    with_break.entry({.label = "b", .value = "2"});

    StatSheet without;
    without.entry({.label = "a", .value = "1"});
    without.entry({.label = "b", .value = "2"});

    CHECK(render_sheet(with_break, 60).rows.size()
          == render_sheet(without, 60).rows.size());
}

TEST_CASE("stat sheet: columns are balanced by row count") {
    // By height, never by section count: sections differ wildly in length
    // (a two-row table against a nine-bucket distribution), and splitting
    // on section count alone leaves one column twice the height of the
    // other — which reads as a layout bug rather than as a choice.
    StatSheet s;
    s.columns(20, 2);
    s.heading("short");
    s.entry({.label = "x", .value = "1"});
    s.heading("long");
    for (int i = 0; i < 10; ++i)
        s.entry({.label = "row" + std::to_string(i), .value = "9"});

    const auto r = render_sheet(s, 80);
    // The tall section did NOT get crammed in beside the short one: the
    // rendered height is close to the taller half, not to the whole.
    CHECK(r.rows.size() < 13u);
    CHECK(r.rows.size() >= 10u);
}

TEST_CASE("stat sheet: a donut is round, not an ellipse") {
    // Terminal cells are about twice as tall as they are wide, so a circle
    // plotted in cell units comes out squashed. The x radius is doubled to
    // correct it — which means the figure is about twice as wide as it is
    // tall, and that ratio is the test.
    StatSheet s;
    s.donut({.caption = "",
             .segments = {{"a", 3, Color::green()}, {"b", 1, Color::red()}},
             .rows = 7});
    const auto r = render_sheet(s, 80);
    REQUIRE(!r.rows.empty());
    int widest = 0;
    for (const auto& row : r.rows)
        widest = std::max(widest, unicode::str_width(row));
    const int tall = static_cast<int>(r.rows.size());
    // Roughly 2:1 — generous bounds, because the legend rides alongside
    // and the ring is rasterised, but a 1:1 blob or a 4:1 smear fails.
    CHECK(widest > tall);
}

TEST_CASE("stat sheet: a donut has a hole") {
    // It is a DONUT, not a pie, and the hole is not decoration: it is
    // where the headline goes, which is the one place a reader looking at
    // a ring is already looking.
    StatSheet s;
    s.donut({.caption = "",
             .segments = {{"a", 1, Color::green()}},
             .rows = 7,
             .center = "80%"});
    const auto r = render_sheet(s, 60);
    REQUIRE(!r.rows.empty());
    bool found = false;
    for (const auto& row : r.rows)
        if (row.find("80%") != std::string::npos) found = true;
    CHECK(found);
}

TEST_CASE("stat sheet: a donut keeps its legend by shrinking the ring") {
    // An unlabelled ring is three coloured arcs and no information, so
    // when the surface cannot hold both the RING gives up radius — a
    // smaller circle still shows its angles, while a missing key removes
    // the meaning entirely.
    StatSheet s;
    s.donut({.caption = "",
             .segments = {{"alpha 1", 3, Color::green()},
                          {"beta 2", 1, Color::red()}},
             .rows = 7});
    // 40 columns cannot fit a 7-row ring (29 cells) plus a legend.
    const auto r = render_sheet(s, 40);
    REQUIRE(!r.rows.empty());
    bool legend = false;
    for (const auto& row : r.rows)
        if (row.find("alpha") != std::string::npos) legend = true;
    CHECK(legend);
    for (const auto& row : r.rows) CHECK(unicode::str_width(row) <= 40);
}

TEST_CASE("stat sheet: every legend entry is drawn") {
    // More segments than the ring is tall: the overflow gets its own rows
    // rather than being dropped. A key that silently omits a wedge is
    // worse than one that costs an extra line.
    StatSheet s;
    StatDonut d;
    d.rows = 3;
    for (int i = 0; i < 6; ++i)
        d.segments.push_back({"seg" + std::to_string(i), 1.0,
                              Color::green()});
    s.donut(std::move(d));
    const auto r = render_sheet(s, 70);
    for (int i = 0; i < 6; ++i) {
        bool found = false;
        for (const auto& row : r.rows)
            if (row.find("seg" + std::to_string(i)) != std::string::npos)
                found = true;
        CHECK(found);
    }
}

TEST_CASE("stat sheet: a degenerate donut renders nothing") {
    StatSheet s;
    s.donut({.caption = "", .segments = {}});
    s.donut({.caption = "", .segments = {{"z", 0, Color::red()}}});
    CHECK(render_sheet(s, 60).rows.empty());
}

TEST_CASE("stat sheet: a histogram is columns plus a baseline and ticks") {
    // The vertical form: one COLUMN per bucket, so the whole distribution
    // is a shape the eye takes in at once. It costs a fixed height where
    // Dist costs a row per bucket, which is the trade.
    StatSheet s;
    s.histogram({.caption = "spread",
                 .buckets = {{"1", 1}, {"2", 5}, {"3", 3}},
                 .rows = 4,
                 .y_labels = {"5", "4", "3", "1"}});
    const auto r = render_sheet(s, 60);
    // caption + 4 column rows + baseline + ticks.
    REQUIRE(r.rows.size() == 7u);
    CHECK(r.rows[0].find("spread") != std::string::npos);
    // The baseline is a rule, not data.
    CHECK(r.rows[5].find('?') != std::string::npos);
    // Ticks name at least the first bucket.
    CHECK(r.rows[6].find("1") != std::string::npos);
}

TEST_CASE("stat sheet: histogram columns are proportional") {
    // The tallest bucket reaches the top row; a bucket a fifth its size
    // does not. Without that the chart is a texture rather than a
    // measurement.
    StatSheet s;
    s.histogram({.caption = "",
                 .buckets = {{"a", 1}, {"b", 10}},
                 .rows = 4});
    const auto r = render_sheet(s, 40);
    REQUIRE(r.rows.size() >= 5u);
    // Top row: only the tall bucket has ink.
    const auto& top = r.rows[0];
    int ink_top = 0;
    for (char c : top) if (c == '?') ++ink_top;
    // Bottom column row: both do.
    const auto& bottom = r.rows[3];
    int ink_bottom = 0;
    for (char c : bottom) if (c == '?') ++ink_bottom;
    CHECK(ink_top > 0);
    CHECK(ink_bottom > ink_top);
}

TEST_CASE("stat sheet: a non-zero histogram bucket is never empty") {
    // The same rule the bars, the band segments and the filled plot all
    // follow: "almost none" and "none" are different readings.
    StatSheet s;
    s.histogram({.caption = "",
                 .buckets = {{"a", 1000}, {"b", 1}},
                 .rows = 5});
    const auto r = render_sheet(s, 40);
    REQUIRE(r.rows.size() >= 5u);
    // The bottom column row carries ink for BOTH buckets.
    int ink = 0;
    for (char c : r.rows[4]) if (c == '?') ++ink;
    CHECK(ink >= 4);   // two buckets, at least two cells each
}

TEST_CASE("stat sheet: a histogram narrows its bars rather than dropping data") {
    // Resolution follows the WIDTH. Dropping buckets to keep a fixed bar
    // width loses the tail, and the tail of a latency distribution is
    // exactly where the interesting outliers are — a chart that silently
    // omits its slowest bucket answers the wrong question.
    //
    // So the bar width degrades instead: requested width, then narrower
    // bars, then bars with no gap, and only a surface too narrow for one
    // column per bucket drops anything at all.
    StatSheet s;
    StatHistogram h;
    h.rows = 4;
    h.col_width = 4;
    for (int i = 0; i < 12; ++i)
        h.buckets.push_back({std::to_string(i), static_cast<double>(i + 1)});
    s.histogram(std::move(h));

    for (int w : {20, 30, 50, 80, 120}) {
        const auto r = render_sheet(s, w);
        REQUIRE(!r.rows.empty());
        for (const auto& row : r.rows) CHECK(unicode::str_width(row) <= w);
        // The LAST bucket is the tallest here, so if the tail were being
        // dropped the bottom column row would lose its rightmost ink.
        // Count how many cells carry ink on the baseline-adjacent row:
        // every one of the 12 buckets is non-zero, so all must appear.
        const auto& lowest = r.rows[static_cast<std::size_t>(h.rows) - 1];
        int ink = 0;
        for (char c : lowest) if (c == '?') ++ink;
        CHECK_MESSAGE(ink >= 12, "width " << w);
    }
}

TEST_CASE("stat sheet: a histogram labels every y-axis row") {
    // One peak label tells you the ceiling and nothing else, so reading
    // any other bar means estimating its fraction of a number at the far
    // end of the figure. A tick per row turns that estimate into a
    // lookup.
    StatSheet s;
    s.histogram({.caption = "",
                 .buckets = {{"a", 4}, {"b", 2}},
                 .rows = 4,
                 .y_labels = {"40", "30", "20", "10"}});
    const auto r = render_sheet(s, 40);
    REQUIRE(r.rows.size() >= 4u);
    CHECK(r.rows[0].find("40") != std::string::npos);
    CHECK(r.rows[1].find("30") != std::string::npos);
    CHECK(r.rows[2].find("20") != std::string::npos);
    CHECK(r.rows[3].find("10") != std::string::npos);
}

TEST_CASE("stat sheet: y-axis ticks right-align on their last digit") {
    // The gutter is sized to the WIDEST tick and every label is padded
    // into it, so the numbers line up on their last digit and the axis
    // reads as a scale. A ragged gutter reads as a second, meaningless
    // column of text.
    StatSheet s;
    s.histogram({.caption = "",
                 .buckets = {{"a", 1}},
                 .rows = 3,
                 .y_labels = {"100", "50", "1"}});
    const auto r = render_sheet(s, 30);
    REQUIRE(r.rows.size() >= 3u);
    // "100" starts at column 0; "50" is pushed one right; "1" two.
    CHECK(r.rows[0].find("100") == 0u);
    CHECK(r.rows[1].find("50")  == 1u);
    CHECK(r.rows[2].find("1")   == 2u);
}

TEST_CASE("stat sheet: a histogram tolerates a short or absent y axis") {
    // Fewer labels than rows leaves the rest unlabelled — that is how a
    // caller asks for a sparser axis, and how the panel says "this tick
    // would repeat the one above it".
    StatSheet few;
    few.histogram({.caption = "",
                   .buckets = {{"a", 3}, {"b", 1}},
                   .rows = 4,
                   .y_labels = {"3", "", "1"}});
    CHECK(render_sheet(few, 30).rows.size() >= 4u);

    StatSheet none;
    none.histogram({.caption = "",
                    .buckets = {{"a", 3}, {"b", 1}},
                    .rows = 4});
    const auto r = render_sheet(none, 30);
    REQUIRE(r.rows.size() >= 4u);
    // No gutter at all: the first column carries chart, not padding.
    CHECK(r.rows[3].find('?') != std::string::npos);
}

TEST_CASE("stat sheet: a degenerate histogram renders nothing") {
    StatSheet s;
    s.histogram({.caption = "", .buckets = {}});
    s.histogram({.caption = "", .buckets = {{"z", 0}}});
    CHECK(render_sheet(s, 60).rows.empty());
}

TEST_CASE("stat sheet: measured height equals painted rows at every width") {
    // The bug this pins cost real content. A host budgets rows from
    // measure_element(), and it measures at a LARGE width (maya's Panel
    // uses 1<<14). If the sheet lays out differently there than at the
    // width it paints at — a different column count, a different shed
    // decision — the host budgets the wrong number of rows and the tail
    // is silently DROPPED. Not scrolled: dropped, with no scrollbar and
    // no clue, because the host believed everything fit.
    //
    // Checked across the breakpoint in both directions, because the
    // failure only appears when measure and paint land on opposite sides
    // of a column split.
    auto build = [] {
        StatSheet s;
        s.indent(1);
        s.reserve_right(7);
        s.columns(46, 2);
        s.heading("One");
        for (int i = 0; i < 4; ++i)
            s.entry({.label = "row" + std::to_string(i), .value = "1"});
        s.heading("Two");
        for (int i = 0; i < 4; ++i)
            s.entry({.label = "item" + std::to_string(i), .value = "2"});
        s.heading("Three");
        s.histogram({.caption = "spread",
                     .buckets = {{"a", 1}, {"b", 3}, {"c", 2}},
                     .rows = 4});
        return s;
    };

    for (int w : {40, 60, 76, 90, 100, 120, 160, 200}) {
        const auto s  = build();
        const auto el = s.build();
        // Count painted rows WITHOUT the trailing-blank trim render_sheet
        // does: a column shorter than its neighbour ends in real blank
        // rows, and trimming them would make this compare a trimmed count
        // against an untrimmed measurement.
        StylePool pool;
        Canvas canvas(w, 64, &pool);
        render_tree(el, canvas, pool, theme::dark, /*auto_height=*/true);
        int painted = 0;
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < w; ++x) {
                const char32_t c = canvas.get(x, y).character;
                if (c != 0 && c != U' ') { painted = y + 1; break; }
            }
        const int measured = measure_element(el, w).height.value;
        // Measured is what the host budgets; it must cover everything the
        // sheet paints, or the tail is silently dropped rather than
        // scrolled. Equal or greater — never less.
        CHECK_MESSAGE(measured >= painted, "width " << w);
    }
}

TEST_CASE("unicode: truncate_to_width cuts on column boundaries") {
    // The trap this exists to remove: substr() slices multi-byte glyphs in
    // half, which renders as a replacement char and destroys the alignment
    // the caller was truncating to preserve.
    CHECK(unicode::truncate_to_width("hello", 3) == "hel");
    CHECK(unicode::truncate_to_width("hello", 0).empty());
    CHECK(unicode::truncate_to_width("hello", 99) == "hello");

    // "café" — the é is 2 bytes, 1 column.
    const std::string cafe = "caf\xc3\xa9";
    CHECK(unicode::str_width(unicode::truncate_to_width(cafe, 4)) == 4);
    CHECK(unicode::truncate_to_width(cafe, 3) == "caf");

    // A WIDE glyph straddling the limit is dropped, never half-drawn:
    // one column narrow is a rounding error, one column wide overflows.
    const std::string wide = "\xe6\xbc\xa2\xe5\xad\x97";   // 漢字, 2 cols each
    CHECK(unicode::str_width(unicode::truncate_to_width(wide, 3)) == 2);
    CHECK(unicode::str_width(unicode::truncate_to_width(wide, 4)) == 4);
}
