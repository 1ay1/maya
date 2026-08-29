#pragma once
// maya::widget::BranchPicker — git branch list (background-free)
//
// A branch chooser: each branch with a current-marker, name, ahead/behind
// counts (↑n ↓n), and a dim last-commit hint. Remote branches get a cloud
// glyph. Active row shaded; the checked-out branch is accented.
//
// Usage:
//   BranchPicker b;
//   b.branch("main", true, 0, 0, "fix rope index")
//    .branch("feature/rope", false, 3, 1, "wip")
//    .remote("origin/main", "add concat")
//    .active(0);
//   Element ui = b | dsl::width(48);

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct BranchPickerTheme {
    Color current = Color::hex(0xA6E3A1);
    Color name    = Color::hex(0xBAC2DE);
    Color ahead   = Color::hex(0xA6E3A1);
    Color behind  = Color::hex(0xF38BA8);
    Color hint     = Color::hex(0x585B70);
    Color remote    = Color::hex(0x89B4FA);
    Color active     = Color::hex(0x232634);
};

class BranchPicker {
public:
    BranchPicker& branch(std::string name, bool current, int ahead, int behind,
                         std::string hint = {}) {
        rows_.push_back({std::move(name), current, false, ahead, behind, std::move(hint)});
        return *this;
    }
    BranchPicker& remote(std::string name, std::string hint = {}) {
        rows_.push_back({std::move(name), false, true, 0, 0, std::move(hint)});
        return *this;
    }
    BranchPicker& active(int i) { active_ = i; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> out;
        for (size_t i = 0; i < rows_.size(); ++i)
            out.push_back(row(rows_[i], static_cast<int>(i) == active_));
        return dsl::v(std::move(out)).build();
    }

private:
    struct B { std::string name; bool current, remote; int ahead, behind; std::string hint; };
    std::vector<B> rows_;
    int            active_ = -1;
    BranchPickerTheme theme;

    Element row(const B& b, bool on) const {
        const Color shade = on ? theme.active : Color{};
        auto tint = [on, shade](Style st){ return on ? st.with_bg(shade) : st; };
        std::string left; std::vector<StyledRun> lr;
        auto put = [&](std::string_view t, Style st){ if(t.empty())return;
            lr.push_back({left.size(), t.size(), tint(st)}); left += t; };

        put(b.current ? "\xef\x81\x92 " : "  ", tint(Style{}.with_fg(theme.current))); //  check
        put(b.remote ? "\xef\x83\x82 " : "\xef\x84\x98 ",  //  cloud /  branch
            tint(Style{}.with_fg(b.remote ? theme.remote : theme.name)));
        Style ns = Style{}.with_fg(b.current ? theme.current : theme.name);
        if (b.current) ns = ns.with_bold();
        put(b.name, ns);
        if (b.ahead)  put("  \xe2\x86\x91" + std::to_string(b.ahead), tint(Style{}.with_fg(theme.ahead)));
        if (b.behind) put(" \xe2\x86\x93" + std::to_string(b.behind), tint(Style{}.with_fg(theme.behind)));

        std::string hint = b.hint;
        return Element{ComponentElement{
            .render = [left = std::move(left), lr = std::move(lr), hint = std::move(hint),
                       on, shade, hc = theme.hint](int w, int) -> Element {
                const Style fill = on ? Style{}.with_bg(shade) : Style{};
                std::string s = left; std::vector<StyledRun> runs = lr;
                int rw = string_width(hint);
                int gap = std::max(1, w - string_width(s) - rw);
                runs.push_back({s.size(), static_cast<size_t>(gap), fill});
                s.append(static_cast<size_t>(gap), ' ');
                if (!hint.empty()) { Style hs = Style{}.with_fg(hc); if (on) hs = hs.with_bg(shade);
                                     runs.push_back({s.size(), hint.size(), hs}); s += hint; }
                int total = string_width(s);
                if (total < w) { runs.push_back({s.size(), static_cast<size_t>(w-total), fill});
                                 s.append(static_cast<size_t>(w-total), ' '); }
                return Element{TextElement{ .content = std::move(s), .style = fill,
                                            .wrap = TextWrap::NoWrap, .runs = std::move(runs) }};
            },
            .measure = [](int mw) -> Size { return Size{Columns(mw), Rows(1)}; },
        }};
    }
};

} // namespace maya
