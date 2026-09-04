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
        //
        // Two ways to populate the list — use exactly one:
        //   • `rows`: typed structured rows. The widget draws the
        //     left-edge cursor/active bar, applies a full-width inverse-like
        //     highlight (light background + dark bold text) on the selected
        //     row, and lays out leading + trailing cells with a flex spacer.
        //     This is the path every chrome-consistent picker should take.
        //   • `items`: raw pre-built Elements. Escape hatch for
        //     callers that need full layout control (e.g. embedding
        //     a PlanView). No selection/active styling is applied.
        std::vector<Element>         items;

        // One row in the scrollable list. The widget owns every
        // chrome decision for the row:
        //   • col 0:    edge bar — `cursor_color` `▎` if `selected`,
        //                `active_color` `▎` if `active` (cursor wins
        //                on overlap), blank otherwise.
        //   • col 1:    single-space gutter.
        //   • leading:  caller-supplied primary text (e.g. label).
        //                Painted with `leading_style` (overridden to
        //                bold + `selected_fg` on the selected row),
        //                truncated with ellipsis on overflow, grows
        //                to absorb slack.
        //   • spacer:   absorbs any remaining width between leading
        //                and trailing.
        //   • trailing: caller-supplied secondary text (e.g.
        //                timestamp, description). Painted with
        //                `trailing_style` (overridden to `selected_fg`
        //                on the selected row). Right-aligned,
        //                truncated with ellipsis on overflow.
        //   • col -1:   single-space gutter so trailing content
        //                doesn't kiss the scrollbar.
        //
        // The row is built inside an `hstack().width(100%)` so the
        // spacer has actual leftover space to grow into (a plain
        // h(...) sizes to natural content width and the spacer
        // collapses to zero — see messenger.cpp build_header()).
        //
        // Pass text + Style (not pre-built Elements): the widget
        // needs to swap the foreground on the selected row, which
        // means it has to construct the text() node itself.
        struct Row {
            // Optional category/kind cell painted between the edge bar
            // and `leading`. Unlike leading/trailing, its style is NOT
            // overridden on the selected row — the badge is a colour-
            // coded identity (tool category, file kind, severity) and
            // must keep its hue while the cursor passes over it. Pad
            // badges to a common width caller-side for column alignment.
            std::string  badge;           // empty ⇒ no badge cell
            Style        badge_style    = {};
            std::string  leading;
            Style        leading_style  = {};
            // Optional match-highlight: byte offsets into `leading` to render
            // in the accent hue (bold) — the characters a fuzzy query matched.
            // Empty ⇒ leading painted as one span. The highlight colour keeps
            // its hue even on the selected row (so "which chars matched" stays
            // legible under the cursor).
            std::vector<int> highlight;
            Color        highlight_fg   = Color::bright_cyan();
            std::string  trailing;        // empty ⇒ no trailing cell
            Style        trailing_style = {};
            // When true, the TRAILING cell is secondary to the leading label
            // and yields space FIRST under width pressure (shrink 4× vs the
            // leading's 1×). Use for rows where the label is what the user
            // selects (command palette) so a long description never eats the
            // label on a narrow terminal. Default false keeps the file-picker
            // policy (leading gives first — the diffstat/locus matters more).
            bool         trailing_secondary = false;
            bool         selected = false;  // cursor is on this row
            bool         active   = false;  // "currently in use" marker
            // Non-selectable SECTION HEADER. Renders as a dim label with no
            // edge bar and no selection band; the widget never paints it as
            // selected. Callers that inject headers must keep the cursor off
            // them (headers carry no action) — point `selected`/`Row::selected`
            // at real rows only.
            bool         is_header = false;
        };

        // Structured rows. Mutually exclusive with `items`: if this
        // is non-empty, `items` is ignored. The widget builds each
        // row's Element on `build()` using the styling contract above.
        std::vector<Row>             rows;

        // 0-based index into the list (rows or items) of the
        // currently-selected entry. Used solely to keep the selection
        // inside the viewport via auto-scroll on paint. Negative ⇒
        // no selection (no clamp). Out-of-range values are ignored.
        // When `rows` is populated, the widget cross-checks this
        // against `Row::selected` — callers should keep them in
        // sync; `selected` here is what drives auto-scroll, while
        // `Row::selected` is what drives per-row styling.
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

        // Scrollbar style. Defaults to the neon preset (line track,
        // bright cyan thumb) so the scroll affordance pops on dark
        // terminal themes. Override in Config if a particular picker
        // wants to match a different accent color.
        ScrollbarStyle               scrollbar_style = ScrollbarStyle::neon();

        // Cursor (selected-row) bar colour. Cyan by default — matches
        // the neon scrollbar thumb and is universally read as "focus"
        // on dark themes.
        Color                        cursor_color = Color::bright_cyan();

        // Selected-row palette. A full-width light background with dark text
        // mirrors reverse-video selection while remaining deterministic across
        // terminal themes. Callers can override both for a themed picker.
        Color                        selected_bg  = Color::bright_white();
        Color                        selected_fg  = Color::black();

        // Active-row bar colour. Magenta by default so it sits on a
        // different colour axis from the cursor and reads as a
        // persistent "current" marker (vs. the cursor's transient
        // focus). Pickers without an active concept just leave every
        // Row::active false and this colour is never used.
        Color                        active_color = Color::bright_magenta();
    };

    explicit Picker(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        // viewport_h is the MAX rows the scrollable region paints.
        // When the item list is shorter, shrink the viewport to match
        // so a 9-item list doesn't leave 5 rows of empty space inside
        // the border. Floor at 1 row so a zero-item list (placeholder
        // "no matches" only) still has a paintable cell.
        // Materialise structured rows into Elements once, up front.
        // Everything below treats the list as `items`; `rows` is just
        // a typed sugar layer that lets the widget own the row chrome
        // (edge bar + bold styling + 100%-width hstack) instead of
        // every adapter reimplementing it.
        std::vector<Element> materialised;
        const std::vector<Element>* list = &cfg_.items;
        if (!cfg_.rows.empty()) {
            const int n    = static_cast<int>(cfg_.rows.size());
            const int cap0 = std::max(1, cfg_.viewport_h);
            materialised.reserve(cfg_.rows.size());
            // VIRTUALIZATION. Only rows in (or just around) the visible
            // window get a real, styled, multi-Element row; every other row
            // is a cheap 1-line blank placeholder. The vector still holds N
            // entries, so all downstream height / scroll / scrollbar math is
            // byte-identical — we've only swapped the OFF-SCREEN Elements for
            // trivial ones, turning an O(N) build+layout per frame into
            // O(viewport). Small lists take the exact full path (no risk).
            if (cfg_.scroll && n > 3 * cap0) {
                const int vh0 = std::min(cap0, n);
                // Mirror the keep-selection-in-view clamp so the window we
                // realize matches the s.y the auto-scroll clamp lands on.
                int y = std::clamp(cfg_.scroll->y, 0, std::max(0, n - vh0));
                if (cfg_.selected < y)               y = cfg_.selected;
                else if (cfg_.selected >= y + vh0)   y = cfg_.selected - vh0 + 1;
                y = std::clamp(y, 0, std::max(0, n - vh0));
                const int w0 = std::max(0, y - 1);          // 1 row of headroom
                const int w1 = std::min(n, y + vh0 + 1);
                for (int i = 0; i < n; ++i) {
                    if (i >= w0 && i < w1)
                        materialised.push_back(build_row(cfg_.rows[static_cast<std::size_t>(i)]));
                    else
                        materialised.push_back(text(std::string{" "}) | height(1));
                }
            } else {
                for (const auto& r : cfg_.rows)
                    materialised.push_back(build_row(r));
            }
            list = &materialised;
        }

        const int cap = std::max(1, cfg_.viewport_h);
        const int item_count = static_cast<int>(list->size());
        // Content height in ROWS. Structured rows are one line each by
        // contract, so count == rows. Raw `items` may be MULTI-ROW
        // Elements (a full diff body or a PlanView arrives as ONE
        // Element) — measure their real heights, or the viewport
        // collapses to items.size() rows (a one-Element body rendered
        // a 1-row viewport: the "output not visible" bug).
        int content_rows = item_count;
        if (cfg_.rows.empty()) {
            content_rows = 0;
            for (const auto& it : *list) {
                content_rows +=
                    measure_element(it, 1 << 14).height.value;
                // The sum only feeds min(cap, ·) and a <= vh check —
                // once past the cap the answers can't change, so stop
                // measuring (a 5000-line body would otherwise pay a
                // layout pass per line per frame).
                if (content_rows > cap) break;
            }
        }
        const int vh = std::min(cap, std::max(1, content_rows));

        // Auto-scroll: clamp state->y so the selected row sits inside
        // the viewport. Only fires when the selection is out of view,
        // so a user who manually scrolled past the cursor's row keeps
        // their position (until they move the cursor again).
        //
        // The scroll offset is in ROWS (paint translates by -y rows),
        // so the clamp must be computed in row space too. Structured
        // rows are 1 row each (index == row). Raw `items` may be
        // multi-row Elements — the old index-space clamp under-scrolled
        // there (item 3 of four 5-row items sits at row 15, not row 3),
        // leaving the selection stranded off-viewport. Measure the
        // prefix heights to find the selection's true row extent; the
        // list is picker-sized (dozens), so O(selected) measures per
        // frame is cheap, and the common structured-rows path skips
        // measuring entirely.
        if (cfg_.scroll
            && cfg_.selected >= 0
            && cfg_.selected < item_count) {
            auto& s = *cfg_.scroll;
            int sel_start = cfg_.selected;   // row where the item begins
            int sel_rows  = 1;               // its height in rows
            if (cfg_.rows.empty()) {
                sel_start = 0;
                for (int i = 0; i < cfg_.selected; ++i)
                    sel_start += measure_element(
                        (*list)[static_cast<std::size_t>(i)], 1 << 14)
                        .height.value;
                sel_rows = std::max(1, measure_element(
                    (*list)[static_cast<std::size_t>(cfg_.selected)], 1 << 14)
                    .height.value);
            }
            const int sel_end = sel_start + sel_rows;   // exclusive
            if (sel_start < s.y)
                s.y = sel_start;
            else if (sel_end > s.y + vh)
                // Align the item's BOTTOM to the viewport bottom (for a
                // taller-than-viewport item this shows its tail; the
                // first branch never fires simultaneously).
                s.y = sel_end - vh;
            if (s.y < 0) s.y = 0;
            // Upper clamp is owned by the renderer's writeback
            // (s->clamp() against the freshly measured max_y) — the
            // widget doesn't always know total content rows here (the
            // measurement loop above early-outs past the cap).
        } else if (cfg_.scroll && content_rows <= vh) {
            // List fits entirely inside the viewport — reset to top so
            // a previously-scrolled state (from a longer match set,
            // e.g. before the user typed a more specific query) doesn't
            // leave the list paint-shifted off the top edge.
            cfg_.scroll->y = 0;
        }

        std::vector<Element> rows;
        rows.reserve(cfg_.header.size() + 1 + cfg_.footer.size());

        // Header/footer rows are caller-supplied Elements (query lines,
        // separators, key-hint strips, position counters). They are
        // single-line by contract, but a narrow picker squeezes their
        // text — and the default TextWrap::Wrap would reflow an
        // overflowing hint row onto a SECOND line, growing the modal and
        // pushing its bottom border (or the row below) out of place. Lock
        // each to exactly one cell tall with overflow clipped so the
        // chrome stays put at every width; content that can't fit is
        // truncated at the right edge instead of wrapping.
        auto one_line = [](const Element& e) {
            return Element(e) | height(1) | overflow(Overflow::Hidden);
        };

        for (const auto& h_row : cfg_.header) rows.push_back(one_line(h_row));

        // Build the scrollable list. The full item vector is handed
        // to maya inside a vstack; `dsl::scroll(state, vh)` clips it
        // to vh rows and translates by -state.y at paint time. The
        // paired scrollbar reads bar_v_bounds via writeback so its
        // thumb tracks the same content↔viewport ratio.
        if (cfg_.scroll) {
            auto& s = *cfg_.scroll;
            auto viewport = vstack();
            auto items_copy = *list;        // build() is const; vstack consumes
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
            auto items_copy = *list;
            rows.push_back(std::move(stack)(items_copy).build());
        }

        for (const auto& f_row : cfg_.footer) rows.push_back(one_line(f_row));

        return vstack()
            .padding(1, 2)
            .min_width(Dimension::fixed(cfg_.min_width))
            .border(BorderStyle::Round)
            .border_color(cfg_.accent)
            .border_text(cfg_.title, BorderTextPos::Top, BorderTextAlign::Center)
            (rows);
    }

