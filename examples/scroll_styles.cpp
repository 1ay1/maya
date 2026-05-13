// scroll_styles.cpp — showcase every built-in scrollbar style preset.
//
// Compact tile layout: each preset gets a small labeled card showing its
// vertical bar + horizontal bar at the current scroll position. All
// tiles share one ScrollState so they move together. Press ↑/↓/←/→ to
// scroll; every tile updates in lock-step.
//
// To add a custom style, define a static factory on ScrollbarStyle in
// widget/scrollbar.hpp (5 lines) and append it to kPresets below.
//
// To build a fully custom scrollbar widget (gradient thumb, percentage
// label, click-to-jump, etc.), skip ScrollbarStyle entirely:
//
//     Element my_bar(const ScrollState& s, int viewport_h) {
//         int thumb_h = std::max(1, viewport_h * viewport_h /
//                                    (viewport_h + s.max_y));
//         int thumb_y = (viewport_h - thumb_h) * s.y /
//                       std::max(1, s.max_y);
//         // emit any Element from these.
//     }

#include <maya/maya.hpp>
#include <maya/widget/scrollbar.hpp>

#include <array>
#include <string>

using namespace maya;
using namespace maya::dsl;

struct Preset {
    const char*     name;
    ScrollbarStyle  style;
};

static const std::array<Preset, 13> kPresets = {{
    {"line",        ScrollbarStyle::line()},
    {"block",       ScrollbarStyle::block()},
    {"slim",        ScrollbarStyle::slim()},
    {"heavy",       ScrollbarStyle::heavy()},
    {"double_line", ScrollbarStyle::double_line()},
    {"dotted",      ScrollbarStyle::dotted()},
    {"dashed",      ScrollbarStyle::dashed()},
    {"braille",     ScrollbarStyle::braille()},
    {"ascii",       ScrollbarStyle::ascii()},
    {"shadow",      ScrollbarStyle::shadow()},
    {"minimal",     ScrollbarStyle::minimal()},
    {"neon",        ScrollbarStyle::neon()},
    {"retro",       ScrollbarStyle::retro()},
    // danger() and pixel() are also available — drop them in here.
}};

int main() {
    // Small viewports so each tile stays compact.
    constexpr int kTileH = 6;
    constexpr int kTileW = 12;

    ScrollState state;
    // Prime the maxes so the tiles show meaningful thumb positions from
    // the very first frame, before any real content is wired up. The
    // writeback below will overwrite these with the actual content
    // extents once the first paint completes.
    state.max_x = 24;
    state.max_y = 18;
    state.step_x = 2;
    state.step_y = 1;

    // No manual state.handle(*ev) plumbing — the framework's auto-
    // dispatch forwards every input to scroll states that were painted
    // in the previous frame. Just quit on 'q' and the rest is wired.
    run({.title = "scroll_styles"},
        [&](const Event& ev) {
            if (key(ev, 'q')) return false;
            return true;
        },
        [&] {
            // Each tile is a labeled card showing both axes' bars.
            //   ┌─name──────────┐
            //   │ │ │ │         │  <- vertical bar (kTileH rows tall)
            //   │ ┃ │ │         │
            //   │ ┃ │ │         │
            //   │ │ │ │         │
            //   │ │ │ │         │
            //   │ │ │ │         │
            //   │ ──━━━━──      │  <- horizontal bar (kTileW cols wide)
            //   └───────────────┘
            auto tile = [&](const Preset& p) {
                return v(
                    text(p.name) | Bold | Fg<140, 180, 255>,
                    scrollbar_y(state, kTileH, p.style),
                    scrollbar_x(state, kTileW, p.style)
                ) | pad<0, 1> | border_<Single> | bcol<70, 75, 90>;
            };

            // Pack into rows of 4 tiles so the showcase fits in ~70 cols.
            constexpr int kPerRow = 4;
            std::vector<Element> rows;
            for (std::size_t i = 0; i < kPresets.size(); i += kPerRow) {
                std::vector<Element> row_cells;
                for (std::size_t j = i; j < i + kPerRow && j < kPresets.size(); ++j) {
                    row_cells.push_back(tile(kPresets[j]));
                }
                rows.push_back(h(std::move(row_cells)).build());
            }

            const std::string status =
                "y=" + std::to_string(state.y) + "/" + std::to_string(state.max_y) +
                "  x=" + std::to_string(state.x) + "/" + std::to_string(state.max_x);

            return v(
                t<"Scrollbar style showcase"> | Bold | Fg<100, 180, 255>,
                t<"13 presets. All bars share one ScrollState."> | Dim,
                blank_,
                v(std::move(rows)),
                blank_,
                text(status) | Fg<255, 180, 100>,
                t<"↑/↓/←/→ scroll · PgUp/PgDn · Home/End · q quit"> | Dim
            ) | pad<1> | border_<Round> | bcol<50, 55, 70>;
        }
    );
}
