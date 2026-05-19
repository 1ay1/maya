#pragma once
// maya::widget::Picker — bordered modal picker with scrollable result list.
//
// Reducer-friendly: stateless except for a borrowed ScrollState pointer.
// Host (e.g. agentty) owns the ScrollState as `mutable` on its Model
// and hands a pointer in via Config. maya owns every chrome decision —
// border style, scrollbar glyphs, scrollable region clipping, viewport
// height enforcement, keep-selection-in-view auto-scroll.
//
// Three optional regions stack vertically inside the border:
//
//     ┌─ title ──────────────────────────┐
//     │  HEADER (zero or more rows)      │   ← e.g. query line + separator
//     │  ─────────────────────────────── │
//     │  ITEM 0                        ▲ │   ← scrollable, paired w/ scrollbar
//     │  ITEM 1  ← selected            ┃ │
//     │  ITEM 2                        ┃ │
//     │  ITEM 3                        ▽ │
//     │                                  │
//     │  FOOTER (zero or more rows)      │   ← e.g. " ↑↓ move · Enter open "
//     └──────────────────────────────────┘
//
// The scrollable region is exactly `viewport_h` rows tall regardless of
// how many items it holds. Items beyond that scroll via maya's standard
// ScrollState mechanism (arrow keys + mouse wheel auto-dispatched as long
// as the bar is painted, plus drag-the-thumb if the host opts into mouse
// handling for the state).
//
// Auto-scroll: before paint, the widget clamps `state->y` so the
// `selected_index` row sits inside the viewport. Manual scrolling
// (cursor on-screen → user scrolls down via wheel) is preserved
// because the clamp only fires when the selection is actually outside.
//
// Usage:
//
//   Picker::Config cfg;
//   cfg.title       = " Models ";
//   cfg.accent      = Color::cyan();
//   cfg.min_width   = 40;
//   cfg.header      = { query_line, separator };
//   cfg.items       = model_rows;          // already-built Elements
//   cfg.selected    = picker.cursor;
//   cfg.footer      = { hint_row };
//   cfg.scroll      = &m.ui.model_picker_scroll;
//   cfg.viewport_h  = 14;
//   return Picker{std::move(cfg)}.build();

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "../core/scroll_state.hpp"
#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../style/border.hpp"
#include "../style/color.hpp"

#include "scrollbar.hpp"

namespace maya {

class Picker {
public:
    struct Config {
        // Border title shown centred on the top edge. Pad with spaces
        // (" Models ") so the corner joiners breathe.
        std::string                  title;

        // Border + accent color. Single source of truth — the same
        // value paints the border and any caller-decided accents on
        // the header/items can reference it independently.
        Color                        accent       = Color::white();

        // Minimum picker width in cols. The widget never sets a fixed
        // or max width — the parent's cross-axis Stretch (Overlay's
        // bg-vstack pattern) drives the actual rendered width. This
        // is the floor for narrow terminals so rows stay readable.
        int                          min_width    = 50;

        // Static rows above the scrollable list (e.g. search input
        // line + separator). Painted as-is, never clipped by scroll.
        std::vector<Element>         header;

        // The scrollable list. Each Element is one row. The widget
        // wraps these in a viewport `viewport_h` rows tall, paired
        // with a vertical scrollbar that appears only when the list
        // overflows the viewport.
        std::vector<Element>         items;

        // 0-based index into `items` of the currently-selected row.
        // Used solely to keep the selection inside the viewport via
        // auto-scroll on paint. Negative ⇒ no selection (no clamp).
        // Out-of-range values are ignored (treated as no-selection).
        int                          selected     = -1;

        // Static rows below the scrollable list (e.g. " ↑↓ move " hint,
        // or a "N/total" position indicator). Painted as-is.
        std::vector<Element>         footer;

        // Borrowed pointer to the host's ScrollState. Required when
        // `items.size() > 0` — the widget mutates state->y to keep
        // the selection visible, and reads state->max_y / bar bounds
        // after maya's writeback. Lifetime contract: the state must
        // outlive the Element this Config builds. A null pointer
        // disables scrolling entirely (the list paints as a static
        // block — useful only when items.size() ≤ viewport_h is a
        // structural invariant for the caller).
        ScrollState*                 scroll       = nullptr;

        // Height of the scrollable region in rows. Items beyond this
        // are reachable via the scrollbar. Capped to ≥ 1 by build().
        int                          viewport_h   = 14;

        // Scrollbar style. Defaults to the line preset (thin track,
        // heavy thumb) which matches the other dim ornamentation in
        // the picker chrome.
        ScrollbarStyle               scrollbar_style = ScrollbarStyle::line();
    };

    explicit Picker(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        const int vh = std::max(1, cfg_.viewport_h);

        // Auto-scroll: clamp state->y so the selected row sits inside
        // the viewport. Only fires when the selection is out of view,
        // so a user who manually scrolled past the cursor's row keeps
        // their position (until they move the cursor again).
        if (cfg_.scroll
            && cfg_.selected >= 0
            && cfg_.selected < static_cast<int>(cfg_.items.size())) {
            auto& s = *cfg_.scroll;
            const int total = static_cast<int>(cfg_.items.size());
            if (cfg_.selected < s.y)
                s.y = cfg_.selected;
            else if (cfg_.selected >= s.y + vh)
                s.y = cfg_.selected - vh + 1;
            const int max_y = std::max(0, total - vh);
            if (s.y > max_y) s.y = max_y;
            if (s.y < 0)     s.y = 0;
        }

        std::vector<Element> rows;
        rows.reserve(cfg_.header.size() + 1 + cfg_.footer.size());

        for (const auto& h_row : cfg_.header) rows.push_back(h_row);

        // Build the scrollable list. The full item vector is handed
        // to maya inside a vstack; `dsl::scroll(state, vh)` clips it
        // to vh rows and translates by -state.y at paint time. The
        // paired scrollbar reads bar_v_bounds via writeback so its
        // thumb tracks the same content↔viewport ratio.
        if (cfg_.scroll) {
            auto& s = *cfg_.scroll;
            auto viewport = vstack();
            auto items_copy = cfg_.items;   // build() is const; vstack consumes
            Element scrollable = std::move(viewport)(items_copy)
                                 | scroll(s, vh)
                                 | grow(1.0f);
            rows.push_back(
                h(std::move(scrollable),
                  scrollbar_y(s, vh, cfg_.scrollbar_style)
                ).build());
        } else {
            // No scroll state — paint items inline. Used when the
            // caller can guarantee items.size() ≤ vh structurally
            // (rare; mostly tests / placeholder messages).
            auto stack = vstack();
            auto items_copy = cfg_.items;
            rows.push_back(std::move(stack)(items_copy).build());
        }

        for (const auto& f_row : cfg_.footer) rows.push_back(f_row);

        return vstack()
            .padding(1, 2)
            .min_width(Dimension::fixed(cfg_.min_width))
            .border(BorderStyle::Round)
            .border_color(cfg_.accent)
            .border_text(cfg_.title, BorderTextPos::Top, BorderTextAlign::Center)
            (rows);
    }

private:
    Config cfg_;
};

} // namespace maya
