#pragma once
// maya::widget::GridSplit — 2x2 editor grid (background-free)
//
// A four-pane editor grid (SplitView only does one axis). Four content
// Elements arranged as a 2x2 with dividers between; the focused quadrant's
// title bar is accented.
//
// Usage:
//   GridSplit g;
//   g.pane(0, a, "a.cpp").pane(1, b, "b.cpp").pane(2, c, "c.cpp").pane(3, d, "d.cpp")
//    .focus(0);
//   Element ui = g | dsl::grow();

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct GridSplitTheme {
    Color divider       = Color::hex(0x1E1E2E);
    Color title         = Color::hex(0x6C7086);
    Color title_focused = Color::hex(0xE6EDF3);
    Color accent        = Color::hex(0x89B4FA);
};

class GridSplit {
public:
    GridSplit& pane(int idx, Element content, std::string title = {}) {
        if (idx >= 0 && idx < 4) { panes_[idx] = std::move(content); titles_[idx] = std::move(title); }
        return *this;
    }
    GridSplit& focus(int i) { focus_ = i; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        auto quad = [&](int i) {
            std::string s; std::vector<StyledRun> r;
            const bool on = (i == focus_);
            auto put=[&](std::string_view t, Style st){ if(t.empty())return;
                r.push_back({s.size(),t.size(),st}); s+=t; };
            put(on ? "\xe2\x96\x8e " : "  ", Style{}.with_fg(theme.accent));
            Style ts = Style{}.with_fg(on ? theme.title_focused : theme.title);
            if (on) ts = ts.with_bold();
            put(titles_[i].empty() ? "\xe2\x80\x94" : titles_[i], ts);
            Element header{TextElement{ .content=std::move(s), .style=Style{},
                                        .wrap=TextWrap::NoWrap, .runs=std::move(r) }};
            return (dsl::v(header, panes_[i] | dsl::grow(1)) | dsl::grow(1)).build();
        };
        Element top = dsl::h(quad(0), vdivider(), quad(1)).build();
        Element bot = dsl::h(quad(2), vdivider(), quad(3)).build();
        return (dsl::v(top | dsl::grow(1), hdivider(), bot | dsl::grow(1)) | dsl::grow(1)).build();
    }

private:
    std::array<Element, 4>     panes_{ TextElement{}, TextElement{}, TextElement{}, TextElement{} };
    std::array<std::string, 4> titles_;
    int                        focus_ = 0;
    GridSplitTheme             theme;

    Element vdivider() const {
        return Element{ComponentElement{
            .render = [c = theme.divider](int, int h) -> Element {
                std::vector<Element> rows;
                for (int i = 0; i < std::max(1, h); ++i)
                    rows.push_back(Element{TextElement{ .content="\xe2\x94\x82",
                                                        .style=Style{}.with_fg(c) }});
                return dsl::v(std::move(rows)).build();
            },
            .measure = [](int) -> Size { return Size{Columns(1), Rows(1)}; },
        }};
    }
    Element hdivider() const {
        return Element{ComponentElement{
            .render = [c = theme.divider](int w, int) -> Element {
                std::string s; for (int i = 0; i < std::max(1, w); ++i) s += "\xe2\x94\x80";
                return Element{TextElement{ .content=std::move(s), .style=Style{}.with_fg(c) }};
            },
            .measure = [](int mw) -> Size { return Size{Columns(mw), Rows(1)}; },
        }};
    }
};

} // namespace maya
