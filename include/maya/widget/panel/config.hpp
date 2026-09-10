#pragma once
// maya::panel::Config — everything a host supplies to build one panel.
//
// Pure data; the widget (widget.hpp) turns it into an Element. Hosts fill a
// Config and never touch layout — the widget owns every chrome decision:
// border, viewport clipping, scrollbar glyphs, keep-selection-in-view.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../../element/element.hpp"
#include "../../style/color.hpp"
#include "../scrollbar.hpp"
#include "item.hpp"
#include "menu.hpp"
#include "theme.hpp"

namespace maya {
// Forward-declared rather than pulling in tab_strip.hpp: Config is pure data
// and this is the one field that names the strip's style. The enum has a
// fixed underlying type, so a declaration is enough to hold one by value.
enum class TabMark : std::uint8_t;
}  // namespace maya

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

    // How the active tab is marked. Defaults to TabMark::Underline (the
    // value 0), which is what a panel's tabs have always used; a host that
    // wants the denser editor treatment — " │ " dividers, no rule — sets
    // TabMark::Editor. Spelled as an optional so the default stays owned
    // by TabStrip rather than duplicated here.
    std::optional<TabMark>   tab_mark;

    // Paint the active tab as a FILLED CHIP in the panel's accent instead
    // of marking it with weight or a rule. Off by default.
    //
    // Worth its own flag rather than being folded into TabMark because it
    // is orthogonal to the mark: it answers "how loud" where the mark
    // answers "what shape". A one-tab strip is the case that needs it —
    // there is no dim neighbour to contrast against, so colour alone says
    // nothing and a lone underline reads as a stray rule.
    bool                     tab_fill = false;

    // Borrowed; must outlive the built Element. Null disables scrolling.
    ScrollState* scroll     = nullptr;
    int          viewport_h = 14;

    // The HOST clamps this to the terminal: a min-width wider than the
    // screen is not a minimum but an overflow, and the overlay centres the
    // panel so the excess is split off both edges and the labels vanish.
    int   min_width = 60;

    // ── What the frame costs, as ONE fact ────────────────────────────────
    //
    // Panel wraps its body in `.padding(1, 2).border(Round)`: one border
    // column and two pad columns on each side. Content laid out at the
    // panel's OUTER width is therefore six columns too wide, and a Canvas
    // clips rather than spills — so the excess does not paint past the
    // frame, it paints OVER the frame's own right border and stops. The
    // visible damage is a row that simply has no right edge.
    //
    // This was a number every caller had to know and none was told. The
    // one caller that noticed hand-counted it (agentty's stats panel:
    // `sheet.reserve_right(7)`, derived by measuring at three widths), and
    // a hand-counted constant describing someone else's internals goes
    // stale the moment the padding changes — silently, because nothing
    // references anything. A probe across all 14 agentty panels found 13
    // losing their border below 60 columns.
    //
    // So the widget states its own cost. content_width() below is the
    // width a caller may actually paint in, and it is derived from these
    // rather than repeated.
    static constexpr int kBorderCols  = 2;   // one each side
    static constexpr int kPaddingCols = 4;   // two each side
    // The vertical scrollbar rides in the right pad when the body
    // overflows. Charged UNCONDITIONALLY: it appears exactly when content
    // crosses the viewport, and a body whose width changes at that moment
    // is a layout that shifts under the reader for a reason they cannot
    // see. It is also the only way to break the circularity — whether the
    // bar exists depends on the viewport, which depends on the width the
    // bar's presence would determine.
    static constexpr int kScrollbarCols = 1;
    static constexpr int kChromeCols =
        kBorderCols + kPaddingCols + kScrollbarCols;

    // The width content may paint in, given the panel's OUTER width.
    // Never negative; a panel narrower than its own chrome has no body.
    [[nodiscard]] static constexpr int content_width(int outer) noexcept {
        const int inner = outer - kChromeCols;
        return inner > 0 ? inner : 0;
    }

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
