#pragma once
// maya::floating — first-class, caret-anchored floating overlays.
//
// The flex tree lays out content in flow. Popups — autocomplete, hover cards,
// signature help, context menus, tooltips — must float at an ABSOLUTE cell
// (e.g. the text caret), on top of everything, without perturbing the layout
// beneath them, and they must stay on screen (clamp) and flip to the other
// side of the anchor when they'd overflow (a completion menu near the bottom
// opens upward).
//
// This builds on the renderer's z-stack primitive (`dsl::zstack`): the base is
// layer 0, each float is a self-positioning layer painted on top and clipped to
// the screen. Positioning/clamping/flipping happen at paint time inside a
// ComponentElement that knows the screen size, so a `Program::view` can simply:
//
//   return with_float(main_ui,
//       { .content = completion_menu, .cx = caret_x, .cy = caret_y,
//         .w = menu_w, .h = menu_h, .side = Side::Below });
//
// The float carries its own size (w,h) so clamp + flip are exact; pass 0 on an
// axis to skip clamping there (natural size, top/left aligned).

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/builder.hpp"
#include "../element/element.hpp"

namespace maya {

// A floating layer anchored to a screen cell.
struct Float {
    Element content;
    int     cx = 0, cy = 0;   // anchor cell (e.g. the caret position, in screen space)
    int     w  = 0, h  = 0;   // content size in cells; 0 = unknown (that axis isn't clamped)

    enum class Side : std::uint8_t { Below, Above, Right, Left, Over };
    Side    side  = Side::Below;
    int     gap   = 0;        // cells between the anchor and the float
    bool    flip  = true;     // flip to the opposite side if it would overflow
    bool    clamp = true;     // keep the float fully on screen
    int     z     = 0;        // paint order among floats (higher = on top)

    // A float must OCCLUDE the flow content beneath it (z-stack overlays are
    // painted without clearing). The backdrop fills the float's rect so the
    // editor text doesn't bleed through transparent cells. `Color::default_color`
    // occludes with the terminal's own background; override for a raised surface.
    bool    occlude = true;
    Color   bg      = Color::default_color();
};

namespace detail {

// Build an empty spacer cell of a fixed size (either axis 0 = auto).
[[nodiscard]] inline Element gap_cell(int w, int h) {
    auto b = maya::detail::box();
    if (w > 0) b = std::move(b).width(Dimension::fixed(w));
    if (h > 0) b = std::move(b).height(Dimension::fixed(h));
    return std::move(b)();
}

// Place `content` (natural or pinned to w×h) at absolute cell (x, y) within a
// full-screen layer: top padding of y rows, left padding of x cols.
[[nodiscard]] inline Element place_at(Element content, int x, int y, int cw, int ch) {
    using namespace maya::dsl;
    Element c = std::move(content);
    if (cw > 0) c = c | width(cw);
    if (ch > 0) c = c | height(ch);
    Element row = h(gap_cell(x, 0), c, spacer()).build();
    Element col = v(gap_cell(0, y), row, spacer()).build();
    return col;
}

// Resolve a float's top-left cell given the screen size, honouring side, gap,
// flip and clamp.
[[nodiscard]] inline std::pair<int, int> resolve(const Float& f, int sw, int sh) {
    int x = f.cx, y = f.cy;
    switch (f.side) {
        case Float::Side::Below: x = f.cx;              y = f.cy + 1 + f.gap; break;
        case Float::Side::Above: x = f.cx;              y = f.cy - f.h - f.gap; break;
        case Float::Side::Right: x = f.cx + 1 + f.gap;  y = f.cy; break;
        case Float::Side::Left:  x = f.cx - f.w - f.gap; y = f.cy; break;
        case Float::Side::Over:  x = f.cx;              y = f.cy; break;
    }
    if (f.flip && f.h > 0) {
        if (f.side == Float::Side::Below && y + f.h > sh) y = f.cy - f.h - f.gap; // open upward
        if (f.side == Float::Side::Above && y < 0)        y = f.cy + 1 + f.gap;   // fall downward
    }
    if (f.flip && f.w > 0) {
        if (f.side == Float::Side::Right && x + f.w > sw) x = f.cx - f.w - f.gap;
        if (f.side == Float::Side::Left  && x < 0)        x = f.cx + 1 + f.gap;
    }
    if (f.clamp) {
        if (f.w > 0) x = std::clamp(x, 0, std::max(0, sw - f.w)); else x = std::max(0, x);
        if (f.h > 0) y = std::clamp(y, 0, std::max(0, sh - f.h)); else y = std::max(0, y);
    }
    return {x, y};
}

} // namespace detail

// Compose `base` with floating layers painted on top (via z-stack). Floats are
// painted in ascending z (stable for equal z). Each float positions itself at
// paint time from the screen size it's given.
[[nodiscard]] inline Element with_floats(Element base, std::vector<Float> floats) {
    if (floats.empty()) return base;
    std::stable_sort(floats.begin(), floats.end(),
                     [](const Float& a, const Float& b) { return a.z < b.z; });

    std::vector<Element> layers;
    layers.reserve(floats.size() + 1);
    layers.push_back(std::move(base));
    for (auto& f : floats) {
        layers.push_back(Element{ComponentElement{
            .render = [f = std::move(f)](int sw, int sh) -> Element {
                auto [x, y] = detail::resolve(f, sw, sh);
                Element content = f.content;
                if (f.occlude && f.w > 0 && f.h > 0)
                    content = maya::detail::box().bg(f.bg)
                        .width(Dimension::fixed(f.w)).height(Dimension::fixed(f.h))(content);
                return detail::place_at(std::move(content), x, y, f.w, f.h);
            },
            .measure = [](int mw) -> Size { return {Columns(mw), Rows(1)}; },
        }});
    }
    return dsl::zstack(std::move(layers));
}

[[nodiscard]] inline Element with_float(Element base, Float f) {
    std::vector<Float> v; v.push_back(std::move(f));
    return with_floats(std::move(base), std::move(v));
}

} // namespace maya