private:
    // U+258E LEFT ONE QUARTER BLOCK — thin vertical bar pinned at col 0.
    // Same convention every modern editor uses for "current" / "selected".
    static constexpr const char* kEdgeBar = "\xe2\x96\x8e";

    // Materialise one Row into an Element. Owns every row-chrome
    // decision: edge bar colour + glyph, full-width selected background,
    // selected-state styling override, and right-pinned trailing cell.
    [[nodiscard]] Element build_row(const Config::Row& r) const {
        using namespace dsl;

        // Section header: a dim, non-selectable label that groups the rows
        // beneath it. No edge bar, never highlighted (callers keep the cursor
        // off headers). Reads as a quiet divider — an uppercased section name
        // in its category hue with leading indent — set off from the rows.
        // An optional `trailing` (e.g. a count) is right-pinned and dimmed so
        // the header can carry a quiet stat without competing with the label.
        if (r.is_header) {
            Style hs = r.leading_style.with_dim(true).with_bold(true);
            std::vector<Element> cells;
            cells.push_back(text(std::string{"  "}));
            cells.push_back(text(r.leading, hs) | clip);
            cells.push_back(spacer());
            if (!r.trailing.empty()) {
                Style ts = r.trailing_style.with_dim(true);
                cells.push_back(text(r.trailing, ts));
                cells.push_back(text(std::string{"  "}));
            }
            return hstack().width(Dimension::percent(100))(std::move(cells))
                 | height(1) | overflow(Overflow::Hidden);
        }

        // Edge bar: cursor wins on overlap (cursor is transient and
        // the user just moved it here; active is what they're moving
        // AWAY from). Blank single space when neither, so the row
        // stays column-aligned with selected/active rows.
        auto edge = r.selected
            ? text(std::string{kEdgeBar},
                   Style{}.with_fg(cfg_.selected_fg).with_bold())
            : r.active
                ? text(std::string{kEdgeBar},
                       Style{}.with_fg(cfg_.active_color).with_bold())
                : text(std::string{" "});

        // On the selected row, use one high-contrast foreground across every
        // cell. The row box gets `selected_bg` below, including spacer/gutter
        // cells, so focus reads as the same full-width reverse-video band used
        // by compact account lists rather than as a subtle edge marker.
        Style ls = r.leading_style;
        Style ts = r.trailing_style;
        if (r.selected) {
            ls = Style{}.with_fg(cfg_.selected_fg).with_bold();
            ts = ts.with_fg(cfg_.selected_fg).with_dim(false);
        }

        // Badge follows the selected foreground too: preserving a category hue
        // against the light highlight can make it illegible. Off-selection it
        // keeps the caller's identity colour unchanged.
        auto badge_cell = [&]() -> Element {
            Style bs = r.badge_style;
            if (r.selected)
                bs = bs.with_fg(cfg_.selected_fg).with_dim(false).with_bold();
            return text(r.badge, bs);
        };

        // Leading cell — a single text span, unless the caller supplied
        // `highlight` offsets (fuzzy-match positions), in which case the
        // matched characters render in the accent hue (bold) and the rest in
        // the normal leading style. The highlight keeps its hue on the
        // selected row so "which chars matched" stays legible under the cursor.
        auto leading_cell = [&]() -> Element {
            if (r.highlight.empty()) return text(r.leading, ls) | clip;
            std::vector<bool> hot(r.leading.size(), false);
            for (int p : r.highlight)
                if (p >= 0 && p < static_cast<int>(r.leading.size())) hot[p] = true;
            Style hs = ls.with_fg(r.highlight_fg).with_bold();
            // Build one text span per run of same-highlight chars, then hand
            // them to hstack() in a SINGLE variadic call (the builder takes
            // its children at once; incremental calls don't accumulate).
            std::vector<Element> spans;
            std::size_t i = 0;
            while (i < r.leading.size()) {
                bool h = hot[i];
                std::size_t j = i;
                while (j < r.leading.size() && hot[j] == h) ++j;
                spans.push_back(text(r.leading.substr(i, j - i), h ? hs : ls));
                i = j;
            }
            return hstack()(spans);
        };

        auto finish = [&](Element row) -> Element {
            // Structured rows are a one-row contract. Enforce it here rather
            // than relying on every text child to win flex negotiation: at
            // phone-sized widths a long badge used to wrap onto a second row,
            // desynchronising selection indices and crowding adjacent rows.
            auto bounded = std::move(row)
                         | height(1)
                         | overflow(Overflow::Hidden);
            if (r.selected)
                return std::move(bounded) | Style{}.with_bg(cfg_.selected_bg);
            return bounded;
        };

        // No trailing cell when the caller passes an empty string.
        // Keeps row-builders simple for pickers without a secondary
        // column (e.g. PlanView-embedded rows).
        if (r.trailing.empty()) {
            if (r.badge.empty()) {
                return finish(hstack()
                    .width(Dimension::percent(100))(
                    edge,
                    text(std::string{" "}),
                    leading_cell() | grow(1.0f),
                    text(std::string{" "})
                ));
            }
            return finish(hstack()
                .width(Dimension::percent(100))(
                edge,
                text(std::string{" "}),
                badge_cell() | overflow(Overflow::Hidden) | shrink(0.5f),
                text(std::string{" "}),
                leading_cell() | grow(1.0f),
                text(std::string{" "})
            ));
        }

        // Both cells are flexible so the row degrades gracefully at ANY
        // width — no fixed columns that could crowd a narrow terminal.
        // Leading grows to absorb slack and is the FIRST to give it back
        // (shrink 3×): a long primary label truncates well before it can
        // push the trailing off the row. Trailing keeps its natural width
        // as long as it fits and only shrinks reluctantly (shrink 1×)
        // when the row gets genuinely tight, so the diffstat stays whole
        // on any reasonable terminal but never overflows on a skinny one.
        // Both | clip, so whichever loses space truncates with an
        // ellipsis instead of spilling. This is the overflow the raw
        // grow+clip / clip pair couldn't handle: two long cells (a
        // paragraph-length checkpoint preview meeting a fat "N files +A
        // −B" diffstat) now share negative space by shrink weight
        // instead of fighting over it.
        // Shrink policy: normally the leading gives space first (3×) so a fat
        // trailing diffstat/locus survives (file & checkpoint pickers). But
        // when `trailing_secondary` is set the LABEL is what the user selects
        // (command palette), so invert it — leading barely shrinks (1×) and the
        // trailing yields first (5×), and we insert a 2-col gap so the label and
        // description never touch even as the description truncates to nothing.
        const float lead_shrink  = r.trailing_secondary ? 1.0f : 3.0f;
        const float trail_shrink = r.trailing_secondary ? 5.0f : 1.0f;
        auto gap = r.trailing.empty() ? text(std::string{})
                                      : text(std::string{"  "});
        if (r.badge.empty()) {
            return finish(hstack()
                .width(Dimension::percent(100))(
                edge,
                text(std::string{" "}),
                leading_cell() | grow(1.0f) | shrink(lead_shrink),
                spacer(),
                gap,
                text(r.trailing, ts) | clip | shrink(trail_shrink),
                text(std::string{" "})
            ));
        }
        return finish(hstack()
            .width(Dimension::percent(100))(
            edge,
            text(std::string{" "}),
            badge_cell() | overflow(Overflow::Hidden) | shrink(0.5f),
            text(std::string{" "}),
            leading_cell() | grow(1.0f) | shrink(lead_shrink),
            spacer(),
            gap,
            text(r.trailing, ts) | clip | shrink(trail_shrink),
            text(std::string{" "})
        ));
    }

    Config cfg_;
};

} // namespace maya
