#pragma once
// render_sweep — paint a widget across many widths and assert what must
// hold at EVERY one of them.
//
// WHY THIS EXISTS
// ===============
// A layout bug is not a wrong value in a struct; it is a cell in the wrong
// place on a screen. maya's existing tests assert on trees, sizes and
// hashes, and a whole class of defect is invisible to all three:
//
//   * A row that paints past its slot (text under a scrollbar).
//   * A cell that silently disappears — a meter's VALUE pushed off the end
//     of its row by a bar that grew into the space.
//   * A layout that gets WORSE as the terminal gets wider, because a
//     responsive decision was made against one width and painted at
//     another.
//
// Every one of those shipped at least once. None of them failed a test;
// all of them were obvious the moment the canvas was dumped and counted.
// So this harness paints, then counts.
//
// THE THREE PROPERTIES
// ====================
// fits()       no row exceeds the slot it was painted in
// keeps()      a named string survives at every width
// monotonic()  painted width never DECREASES as the slot grows
//
// The third is the subtle one and the most valuable. A responsive layout
// may legitimately reflow, shrink a bar, or drop a hint — but it must
// never use LESS of a bigger screen than it used of a smaller one. That
// inversion is the signature of measure and paint disagreeing, which is
// the single most expensive bug class this widget family has had.
//
// USING IT
// ========
//     TEST_CASE("my widget behaves across widths") {
//         sweep::Sweep s{[](int w) { return my_widget(w); }};
//         s.fits();
//         s.keeps("42");              // the value must never vanish
//         s.monotonic();
//     }
//
// The builder takes the width because a widget usually needs to be told
// how wide it is; if yours does not, ignore the parameter.

#include <doctest/doctest.h>

#include <maya/element/element.hpp>
#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>
#include <maya/text/unicode_width.hpp>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace maya::sweep {

// The widths every sweep visits.
//
// Not a range: a range is slow and mostly redundant, while a hand-picked
// set hits the places layouts actually change their minds. 40 is a phone
// pane, 80 the historical default (and the renderer's no-context
// fallback — the width a measure pass silently substitutes, so a layout
// that only works there is a layout that only works by accident), 85 and
// 90 straddle a typical two-column threshold, 200 is an ultrawide.
inline constexpr int kWidths[] = {40, 52, 60, 68, 76, 80, 85, 90, 100, 120, 160, 200};

// One painted frame: rows as UTF-8, trailing blanks trimmed.
struct Frame {
    std::vector<std::string> rows;
    int slot = 0;

    // Display COLUMNS, never bytes.
    //
    // The glyphs these widgets are made of — box-drawing rules, block
    // meters, braille plots — are three bytes each. A byte count reads a
    // full 40-column rule as 120 and reports an overflow on every correct
    // frame; it also reads a 21-column dead strip as 63 and sends you
    // hunting a bug that is not there. Both happened.
    [[nodiscard]] static int cols(const std::string& s) {
        return static_cast<int>(unicode::str_width(s));
    }

    // Rightmost painted column across the whole frame.
    [[nodiscard]] int painted_width() const {
        int w = 0;
        for (const auto& r : rows) w = std::max(w, cols(r));
        return w;
    }

    [[nodiscard]] bool contains(const std::string& needle) const {
        for (const auto& r : rows)
            if (r.find(needle) != std::string::npos) return true;
        return false;
    }

    [[nodiscard]] int inked_rows() const {
        int n = 0;
        for (const auto& r : rows) if (!r.empty()) ++n;
        return n;
    }
};

