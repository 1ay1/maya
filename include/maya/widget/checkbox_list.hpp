#pragma once
// maya::widget::CheckboxList — multi-select checkbox list (background-free)
//
// A vertical list of toggleable items with ☑/☐ checkboxes, a label, and an
// optional dim hint. The focused row is shaded; checked labels are accented.
//
// Usage:
//   CheckboxList c;
//   c.item("Format on save", true).item("Trim trailing whitespace", true, "editor")
//    .item("Insert final newline", false).focus(0);
//   Element ui = c | dsl::width(48);

#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct CheckboxListTheme {
    Color checked   = Color::hex(0xA6E3A1);
    Color box       = Color::hex(0x6C7086);
    Color label     = Color::hex(0xCDD6F4);
    Color label_off = Color::hex(0x9399B2);
    Color hint      = Color::hex(0x585B70);
    Color active     = Color::hex(0x232634);
};

class CheckboxList {
public:
    CheckboxList& item(std::string label, bool checked, std::string hint = {}) {
        rows_.push_back({std::move(label), checked, std::move(hint)}); return *this;
    }
    CheckboxList& focus(int i) { focus_ = i; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> out;
        for (size_t i = 0; i < rows_.size(); ++i)
            out.push_back(row(rows_[i], static_cast<int>(i) == focus_));
        return dsl::v(std::move(out)).build();
    }

private:
    struct C { std::string label; bool checked; std::string hint; };
    std::vector<C> rows_;
    int            focus_ = -1;
    CheckboxListTheme theme;

    Element row(const C& c, bool foc) const {
        const Color shade = foc ? theme.active : Color{};
        auto tint = [foc, shade](Style st){ return foc ? st.with_bg(shade) : st; };
        std::string left; std::vector<StyledRun> lr;
        auto put=[&](std::string_view t, Style st){ if(t.empty())return;
            lr.push_back({left.size(),t.size(),tint(st)}); left+=t; };
        put(c.checked ? "\xe2\x98\x91 " : "\xe2\x98\x90 ", // ☑ / ☐
            Style{}.with_fg(c.checked ? theme.checked : theme.box));
        Style ls = Style{}.with_fg(c.checked ? theme.label : theme.label_off);
        if (c.checked) ls = ls.with_bold();
        put(c.label, ls);
        if (!c.hint.empty()) put("   " + c.hint, Style{}.with_fg(theme.hint));
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
