#pragma once
// maya::widget::SplitView — split editor panes (background-free)
//
// Arranges child panes side by side (or stacked) with thin dividers between
// them and an optional per-pane title bar whose accent shows which pane holds
// focus — the split-editor layout of a real IDE. Panes size by weight.
//
// Usage:
//   SplitView sv{SplitView::Row};
//   sv.pane(codeA, "rope.cpp")
//     .pane(codeB, "rope.hpp")
//     .focus(0);
//   Element ui = sv | dsl::grow();

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct SplitViewTheme {
    Color divider       = Color::hex(0x313244);
    Color title         = Color::hex(0x6C7086); // idle pane title
    Color title_focused = Color::hex(0xE6EDF3); // focused pane title
    Color accent        = Color::hex(0x89B4FA); // focused pane underline
};

class SplitView {
public:
    enum Dir { Row, Col };

    explicit SplitView(Dir dir = Row) : dir_(dir) {}

    SplitView& pane(Element content, std::string title = {}, float weight = 1.0f) {
        panes_.push_back({std::move(content), std::move(title), weight});
        return *this;
    }
    SplitView& focus(int i) { focus_ = i; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> kids;
        for (size_t i = 0; i < panes_.size(); ++i) {
            if (i) kids.push_back(divider());
            kids.push_back(pane_element(panes_[i], static_cast<int>(i) == focus_));
        }
        auto container = (dir_ == Row) ? dsl::h(std::move(kids)).build()
                                       : dsl::v(std::move(kids)).build();
        return container;
    }

private:
    struct Pane { Element content; std::string title; float weight; };

    Dir              dir_;
    std::vector<Pane> panes_;
    int              focus_ = 0;
    SplitViewTheme   theme;

    Element pane_element(const Pane& p, bool focused) const {
        Element body = p.content;
        if (p.title.empty())
            return (dsl::v(body | dsl::grow()) | dsl::grow(p.weight)).build();

        // title bar: accent bar + name; focused gets a bright name + underline
        std::string s; std::vector<StyledRun> r;
        auto put = [&](std::string_view t, Style st) {
            if (t.empty()) return; r.push_back({s.size(), t.size(), st}); s += t;
        };
        put(focused ? "\xe2\x96\x8e " : "  ", Style{}.with_fg(theme.accent));
        Style ts = Style{}.with_fg(focused ? theme.title_focused : theme.title);
        if (focused) ts = ts.with_bold();
        put(p.title, ts);
        Element header{TextElement{ .content = std::move(s), .style = Style{},
                                    .wrap = TextWrap::NoWrap, .runs = std::move(r) }};

        return (dsl::v(header, body | dsl::grow()) | dsl::grow(p.weight)).build();
    }

    Element divider() const {
        const Color c = theme.divider;
        const bool vertical = (dir_ == Row);
        return Element{ComponentElement{
            .render = [c, vertical](int w, int h) -> Element {
                if (vertical) {
                    std::vector<Element> rows;
                    for (int i = 0; i < std::max(1, h); ++i)
                        rows.push_back(Element{TextElement{
                            .content = "\xe2\x94\x82", .style = Style{}.with_fg(c) }}); // │
                    return dsl::v(std::move(rows)).build();
                }
                std::string s;
                for (int i = 0; i < std::max(1, w); ++i) s += "\xe2\x94\x80"; // ─
                return Element{TextElement{ .content = std::move(s),
                                            .style = Style{}.with_fg(c) }};
            },
            .measure = [vertical](int mw) -> Size {
                return vertical ? Size{Columns(1), Rows(1)} : Size{Columns(mw), Rows(1)};
            },
        }};
    }
};

} // namespace maya
