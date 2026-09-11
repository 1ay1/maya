// test_columns — maya::columns(), newspaper flow with a WIDTH CEILING.
//
// The contract under test, in the order it matters:
//
//   1. EXACT FILL. columns + gaps == the slot width. Not "close to" — the
//      whole point of stating a MAXIMUM column width rather than a minimum
//      is that the count is chosen first and the entire slot is then divided
//      among exactly that many columns, so no strip is left unclaimed.
//
//   2. THE CEILING HOLDS. No column is ever wider than max_width. When one
//      would be, another column is added. That is the rule that picks the
//      count, so it is the rule most worth asserting at every width.
//
//   3. INERTNESS. Below the split threshold the result is byte-identical to
//      the plain stack — not merely similar. Every existing vstack caller is
//      entitled to that, and it is what makes adopting this safe.
//
//   4. NOTHING IS CLIPPED. The defect this primitive replaces DROPPED
//      characters in a narrow band of widths (a stats value "6.0s" rendered
//      as "6"), because the cell was measured at one width and resolved at
//      another. So the text of every cell is checked at every width from 40
//      to 200, which sweeps every count transition.

#include <doctest/doctest.h>

#include <maya/element/grid.hpp>
#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>
#include <maya/dsl.hpp>
#include <maya/text/unicode_width.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace maya;

