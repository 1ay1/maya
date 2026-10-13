#pragma once
// maya/device/theme_canvas.hpp — a theme fills the frame with its background;
// switching the running app's theme.

#include "internals.hpp"

namespace maya {
namespace detail {

// ============================================================================
// apply_theme_canvas — the one place a theme's background becomes pixels
// ============================================================================
//
// A theme is only half-applied if its foregrounds paint but its background
// does not: every hue in a scheme was contrast-checked against THAT canvas,
// so Dracula's inks over someone's white terminal is not "Dracula", it is a
// legibility bug wearing Dracula's name. So the runtime fills the frame with
// `theme.background` — centrally, here, rather than asking each host to
// remember to wrap its own root.
//
// The decision is DATA, not a name or a table index: `owns_canvas(t)` is
// true exactly when the theme states a real background. `theme::native`
// states `default_color()` — "whatever the user's terminal already is" — so
// nothing is painted and the terminal shows through untouched. That is not a
// fallback, it is the feature: a fill would destroy terminal transparency,
// blur and background images, which is the single most-reported complaint
// against TUIs that hardcode a background.
//
// ── Why this is safe in Mode::Inline ───────────────────────────────────
//
// Fullscreen owns the screen, so a background fill is trivially sound there.
// Inline does NOT: the frame is a window onto live scrollback, and painting
// a row the frame does not own means recolouring the user's history.
//
// Two properties make it sound, and both are asserted in test_style.cpp:
//
//   1. The fill reaches the right edge. A bg-painted blank is `visible` to
//      Canvas::set (style_id != 0), so it advances that row's last-content
//      column to the full width. Without this the diff's erase-to-EOL would
//      trim the trail and you would get a ragged tear with the terminal's
//      own background showing through the gaps — the classic themed-inline
//      artifact.
//   2. The fill stops at the content. The element wraps the root's measured
//      box, so rows below max_content_row are never touched and scrollback
//      is left exactly as the user's terminal drew it.
//
// ── Cost ──────────────────────────────────────────────────────────
//
// On native (the default) this is one predicate on a Color kind and the
// element is returned untouched — no allocation, no wrap, nothing added to
// the tree. Only a theme that actually owns a canvas pays for the wrapper,
// and then it is a single Box around an existing root, not a per-cell walk.
[[nodiscard]] inline Element apply_theme_canvas(Element root, const Theme& t,
                                               int term_width) {
    if (!theme::owns_canvas(t)) return root;
    // Built directly rather than via the `| bgc()` pipe: app.hpp sits below
    // dsl.hpp in the include order, and a runtime seam should not drag the
    // whole DSL in to set one field.
    BoxElement box;
    box.layout.direction = FlexDirection::Column;
    // The TERMINAL's width, in cells — not percent(100).
    //
    // A percentage resolves against the parent's content box, and this box
    // IS the root: it has no parent to take a percentage of, so it ends up
    // sized to its own child. agentty's layout is content-sized (81 columns
    // of chrome in a 100-column window), which left a 19-column unpainted
    // stripe down the right of every row — the hard edge where the fill
    // visibly stopped. Stating the real width is the only thing that makes
    // the fill reach the edge the user can see.
    if (term_width > 0) box.layout.width = Dimension::fixed(term_width);
    // NO fill on this box, and no height either.
    //
    // A box paints its whole RECT, and this wrapper's rect comes from its
    // child — whose min_height can exceed what the host actually drew. The
    // difference got painted in the canvas colour: themed rows BELOW the
    // status bar, an inline frame colouring terminal it does not own.
    //
    // The background reaches cells two other ways, both bounded by real
    // content: build_sgr renders the canvas colour for any style that names
    // no background, and render_tree fills each PAINTED row's tail after
    // paint (where max_content_row is known). This wrapper only states the
    // frame's width.
    box.children.push_back(std::move(root));
    return Element{std::move(box)};
}

} // namespace detail

// Swap the running app's palette.
//
// Works BEFORE run<>() has published its slot, and without a Runtime at all.
// It used to no-op in that window — "a host that sets a theme during startup
// is simply ignored rather than writing through a null" — which quietly threw
// the theme away: theme::live() stayed native, and so did every palette
// PROJECTED from it (markdown's flat colours above all). Anything that built
// an Element outside a frame — startup, a headless render, a unit test — got
// the wrong palette with no indication why, and "set the theme, then start
// the UI" is the obvious order to write.
//
// Routing through the Runtime's slot is about the NO-OP GUARD below, not
// lifetime: the guard needs the previous value to compare against, and the
// Runtime owns a Theme by value that serves as it. (theme::set_live() used
// to store the pointer, which made this a lifetime requirement too; the
// live slot interns now, so a caller may hand it a temporary. The slot is
// still the right place to keep the last-set value.) When there is no
// Runtime we own one here instead, seeded to native so the first comparison
// matches theme::live()'s actual initial state.
inline void app_set_theme(const Theme& t) {
    Theme* slot = detail::Device::live_theme();
    if (slot == nullptr) {
        // Pre-runtime storage for the same comparison. Seeded to native so
        // "set native before startup" is correctly a no-op rather than a
        // spurious swap.
        static Theme pre_runtime = theme::native;
        slot = &pre_runtime;
    }
    // NO-OP IF UNCHANGED. Hosts resolve their theme per frame (that is
    // how `auto` follows a tmux detach or an ssh hop), so this is called
    // on every single frame with the same value almost always. A swap
    // invalidates the render cache and re-derives projected palettes, so
    // doing that unconditionally would throw away every cached component
    // 60 times a second and turn the cache into a pure cost.
    //
    // The guard lives HERE rather than in each host because it is a
    // property of what a swap costs, which is maya's knowledge, not the
    // caller's.
    if (*slot == t) return;
    *slot = t;
    // Same notification as Runtime::set_theme — this is the path hosts
    // actually use (they have no Runtime&), so a projected palette that
    // only re-derived on set_theme would never update in practice.
    detail::Device::on_theme_changed(*slot);
}

/// Register a callback invoked whenever the live theme is replaced.
///
/// For subsystems that keep a palette PROJECTED from the theme rather than
/// reading it per-frame (markdown's flat colour globals being the main one).
/// Without this they stay on the palette they were built with, and picking a
/// scheme repaints the chrome while prose, code and tables keep the old
/// colours — the half-themed look that makes a picker feel broken.
///
/// Call once, before the UI loop. The callback runs on the thread that
/// swapped the theme.
inline void on_theme_changed(void (*fn)(const Theme&)) {
    detail::Device::theme_subscribers().push_back(fn);
}

} // namespace maya
