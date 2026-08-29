#pragma once
// maya::widget::Sash — draggable pane resizer handle (background-free)
//
// The thin divider handle between panes with a centered grip; brightens when
// active (being dragged / hovered). Vertical (between columns) or horizontal
// (between rows). Fills the cross-axis.
//
// Usage:  Sash{}.vertical().active(dragging);   // 1-col tall handle

#include <algorithm>
#include <string>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct SashTheme {
    Color rail   = Color::hex(0x313244);
    Color grip   = Color::hex(0x585B70);
    Color active = Color::hex(0x89B4FA);
};

struct Sash {
    bool     vertical_ = true;
    bool     active_ = false;
    SashTheme theme;

    Sash& vertical()   { vertical_ = true; return *this; }
    Sash& horizontal() { vertical_ = false; return *this; }
    Sash& active(bool v = true) { active_ = v; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        const bool vert = vertical_; const bool act = active_;
        const Color rail = theme.rail, grip = theme.grip, accent = theme.active;
        return Element{ComponentElement{
            .render = [vert, act, rail, grip, accent](int w, int h) -> Element {
                const Color rc = act ? accent : rail;
                const Color gc = act ? accent : grip;
                if (vert) {
                    int H = std::max(1, h), mid = H / 2;
                    std::vector<Element> rows;
                    for (int i = 0; i < H; ++i) {
                        bool g = (i >= mid - 1 && i <= mid + 1);
                        rows.push_back(Element{TextElement{
                            .content = g ? "\xe2\x8a\x9e" : "\xe2\x94\x82", // ⊞ grip / │
                            .style = Style{}.with_fg(g ? gc : rc) }});
                    }
                    return dsl::v(std::move(rows)).build();
                }
                int W = std::max(1, w), mid = W / 2;
                std::string s; std::vector<StyledRun> r;
                for (int i = 0; i < W; ++i) {
                    bool g = (i >= mid - 1 && i <= mid + 1);
                    const char* ch = g ? "\xe2\x8a\x9e" : "\xe2\x94\x80"; // ⊞ / ─
                    r.push_back({s.size(), std::string(ch).size(), Style{}.with_fg(g ? gc : rc)});
                    s += ch;
                }
                return Element{TextElement{ .content = std::move(s), .style = Style{},
                                            .wrap = TextWrap::NoWrap, .runs = std::move(r) }};
            },
            .measure = [vert](int mw) -> Size {
                return vert ? Size{Columns(1), Rows(1)} : Size{Columns(mw), Rows(1)};
            },
        }};
    }
};

} // namespace maya