namespace {

// Paint an element at `w` columns and return the rows as UTF-8, trailing
// blanks trimmed. Painting rather than inspecting the tree is deliberate:
// the bug this replaces was invisible in the tree and only showed on screen.
std::vector<std::string> paint(const Element& el, int w) {
    StylePool pool;
    // The canvas IS the slot. A wider canvas would hand adapt() the canvas
    // width, and any element that grows (a separator, a meter) would then
    // paint to the canvas edge rather than to the slot — measuring the
    // harness instead of the layout.
    Canvas canvas(w, 200, &pool);
    canvas.clear();
    render_tree(el, canvas, pool, theme::dark);

    std::vector<std::string> rows;
    for (int y = 0; y < 200; ++y) {
        std::string line;
        for (int x = 0; x < w; ++x) {
            const char32_t c  = canvas.get(x, y).character;
            const char32_t ch = c ? c : U' ';
            if (ch < 0x80) line += static_cast<char>(ch);
            else if (ch < 0x800) {
                line += static_cast<char>(0xC0 | (ch >> 6));
                line += static_cast<char>(0x80 | (ch & 0x3F));
            } else {
                line += static_cast<char>(0xE0 | (ch >> 12));
                line += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                line += static_cast<char>(0x80 | (ch & 0x3F));
            }
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        rows.push_back(std::move(line));
    }
    while (!rows.empty() && rows.back().empty()) rows.pop_back();
    return rows;
}

int inked(const std::vector<std::string>& rows) {
    int n = 0;
    for (const auto& r : rows) if (!r.empty()) ++n;
    return n;
}

// Display COLUMNS of a painted row, not bytes. The box-drawing glyphs a
// separator paints are 3 bytes each, so a byte count would read a full 40
// column rule as 120 and call every exact fill an overflow.
int cols_of(const std::string& s) { return static_cast<int>(unicode::str_width(s)); }

bool has(const std::vector<std::string>& rows, const std::string& needle) {
    for (const auto& r : rows)
        if (r.find(needle) != std::string::npos) return true;
    return false;
}

// A cell of known height whose lines are long enough to reveal clipping:
// if a column is laid out narrower than it was measured, the tail is what
// disappears, so each line ENDS with a marker.
Element cell(const std::string& tag, int lines) {
    using namespace dsl;
    std::vector<Element> ls;
    for (int i = 0; i < lines; ++i)
        ls.push_back(text(tag + std::to_string(i)).build());
    return v(std::move(ls)).build();
}

std::vector<Element> make(std::initializer_list<std::pair<const char*, int>> spec) {
    std::vector<Element> out;
    for (auto [tag, n] : spec) out.push_back(cell(tag, n));
    return out;
}

} // namespace

// ── 3. Inertness ─────────────────────────────────────────────────────────
//
// First, because it is the guarantee every other caller depends on. A slot
// at or under max_width yields one column, and one column must be the plain
// stack ITSELF — no wrapper, nothing that could perturb an existing layout.
TEST_CASE("columns: a slot within max_width is inert") {
    auto flowed = paint(columns(make({{"alpha", 3}, {"beta", 3}}), 60).build(), 50);
    auto plain  = paint(dsl::v(make({{"alpha", 3}, {"beta", 3}})).build(), 50);
    CHECK_MESSAGE(flowed == plain,
                  "at or below max_width, flow must change nothing at all");
}

// ── 2. The ceiling holds ───────────────────────────────────────────────
//
// Six equal cells so every count divides evenly and the HEIGHT is an exact
// witness to the column count: 1 col = 24 rows, 2 = 12, 3 = 8. (Four cells
// would not witness it — 4 into 3 columns still leaves one holding two, so
// the height would not fall. Choosing a divisible case is what makes the
// assertion mean what it says.)
TEST_CASE("columns: a column is added rather than exceeding max_width") {
    const int kMax = 30, kGap = 2;
    auto six = [] { return make({{"a", 4}, {"b", 4}, {"c", 4},
                                 {"d", 4}, {"e", 4}, {"f", 4}}); };
    CHECK(inked(paint(columns(six(), kMax).build(), kMax)) == 24);
    CHECK(inked(paint(columns(six(), kMax).build(), kMax * 2 + kGap)) == 12);
    CHECK(inked(paint(columns(six(), kMax).build(), kMax * 3 + kGap * 2)) == 8);
}

// ── 1. Exact fill ─────────────────────────────────────────────────────
//
// A cell that paints its full width makes the geometry visible: with an
// exact fill the LAST column ends flush with the slot edge, so the rightmost
// painted cell sits at w-1 for EVERY width. A minimum-width flow fails this
// — it fits as many columns as pay for themselves and leaves the remainder
// as an unclaimed strip nobody owns.
TEST_CASE("columns: the slot is claimed exactly, at every width") {
    for (int w = 40; w <= 200; ++w) {
        std::vector<Element> cs;
        for (int i = 0; i < 6; ++i) {
            using namespace dsl;
            // A separator grows to whatever width it is given, so it reports
            // the column's real RESOLVED width rather than a guess — which
            // is the whole question here.
            std::vector<Element> parts;
            parts.push_back(text(std::string("c") + std::to_string(i)).build());
            parts.push_back(separator().build());
            cs.push_back(v(std::move(parts)).build());
        }
        auto rows = paint(columns(std::move(cs), 40).build(), w);

        int right = 0;
        for (const auto& r : rows) right = std::max(right, cols_of(r));
        REQUIRE(right <= w);   // never past the edge
        REQUIRE(right == w);   // and never short of it
    }
}

// ── 4. Nothing is clipped ────────────────────────────────────────────
//
// The regression guard. Sweeps every count transition; at each width every
// line of every cell must be present in full.
TEST_CASE("columns: no cell is clipped at any width") {
    for (int w = 40; w <= 200; ++w) {
        auto rows = paint(columns(make({{"alpha", 3}, {"bravo", 5},
                                        {"charlie", 2}, {"delta", 4}}),
                                  40).build(), w);
        for (const auto& r : rows) REQUIRE(cols_of(r) <= w);
        for (auto [tag, n] : {std::pair<const char*, int>{"alpha", 3},
                              {"bravo", 5}, {"charlie", 2}, {"delta", 4}})
            for (int i = 0; i < n; ++i)
                REQUIRE(has(rows, std::string(tag) + std::to_string(i)));
    }
}

// ── The ceiling never costs width ────────────────────────────────────
//
// max_width decides HOW MANY columns there are. It is not a margin. When a
// body cannot split — one cell, or max_cols == 1 — there is no second column
// for withheld width to go to, so capping would leave a strip of the slot
// belonging to nobody: the same defect this primitive exists to prevent,
// arriving from the other side.
//
// This is the regression guard for a real bug: capping an unsplittable body
// cost a one-section stats tab 19 columns at width 90 — the row stopped at
// 65 with its value stranded mid-row, AND its label truncated, because the
// lane it shared had been shrunk to a width the slot never asked for.
TEST_CASE("columns: an unsplittable body still gets the whole slot") {
    std::vector<Element> one;
    {
        using namespace dsl;
        std::vector<Element> parts;
        parts.push_back(text("only").build());
        parts.push_back(separator().build());   // grows to whatever it is given
        one.push_back(v(std::move(parts)).build());
    }
    auto rows = paint(columns(std::move(one), 40).build(), 200);

    int right = 0;
    for (const auto& r : rows) right = std::max(right, cols_of(r));
    CHECK_MESSAGE(right == 200,
                  "a ceiling that cannot buy a split must not cost width");
}

// ── Balance ──────────────────────────────────────────────────────────
//
// Splitting on COUNT rather than height is what looks like a layout bug: one
// 9-line cell against three 1-line cells gives 9 rows beside 3. A height
// balance cannot beat the tall cell itself, but must not do worse than it.
TEST_CASE("columns: balanced by height, not by cell count") {
    auto rows = paint(
        columns(make({{"tall", 9}, {"s", 1}, {"t", 1}, {"u", 1}}), 30).build(), 62);
    CHECK_MESSAGE(inked(rows) <= 9,
                  "split by count, leaving one column twice the other");
}

// ── viewport(): even count-split, responsive columns ──────────────────
//
// viewport() shares columns()' width CEILING (count chosen so no column
// exceeds max_width) but fills HORIZONTALLY first — cells go left-to-right
// across the columns, then wrap to the next row (a real grid) — instead of
// balancing by height. These tests pin that contract.

namespace {

// The column (0-based visual x) a single-line tag starts at, or -1 if the
// tag never appears. Used to prove WHICH column a cell landed in.
int tag_col(const std::vector<std::string>& rows, const std::string& tag) {
    for (const auto& r : rows) {
        auto pos = r.find(tag);
        if (pos == std::string::npos) continue;
        // Byte offset == column here: tags are ASCII and left-padding is
        // spaces, both one column per byte up to the tag.
        return static_cast<int>(unicode::str_width(r.substr(0, pos)));
    }
    return -1;
}

// Distinct set of start-columns across the given tags = the column count
// those cells were spread over.
int distinct_cols(const std::vector<std::string>& rows,
                  std::initializer_list<const char*> tags) {
    std::vector<int> xs;
    for (auto* t : tags) {
        int c = tag_col(rows, std::string(t) + "0");
        if (c >= 0 && std::find(xs.begin(), xs.end(), c) == xs.end())
            xs.push_back(c);
    }
    return static_cast<int>(xs.size());
}

} // namespace

TEST_CASE("viewport: the ceiling picks the column count") {
    // Six one-line cells, ceiling 30, no gap so the transitions are the
    // clean multiples: <=30 one col, <=60 two, <=90 three.
    auto cells = [] {
        return make({{"a",1},{"b",1},{"c",1},{"d",1},{"e",1},{"f",1}});
    };
    auto opt = [](int w) {
        return ViewportOpts{.max_width = 30, .gap = 0};
    };

    auto r1 = paint(viewport(cells(), opt(0)).build(), 28);
    CHECK_MESSAGE(distinct_cols(r1, {"a","b","c","d","e","f"}) == 1,
                  "at/under the ceiling: one column");

    auto r2 = paint(viewport(cells(), opt(0)).build(), 60);
    CHECK_MESSAGE(distinct_cols(r2, {"a","b","c","d","e","f"}) == 2,
                  "over one ceiling: two columns");

    auto r3 = paint(viewport(cells(), opt(0)).build(), 90);
    CHECK_MESSAGE(distinct_cols(r3, {"a","b","c","d","e","f"}) == 3,
                  "over twice the ceiling: three columns");
}

TEST_CASE("viewport: fills horizontally first, columns stay aligned") {
    // Horizontal-first over two columns: rows [a,b] [c,d] [e,f]. So a,c,e
    // land in the left column x and b,d,f in the right — same column
    // membership a round-robin would give, but reached by filling rows.
    auto rows = paint(
        viewport(make({{"a",1},{"b",1},{"c",1},{"d",1},{"e",1},{"f",1}}),
                 ViewportOpts{.max_width = 30, .gap = 0}).build(), 60);

    const int la = tag_col(rows, "a0");
    const int lb = tag_col(rows, "b0");
    CHECK(la >= 0);
    CHECK(lb > la);                                // right column is further right
    CHECK_MESSAGE(tag_col(rows, "c0") == la, "c is under a, column 0");
    CHECK_MESSAGE(tag_col(rows, "e0") == la, "e is under c, column 0");
    CHECK_MESSAGE(tag_col(rows, "d0") == lb, "d is under b, column 1");
    CHECK_MESSAGE(tag_col(rows, "f0") == lb, "f is under d, column 1");
}

TEST_CASE("viewport: a wrapped cell starts a fresh aligned row") {
    // The defining difference from round-robin columns: with a TALL first
    // cell, the cell that wraps to the next row must sit BELOW that whole
    // row (grid band), not stacked directly under the tall cell in its
    // column. Four cells over two columns: row0 = [tall(3), b(1)],
    // row1 = [c(1), d(1)]. c must begin at or after the tall cell's bottom.
    auto rows = paint(
        viewport(make({{"tall",3},{"b",1},{"c",1},{"d",1}}),
                 ViewportOpts{.max_width = 30, .gap = 0, .gap_y = 0}).build(), 60);

    // Row of a tag = index of the row whose text contains it.
    auto row_of = [&](const std::string& t) {
        for (int i = 0; i < static_cast<int>(rows.size()); ++i)
            if (rows[static_cast<std::size_t>(i)].find(t) != std::string::npos) return i;
        return -1;
    };
    const int tall_last = row_of("tall2");   // last line of the 3-line cell
    const int c_first   = row_of("c0");      // first line of the wrapped cell
    CHECK(tall_last >= 0);
    CHECK(c_first > tall_last);
    CHECK_MESSAGE(c_first > tall_last,
                  "wrapped cell starts a new row below the tall cell, not beside it");
}

TEST_CASE("viewport: equal_rows makes a row's cells the same height") {
    // Two bordered cards of different natural heights in one row. With
    // equal_rows the shorter card is stretched to the taller one, so both
    // borders close on the SAME row — a clean grid, not ragged boxes.
    using namespace dsl;
    auto boxed = [](const char* tag, int lines) {
        std::vector<Element> ls;
        for (int i = 0; i < lines; ++i)
            ls.push_back(text(std::string(tag) + std::to_string(i)).build());
        return (v(std::move(ls)) | border_<Round>).build();
    };

    std::vector<Element> cells;
    cells.push_back(boxed("a", 2));   // short
    cells.push_back(boxed("b", 6));   // tall
    auto rows = paint(
        viewport(std::move(cells),
                 ViewportOpts{.max_width = 40, .gap = 2, .equal_rows = true}).build(),
        90);

    // Each card's left border sits one column left of its text. The card's
    // bottom is the LAST row that inks anything at that column. With
    // equal_rows both cards must bottom out on the same row.
    const int xa = std::max(0, tag_col(rows, "a0") - 1);
    const int xb = std::max(0, tag_col(rows, "b0") - 1);
    auto bottom_at = [&](int x) {
        int last = -1;
        for (int y = 0; y < static_cast<int>(rows.size()); ++y) {
            const std::string& r = rows[static_cast<std::size_t>(y)];
            if (x < static_cast<int>(r.size()) && r[static_cast<std::size_t>(x)] != ' ')
                last = y;
        }
        return last;
    };
    const int ba = bottom_at(xa);
    const int bb = bottom_at(xb);
    CHECK(ba > 0);
    CHECK(xb > xa);
    CHECK_MESSAGE(ba == bb, "both cards' bottom borders sit on the same row");
}

TEST_CASE("viewport: Flow::Column fills top-to-bottom, contiguous columns") {
    // Same six cells, two columns, but column-major: column 0 gets the first
    // contiguous run [a,b,c] top-to-bottom, column 1 gets [d,e,f]. So a,b,c
    // share the LEFT column x and d,e,f the RIGHT — the transpose of the
    // row-major layout.
    auto rows = paint(
        viewport(make({{"a",1},{"b",1},{"c",1},{"d",1},{"e",1},{"f",1}}),
                 ViewportOpts{.max_width = 30, .gap = 0, .flow = Flow::Column}).build(),
        60);

    const int la = tag_col(rows, "a0");
    const int ld = tag_col(rows, "d0");
    CHECK(la >= 0);
    CHECK(ld > la);                                // second column is further right
    CHECK_MESSAGE(tag_col(rows, "b0") == la, "b runs down column 0 under a");
    CHECK_MESSAGE(tag_col(rows, "c0") == la, "c runs down column 0 under b");
    CHECK_MESSAGE(tag_col(rows, "e0") == ld, "e runs down column 1 under d");
    CHECK_MESSAGE(tag_col(rows, "f0") == ld, "f runs down column 1 under e");

    // And it IS the transpose of Row: in row-major, a and d share NO column.
    auto rrows = paint(
        viewport(make({{"a",1},{"b",1},{"c",1},{"d",1},{"e",1},{"f",1}}),
                 ViewportOpts{.max_width = 30, .gap = 0, .flow = Flow::Row}).build(),
        60);
    CHECK_MESSAGE(tag_col(rrows, "b0") != tag_col(rrows, "a0"),
                  "row-major puts b in the OTHER column from a");
}

TEST_CASE("viewport: columns differ by at most one cell") {
    // Five cells over three columns: 2 | 2 | 1, never 3 | 1 | 1.
    // ceiling 30 at width 90: k=3 is the smallest with ceil(90/k) <= 30.
    auto rows = paint(
        viewport(make({{"a",1},{"b",1},{"c",1},{"d",1},{"e",1}}),
                 ViewportOpts{.max_width = 30, .gap = 0}).build(), 90);

    // Group the five tags by their start column, count per column.
    std::vector<std::pair<int,int>> per;   // (x, count)
    for (const char* t : {"a","b","c","d","e"}) {
        int x = tag_col(rows, std::string(t) + "0");
        REQUIRE(x >= 0);
        auto it = std::find_if(per.begin(), per.end(),
                               [&](auto& p){ return p.first == x; });
        if (it == per.end()) per.push_back({x, 1});
        else ++it->second;
    }
    REQUIRE_MESSAGE(per.size() == 3, "exactly three columns");
    int lo = 99, hi = 0;
    for (auto& [x, c] : per) { lo = std::min(lo, c); hi = std::max(hi, c); }
    CHECK_MESSAGE(hi - lo <= 1, "evenly split: no column holds two more than another");
}

TEST_CASE("viewport: exact fill, nothing clipped") {
    // Sweep every width; the widest inked row must equal the slot (no
    // unclaimed strip) and every tag must survive (no clipped tail).
    for (int w = 40; w <= 200; ++w) {
        auto rows = paint(
            viewport(make({{"aaaa",1},{"bbbb",1},{"cccc",1},{"dddd",1}}),
                     ViewportOpts{.max_width = 30, .gap = 2}).build(), w);
        for (const char* t : {"aaaa0","bbbb0","cccc0","dddd0"})
            CHECK_MESSAGE(has(rows, t),
                          "cell " << t << " survived at width " << w);
    }
}

TEST_CASE("viewport: fewer cells than the width affords spread evenly") {
    // Width 66, ceiling 20 => the viewport is wide enough for 3 columns.
    // With only TWO cells they must sit as two capped, evenly-spaced
    // columns — NOT two half-screen cells each stretched to ~33 wide.
    auto rows = paint(
        viewport(make({{"aa",1},{"bb",1}}),
                 ViewportOpts{.max_width = 20, .gap = 2}).build(), 66);

    // Both cells present, in different columns.
    const int la = tag_col(rows, "aa0");
    const int lb = tag_col(rows, "bb0");
    CHECK(la >= 0);
    CHECK(lb > la);

    // Neither column is stretched past the ceiling: the widest inked row is
    // at most 2*max_width + gap = 42, well under the 66-wide slot. A
    // stretch-to-fill layout would ink the full 66.
    int widest = 0;
    for (const auto& r : rows) widest = std::max(widest, cols_of(r));
    CHECK_MESSAGE(widest <= 2 * 20 + 2,
                  "two cards stay capped at max_width, not stretched to fill");
    CHECK_MESSAGE(widest < 66,
                  "trailing space is left empty, not swallowed by the cards");
}

TEST_CASE("viewport: a lone cell keeps its width, is not blown up") {
    // One cell in a wide viewport: a single capped column, not one cell
    // stretched across the whole slot.
    auto rows = paint(
        viewport(make({{"solo",1}}),
                 ViewportOpts{.max_width = 18, .gap = 2}).build(), 90);
    int widest = 0;
    for (const auto& r : rows) widest = std::max(widest, cols_of(r));
    CHECK_MESSAGE(widest <= 18,
                  "a lone card is at most max_width, never the full slot");
}

TEST_CASE("viewport: one column is the plain stack") {
    // Below the split threshold the flow must be inert: byte-identical to a
    // plain vstack, so adopting it can never disturb a narrow layout.
    auto cells = make({{"x",2},{"y",2},{"z",2}});
    auto other = make({{"x",2},{"y",2},{"z",2}});

    auto vp    = paint(viewport(std::move(cells), ViewportOpts{.max_width = 80}).build(), 40);
    auto plain = paint(dsl::v(std::move(other)).build(), 40);
    CHECK_MESSAGE(vp == plain, "one-column viewport == plain stack, byte for byte");
}

