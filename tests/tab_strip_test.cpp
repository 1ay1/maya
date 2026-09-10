// tab_strip_test — the shared tab strip.
//
// The strip exists because the naive version (a row of styled labels) breaks
// at exactly the size where it matters most. These pin the properties that
// separate it from that version, because they are the ones a host
// re-implementing the strip would get wrong:
//
//   1. the ACTIVE tab is always rendered, whatever the width
//   2. what scrolled past is announced, not silently dropped
//   3. widths are DISPLAY COLUMNS, so non-ASCII labels still align
//
// Rendering note: this file reads the canvas through the same row extractor
// the rest of the widget tests use, which folds every non-ASCII glyph to '?'
// and the full block to '#'. So assertions are about ASCII text, COUNTS of
// marker glyphs, and row shape — never about a specific box-drawing byte.

#include "doctest/doctest.h"

#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>
#include <maya/widget/tab_strip.hpp>

#include <cstdio>
#include <string>
#include <vector>

using namespace maya;

namespace {

struct Rendered {
    std::vector<std::string> rows;
};

// One row of the canvas as text. Non-ASCII folds to '?' (see the file note),
// which is enough to COUNT marker glyphs without asserting on their bytes.
std::string row_text(const Canvas& c, int y, int width) {
    std::string s;
    for (int x = 0; x < width; ++x) {
        const auto cell = c.get(x, y);
        const char32_t ch = cell.character;
        if (ch >= 0x20 && ch < 0x7F) s += static_cast<char>(ch);
        else if (ch == 0 || ch == U' ') s += ' ';
        else s += '?';
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

Rendered render(const TabStrip& s, int width) {
    StylePool pool;
    Canvas canvas(width, 16, &pool);
    render_tree(s.build(), canvas, pool, theme::dark, /*auto_height=*/true);
    Rendered out;
    for (int y = 0; y < 16; ++y) {
        auto r = row_text(canvas, y, width);
        if (r.empty() && out.rows.empty()) continue;   // leading blanks
        out.rows.push_back(std::move(r));
    }
    while (!out.rows.empty() && out.rows.back().empty()) out.rows.pop_back();
    return out;
}

bool has(const Rendered& r, std::string_view needle) {
    for (const auto& row : r.rows)
        if (row.find(needle) != std::string::npos) return true;
    return false;
}

// Count non-ASCII glyph cells in a row — the marker/rule glyphs all fold to
// '?', so this measures the underline's WIDTH without naming its bytes.
int glyphs(const std::string& row) {
    int n = 0;
    for (char c : row) if (c == '?') ++n;
    return n;
}

}  // namespace

TEST_CASE("tab strip: an empty strip renders nothing") {
    TabStrip s;
    const auto r = render(s, 40);
    CHECK(r.rows.empty());
}

TEST_CASE("tab strip: every tab shows when they all fit") {
    TabStrip s;
    s.tab("Smart Mode").tab("Tools").tab("Wire");
    s.active(0);
    const auto r = render(s, 60);
    CHECK(has(r, "Smart Mode"));
    CHECK(has(r, "Tools"));
    CHECK(has(r, "Wire"));
}

TEST_CASE("tab strip: the active tab is visible however narrow the strip") {
    // THE property. A hand-rolled strip renders left-to-right and truncates,
    // so selecting a late tab selects something drawn off-screen — the user
    // is then navigating blind.
    const char* names[] = {"alpha", "bravo", "charlie", "delta",
                           "echo", "foxtrot", "golf", "hotel"};
    TabStrip s;
    for (const char* n : names) s.tab(n);

    for (int active = 0; active < 8; ++active) {
        s.active(active);
        for (int w : {20, 30, 45}) {
            const auto r = render(s, w);
            CHECK_MESSAGE(has(r, names[active]),
                          "active tab must render (active=" << active
                          << " width=" << w << ")");
        }
    }
}

TEST_CASE("tab strip: a narrow strip drops EARLIER tabs, not the active one") {
    // Silently omitting the tail would make the strip lie about how much
    // there is; dropping the active tab would make it lie about where you
    // are. Scrolling from the left is the only option that does neither.
    const char* names[] = {"alpha", "bravo", "charlie", "delta",
                           "echo", "foxtrot", "golf", "hotel"};
    TabStrip s;
    for (const char* n : names) s.tab(n);
    s.active(7);
    const auto r = render(s, 24);
    CHECK(has(r, "hotel"));      // the active tab is in view
    CHECK(!has(r, "alpha"));    // and the ones it scrolled past are not
}

TEST_CASE("tab strip: the underline tracks the active label's width") {
    // The rule row is built in lockstep with the label row, so the two
    // cannot disagree about where a tab starts and ends.
    TabStrip s;
    s.tab("aa").tab("bbbb");
    s.active(1);
    const auto r = render(s, 40);
    REQUIRE(r.rows.size() >= 2);
    // The rule matches 'bbbb' — not 'aa', not the whole row.
    CHECK(glyphs(r.rows[1]) == 4);

    TabStrip t;
    t.tab("aa").tab("bbbb");
    t.active(0);
    const auto r2 = render(t, 40);
    REQUIRE(r2.rows.size() >= 2);
    CHECK(glyphs(r2.rows[1]) == 2);   // and it follows the selection
}

TEST_CASE("tab strip: Dot marking is a single row") {
    // A strip sharing a line budget with content cannot afford the rule.
    TabStrip s;
    s.tab("one").tab("two");
    s.active(0).marker(TabMark::Dot);
    const auto r = render(s, 40);
    CHECK(r.rows.size() == 1);   // Dot mode costs one row
    CHECK(has(r, "one"));
    CHECK(has(r, "two"));
}

TEST_CASE("tab strip: status dots and details render") {
    // The diff reviewer's shape: a per-tab status and a trailing diffstat.
    TabStrip s;
    s.tab("rope.cpp").dot(Color::green()).detail("+12 -3", Color::cyan());
    s.tab("node.hpp");
    s.active(0);
    const auto r = render(s, 60);
    CHECK(has(r, "rope.cpp"));
    CHECK(has(r, "+12 -3"));   // the detail is part of the tab
}

TEST_CASE("tab strip: a detail participates in the width arithmetic") {
    // The subtle one. A host appending its own trailing text AFTER the
    // widget measured would scroll by the wrong amount and push the active
    // tab off the edge — which is precisely why detail is a field and not
    // something the caller concatenates into the label.
    TabStrip s;
    for (int i = 0; i < 8; ++i)
        s.tab("file" + std::to_string(i)).detail("+100 -100", Color::cyan());
    s.active(7);
    const auto r = render(s, 30);
    CHECK(has(r, "file7"));   // the active tab still fits once details count
}

TEST_CASE("tab strip: the three marks are visibly different") {
    // A family, not one house style — so this both PINS each mode's shape
    // and prints them together, because "do these read as three
    // deliberate styles" is a judgement no assertion makes for you.
    auto build = [](TabMark m) {
        TabStrip s;
        s.tab("rope.cpp").tab("node.hpp").tab("notes.md");
        s.active(1).marker(m);
        return s;
    };

    const auto und = render(build(TabMark::Underline), 60);
    const auto dot = render(build(TabMark::Dot), 60);
    const auto edt = render(build(TabMark::Editor), 60);

    std::printf("\n--- Underline ---\n");
    for (const auto& r : und.rows) std::printf("|%s|\n", r.c_str());
    std::printf("--- Dot ---\n");
    for (const auto& r : dot.rows) std::printf("|%s|\n", r.c_str());
    std::printf("--- Editor ---\n");
    for (const auto& r : edt.rows) std::printf("|%s|\n", r.c_str());

    CHECK(und.rows.size() == 2);   // label + rule under the active label
    CHECK(dot.rows.size() == 1);   // one row, marker inline
    CHECK(edt.rows.size() == 2);   // label + full-width rule

    // Every mode shows every tab when they fit.
    for (const auto* r : {&und, &dot, &edt}) {
        CHECK(has(*r, "rope.cpp"));
        CHECK(has(*r, "node.hpp"));
        CHECK(has(*r, "notes.md"));
    }

    // The editor rule spans the WHOLE width — a rule that stopped where the
    // tabs stop would leave a ragged edge reading as a broken strip.
    CHECK(glyphs(edt.rows[1]) == 60);
    // The underline rule covers only the active label.
    CHECK(glyphs(und.rows[1]) == 8);   // "node.hpp"
}

TEST_CASE("tab strip: editor mode still keeps the active tab in view") {
    // The dividers cost columns, so the scroll arithmetic has to account
    // for them — a mode that measured gaps but rendered dividers would
    // overflow by 1 column per tab and push the active one off the edge.
    TabStrip s;
    for (int i = 0; i < 10; ++i) s.tab("buffer" + std::to_string(i) + ".cpp");
    s.marker(TabMark::Editor);
    for (int active : {0, 4, 9}) {
        s.active(active);
        for (int w : {28, 40, 70}) {
            const auto r = render(s, w);
            CHECK_MESSAGE(has(r, "buffer" + std::to_string(active) + ".cpp"),
                          "editor mode keeps the active tab (i=" << active
                          << " w=" << w << ")");
        }
    }
}

TEST_CASE("tab strip: an out-of-range active index never crashes") {
    // A panel is a VIEW: a bad index should show a slightly wrong strip,
    // not take the frame down.
    TabStrip s;
    s.tab("one").tab("two");
    for (int bad : {-5, -1, 2, 99}) {
        s.active(bad);
        const auto r = render(s, 40);
        CHECK(has(r, "one"));
        CHECK(has(r, "two"));
    }
}

TEST_CASE("tab strip: non-ASCII labels underline by display columns") {
    // Measuring bytes would underline ~3x too long on these.
    TabStrip s;
    s.tab("\xe6\x97\xa5\xe6\x9c\xac");   // 日本 — 2 chars, 4 columns
    s.active(0);
    const auto r = render(s, 40);
    REQUIRE(r.rows.size() >= 2);
    // Two wide glyphs underline as four columns.
    CHECK(glyphs(r.rows[1]) == 4);
}