// Paint `el` into a canvas exactly `w` wide.
//
// The canvas IS the slot, deliberately. Painting into a wider canvas hands
// adapt() the canvas width, so anything that grows paints to the canvas
// edge rather than to the slot — and the harness measures itself instead
// of the layout. That mistake produced a confident "every row overflows by
// exactly 20" report that was entirely an artifact of the probe.
//
// auto_height is TRUE because that is what the application passes. It is
// not a detail: the flag changes how the tree is laid out, so a harness
// that left it default would paint a layout the user never sees — passing
// green while the real screen was broken, which is worse than no harness.
// A test rig must render through the same door as the app.
[[nodiscard]] inline Frame paint(const Element& el, int w, int h = 240) {
    StylePool pool;
    Canvas canvas(w, h, &pool);
    canvas.clear();
    render_tree(el, canvas, pool, theme::dark, /*auto_height=*/true);
    Frame f;
    f.slot = w;
    for (int y = 0; y < h; ++y) {
        std::string line;
        for (int x = 0; x < w; ++x) {
            const char32_t c  = canvas.get(x, y).character;
            const char32_t ch = c ? c : U' ';
            if (ch < 0x80) {
                line += static_cast<char>(ch);
            } else if (ch < 0x800) {
                line += static_cast<char>(0xC0 | (ch >> 6));
                line += static_cast<char>(0x80 | (ch & 0x3F));
            } else if (ch < 0x10000) {
                line += static_cast<char>(0xE0 | (ch >> 12));
                line += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                line += static_cast<char>(0x80 | (ch & 0x3F));
            } else {
                line += static_cast<char>(0xF0 | (ch >> 18));
                line += static_cast<char>(0x80 | ((ch >> 12) & 0x3F));
                line += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                line += static_cast<char>(0x80 | (ch & 0x3F));
            }
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        f.rows.push_back(std::move(line));
    }
    while (!f.rows.empty() && f.rows.back().empty()) f.rows.pop_back();
    return f;
}

// A widget painted at every width in kWidths.
class Sweep {
public:
    using Build = std::function<Element(int width)>;

    explicit Sweep(Build build) : build_(std::move(build)) {
        for (int w : kWidths) {
            // COLUMNS is how a widget discovers the terminal outside a
            // render pass — Panel clamps its own min-width against it, and
            // anything calling terminal_cols() reads it. Leaving it unset
            // lets a widget build itself for one width and be painted at
            // another, which is the exact defect these sweeps exist to
            // catch: the harness would manufacture the bug it is testing
            // for and blame the widget.
            const std::string cols = std::to_string(w);
            ::setenv("COLUMNS", cols.c_str(), 1);
            frames_.push_back(paint(build_(w), w));
        }
        ::unsetenv("COLUMNS");
    }

    [[nodiscard]] const std::vector<Frame>& frames() const { return frames_; }

    // ── fits ────────────────────────────────────────────────────────────
    //
    // No row may paint past the slot it was given. Overflow is not a
    // cosmetic issue in a terminal: the cells land under whatever is drawn
    // next (a scrollbar, a frame border), so the symptom is another
    // widget's glyphs being replaced by yours.
    void fits() const {
        for (const auto& f : frames_)
            for (std::size_t i = 0; i < f.rows.size(); ++i)
                REQUIRE_MESSAGE(Frame::cols(f.rows[i]) <= f.slot,
                                "row " << i << " overran slot " << f.slot);
    }

    // ── keeps ───────────────────────────────────────────────────────────
    //
    // A string that must survive every width.
    //
    // For the cells a reader cannot do without — a metric's VALUE, a
    // count, a name. A picture may legitimately shrink and a hint may
    // legitimately drop, but a number that silently disappears leaves the
    // reader with a bar and no idea what it measures. That shipped: a
    // meter's value was pushed off the end of its row by a bar that grew
    // into the space, at every width, and no test noticed.
    void keeps(const std::string& needle) const {
        for (const auto& f : frames_)
            REQUIRE_MESSAGE(f.contains(needle),
                            "'" << needle << "' vanished at width " << f.slot);
    }

    // ── monotonic ───────────────────────────────────────────────────────
    //
    // Painted width must never DECREASE as the slot grows.
    //
    // The most valuable of the three, because it catches the cause rather
    // than a symptom. A layout may reflow, rebalance or shed as it widens
    // — all fine, all still monotonic. What it must never do is use less
    // of a bigger screen than it used of a smaller one: that means a
    // responsive decision was made against one width and painted at
    // another, which is measure and paint disagreeing.
    //
    // Slack tolerates a widget whose content genuinely has a maximum
    // useful width (a centred dialog, a prose column). Pass the ceiling it
    // is allowed to stop growing at; 0 means "must keep growing".
    void monotonic(int content_ceiling = 0) const {
        int prev = 0, prev_slot = 0;
        for (const auto& f : frames_) {
            const int w = f.painted_width();
            if (content_ceiling == 0 || f.slot <= content_ceiling)
                REQUIRE_MESSAGE(w >= prev,
                                "width " << f.slot << " painted " << w
                                << " cols but narrower width " << prev_slot
                                << " painted " << prev
                                << " — a wider slot rendered LESS");
            prev = std::min(w, content_ceiling ? content_ceiling : w);
            prev_slot = f.slot;
        }
    }

    // ── fills ───────────────────────────────────────────────────────────
    //
    // The slot is claimed to within `slack` columns.
    //
    // Stricter than monotonic and not always appropriate — a widget with a
    // deliberate right margin will fail it — so it takes the slack it is
    // allowed. Use it where a layout claims to divide its slot exactly.
    void fills(int slack) const {
        for (const auto& f : frames_) {
            const int w = f.painted_width();
            REQUIRE_MESSAGE(f.slot - w <= slack,
                            "width " << f.slot << " left " << (f.slot - w)
                            << " columns unclaimed (slack " << slack << ")");
        }
    }

    // Print every frame. Not an assertion — the thing you reach for the
    // moment one fails, so it lives beside them rather than in a scratch
    // file that has to be rewritten each time.
    void dump() const {
        for (const auto& f : frames_) {
            MESSAGE("── width " << f.slot << " ──");
            for (const auto& r : f.rows) MESSAGE(r);
        }
    }

private:
    Build build_;
    std::vector<Frame> frames_;
};

} // namespace maya::sweep
