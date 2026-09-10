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
