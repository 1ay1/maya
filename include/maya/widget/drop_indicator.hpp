#pragma once
// maya::widget::DropIndicator — drag-and-drop insertion line (background-free)
//
// The accent line shown while dragging a tab/file to indicate where it will
// drop: a full-width rule with a centered label and end arrows.
//
// Usage:  DropIndicator{}.label("Move to new group");

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct DropIndicator {
    std::string label_;
    Color       accent = Color::hex(0x89B4FA);

    DropIndicator& label(std::string s) { label_ = std::move(s); return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::string lbl = label_;
        Color c = accent;
        return Element{ComponentElement{
            .render = [lbl = std::move(lbl), c](int w, int) -> Element {
                std::string mid = lbl.empty() ? "" : " \xe2\x96\xb8 " + lbl + " \xe2\x97\x82 "; // ▸ … ◂
                int mw = string_width(mid);
                int rest = std::max(0, w - mw);
                int left = rest / 2, right = rest - left;
                std::string s; std::vector<StyledRun> r;
                std::string l; for (int i = 0; i < left; ++i) l += "\xe2\x94\x81"; // ━
                std::string rr; for (int i = 0; i < right; ++i) rr += "\xe2\x94\x81";
                r.push_back({0, l.size(), Style{}.with_fg(c)}); s = l;
                if (!mid.empty()) { r.push_back({s.size(), mid.size(), Style{}.with_fg(c).with_bold()}); s += mid; }
                r.push_back({s.size(), rr.size(), Style{}.with_fg(c)}); s += rr;
                return Element{TextElement{ .content = std::move(s), .style = Style{},
                                            .wrap = TextWrap::NoWrap, .runs = std::move(r) }};
            },
            .measure = [](int mw) -> Size { return Size{Columns(mw), Rows(1)}; },
        }};
    }
};

} // namespace maya
