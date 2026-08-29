#pragma once
// maya::widget::OptionPicker — single-select radio list (background-free)
//
// A vertical list of mutually-exclusive options, each a radio (◉/○) + label +
// optional dim description. The selected option is accented; the focused row is
// shaded.
//
// Usage:
//   OptionPicker p;
//   p.option("LF", "Line Feed (\\n)").option("CRLF", "Carriage Return + LF")
//    .select(0).focus(0);
//   Element ui = p | dsl::width(48);

#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct OptionPickerTheme {
    Color selected = Color::hex(0x89B4FA);
    Color label    = Color::hex(0xCDD6F4);
    Color desc     = Color::hex(0x7F849C);
    Color radio_off = Color::hex(0x585B70);
    Color active     = Color::hex(0x232634);
};

class OptionPicker {
public:
    OptionPicker& option(std::string label, std::string desc = {}) {
        opts_.push_back({std::move(label), std::move(desc)}); return *this;
    }
    OptionPicker& select(int i) { selected_ = i; return *this; }
    OptionPicker& focus(int i)  { focus_ = i; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> out;
        for (size_t i = 0; i < opts_.size(); ++i)
            out.push_back(row(opts_[i], static_cast<int>(i) == selected_,
                              static_cast<int>(i) == focus_));
        return dsl::v(std::move(out)).build();
    }

private:
    struct O { std::string label, desc; };
    std::vector<O> opts_;
    int            selected_ = 0;
    int            focus_ = -1;
    OptionPickerTheme theme;

    Element row(const O& o, bool sel, bool foc) const {
        const Color shade = foc ? theme.active : Color{};
        auto tint = [foc, shade](Style st){ return foc ? st.with_bg(shade) : st; };
        std::string left; std::vector<StyledRun> lr;
        auto put=[&](std::string_view t, Style st){ if(t.empty())return;
            lr.push_back({left.size(),t.size(),tint(st)}); left+=t; };
        put(sel ? "\xe2\x97\x89 " : "\xe2\x97\x8b ", // ◉ / ○
            Style{}.with_fg(sel ? theme.selected : theme.radio_off));
        Style ls = Style{}.with_fg(theme.label); if (sel) ls = ls.with_bold();
        put(o.label, ls);
        if (!o.desc.empty()) put("   " + o.desc, Style{}.with_fg(theme.desc));
        if (!foc)
            return Element{TextElement{ .content=std::move(left), .style=Style{},
                                        .wrap=TextWrap::NoWrap, .runs=std::move(lr) }};
        return Element{ComponentElement{
            .render=[left=std::move(left), lr=std::move(lr), shade](int w, int)->Element{
                std::string s=left; std::vector<StyledRun> runs=lr;
                int used=string_width(s);
                if(used<w){ runs.push_back({s.size(),(size_t)(w-used),Style{}.with_bg(shade)});
                            s.append((size_t)(w-used),' '); }
                return Element{TextElement{ .content=std::move(s), .style=Style{}.with_bg(shade),
                                            .wrap=TextWrap::NoWrap, .runs=std::move(runs) }};
            },
            .measure=[](int mw)->Size{ return Size{Columns(mw),Rows(1)}; },
        }};
    }
};

} // namespace maya
