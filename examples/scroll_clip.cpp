// scroll_clip.cpp — first-class framework scrolling, one-axis (vertical).
//
// Uses the maya scroll primitive:
//   - ScrollState — plain data, holds {x, y, max_x, max_y, step_x, step_y}
//                   and key/mouse handlers.
//   - dsl::scroll(state, viewport) — DSL pipe that wraps any element into
//                                    an overflow:Hidden viewport.
//   - The renderer translates descendants by -scroll_y at paint time and
//     writes max_y back after layout so clamping is automatic.
//
// Compare with scroll_slice.cpp (which emits only visible rows manually).
// Use the slice pattern for large indexable data sets (logs, lists with
// millions of rows). Use the clip pattern — this file — for any other
// content: heterogeneous, markdown, computed, dynamic.
//
// Keys: ↑/↓ row · PgUp/PgDn page · Home/End jump · q quit.

#include <maya/maya.hpp>
#include <maya/widget/scrollbar.hpp>

#include <array>
#include <string>

using namespace maya;
using namespace maya::dsl;

static constexpr std::array<const char*, 30> kLines = {
    "01  the quick brown fox jumps over the lazy dog",
    "02  pack my box with five dozen liquor jugs",
    "03  sphinx of black quartz, judge my vow",
    "04  how vexingly quick daft zebras jump",
    "05  bright vixens jump; dozy fowl quack",
    "06  jackdaws love my big sphinx of quartz",
    "07  the five boxing wizards jump quickly",
    "08  amazingly few discotheques provide jukeboxes",
    "09  heavy boxes perform quick waltzes and jigs",
    "10  jaded zombies acted quaintly but kept driving",
    "11  a quick movement of the enemy will jeopardize six gunboats",
    "12  all questions asked by five watched experts amaze the judge",
    "13  back in june we delivered oxygen equipment of the same size",
    "14  crazy frederick bought many very exquisite opal jewels",
    "15  fix problem quickly with galvanized jets",
    "16  glib jocks quiz nymph to vex dwarf",
    "17  jinxed wizards pluck ivy from the big quilt",
    "18  my girl wove six dozen plaid jackets before she quit",
    "19  six big juicy steaks sizzled in a pan as five workmen left",
    "20  the wizard quickly jinxed the gnomes before they vaporized",
    "21  we promptly judged antique ivory buckles for the next prize",
    "22  waltz, bad nymph, for quick jigs vex",
    "23  watch jeopardy! alex trebek's fun tv quiz game",
    "24  when zombies arrive, quickly fax judge pat",
    "25  woven silk pyjamas exchanged for blue quartz",
    "26  big july earthquakes confound zany experimental vow",
    "27  five quacking zephyrs jolt my wax bed",
    "28  fix problem quickly with galvanized jets",
    "29  jumpy halfling dwarves pick quartz box",
    "30  ── end ──",
};

int main() {
    constexpr int kViewportH = 8;
    ScrollState state;

    auto rows = []() {
        std::vector<Element> out;
        out.reserve(kLines.size());
        for (std::size_t i = 0; i < kLines.size(); ++i) {
            auto row = text(kLines[i]);
            if (i % 2 == 0) row = row | Dim;
            out.push_back(row);
        }
        return out;
    };

    run({.title = "scroll_clip"},
        [&](const Event& ev) {
            if (key(ev, 'q')) return false;
            // Scroll handling is auto-dispatched by the runtime; the
            // event_fn only deals with app-specific keys like 'q'.
            return true;
        },
        [&] {
            const std::string status =
                "y=" + std::to_string(state.y) +
                "/" + std::to_string(state.max_y);

            return v(
                t<"Scrollable viewport — framework primitive"> | Bold | Fg<100, 180, 255>,
                t<"overflow:Hidden + scroll_y; renderer translates at paint time."> | Dim,
                blank_,
                h(
                    v(rows()) | scroll(state, kViewportH) | grow_<1>,
                    scrollbar_y(state, kViewportH)
                ),
                blank_,
                text(status) | Fg<255, 180, 100>,
                t<"↑/↓ row · PgUp/PgDn page · Home/End · q quit"> | Dim
            ) | pad<1> | border_<Round> | bcol<50, 55, 70>;
        }
    );
}
