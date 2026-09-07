#pragma once
// maya::panel::Config — everything a host supplies to build one panel.
//
// Pure data; the widget (widget.hpp) turns it into an Element. Hosts fill a
// Config and never touch layout — the widget owns every chrome decision:
// border, viewport clipping, scrollbar glyphs, keep-selection-in-view.

#include <optional>
#include <string>
#include <vector>

#include "../../element/element.hpp"
#include "../../style/color.hpp"
#include "../scrollbar.hpp"
#include "menu.hpp"
#include "row.hpp"
#include "theme.hpp"

namespace maya::panel {

struct Config {
    std::string title;        // centred on the top border
    std::string subtitle;     // status line above the body

    std::vector<Row> rows;
    // Index into `rows` (or `items`) of the cursor. <0 = no selection.
    int              selected = -1;

    // Pre-built rows, for callers that own their own row rendering
    // (the thread list's virtualisation). Ignored when `rows` is set.
    std::vector<Element> items;

    std::optional<Menu> menu;
    int                 menu_row = -1;

    std::vector<Element> header;   // above the body, never scrolls
    std::string          note;     // below the body
    std::vector<Element> footer;   // key hints

    // Borrowed; must outlive the built Element. Null disables scrolling.
    ScrollState* scroll     = nullptr;
    int          viewport_h = 14;

    // The HOST clamps this to the terminal: a min-width wider than the
    // screen is not a minimum but an overflow, and the overlay centres the
    // panel so the excess is split off both edges and the labels vanish.
    int   min_width = 60;
    Color accent    = Color::blue();

    // Colour of the edge bar on the ACTIVE row (the persistent "currently
    // in use" marker, distinct from the cursor). The cursor wins on
    // overlap — where you ARE outranks where you were.
    Color active_color = Color::bright_magenta();

    Theme          theme{};
    ScrollbarStyle scrollbar_style = ScrollbarStyle::neon();
};

} // namespace maya::panel
