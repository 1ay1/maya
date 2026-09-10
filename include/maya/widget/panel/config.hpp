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
#include "item.hpp"
#include "menu.hpp"
#include "theme.hpp"

namespace maya::panel {

struct Config {
    std::string title;        // centred on the top border
    std::string subtitle;     // status line above the body

    std::vector<Item> items;
    // Index into `items` (or `prebuilt`) of the cursor. <0 = no selection.
    int               selected = -1;

    // Pre-built Elements, for callers that own their own item rendering
    // (the thread list's virtualisation). Ignored when `items` is set.
    std::vector<Element> prebuilt;

    std::optional<Menu> menu;
    int                 menu_row = -1;

    std::vector<Element> header;   // above the body, never scrolls
    std::string          note;     // below the body
    // Overrides the DERIVED editing hint ("editing · ↵ done · ↑↓ next
    // field") while a field is live. For panels where Enter means
    // something else — a single-field form whose Enter SUBMITS — the
    // default would teach the wrong key. Empty = use the default.
    std::string          editing_note;
    std::vector<Element> footer;   // key hints

    // ── Tabs ─────────────────────────────────────────────────────────
    //
    // A panel that shows SEVERAL views of one subject renders a tab strip
    // between the subtitle and the header. Empty = no strip, which is every
    // existing panel: this is additive, and a host that never sets `tabs`
    // builds exactly the frame it built before.
    //
    // It lives in the widget rather than in the one host that needed it
    // first, because a tab strip is CHROME. Every chrome decision in this
    // family — border, viewport clipping, scrollbar glyphs, keeping the
    // selection in view — belongs to the widget, and hosts supply data. A
    // strip hand-rolled into `header` by each host would drift in padding,
    // in the selected-tab treatment, and in how it degrades when the panel
    // is narrower than its labels; three hosts would mean three strips that
    // look almost alike.
    //
    // `tab_active` indexes `tabs`. Out of range renders every tab inactive
    // rather than asserting: a panel is a view, and a bad index should show
    // a slightly wrong strip, not take down the frame.
    std::vector<std::string> tabs;
    int                      tab_active = 0;

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

// NOTE: no compatibility accessors for the renamed members (rows→items,
// items→prebuilt) — the compiler finds every stale spelling, which is the
// point of a hard rename.
