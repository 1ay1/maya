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

// ── The ceiling binds even unsplit ─────────────────────────────────────
//
// A single cell cannot be split, but a ceiling that stops applying when
// there is nothing to split is not a ceiling. Content must still be bounded
// — a row stretched across 200 columns puts its label at one edge and its
// value at the other, which is unreadable in a different way than a clipped
// value rather than an acceptable one.
TEST_CASE("columns: one cell is still bounded by max_width") {
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
    CHECK_MESSAGE(right == 40,
                  "an unsplittable body must still honour the ceiling");
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
