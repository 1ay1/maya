#pragma once
// maya::widget::ExtensionsList — extension marketplace cards (background-free)
//
// The Extensions sidebar: a list of extension cards, each with an icon, name,
// version, publisher, a one-line description, a star rating / install count,
// and an install/installed/enabled affordance. Active card shaded.
//
// Usage:
//   ExtensionsList e;
//   e.ext("clangd", "12.0", "LLVM", "C/C++ language server", 4.8f, true, true)
//    .ext("Catppuccin", "1.3", "catppuccin", "Soothing pastel theme", 5.0f, true, false)
//    .ext("Vim", "1.29", "vscodevim", "Vim emulation", 4.3f, false, false)
//    .active(0);
//   Element ui = e | dsl::width(50);

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct ExtensionsListTheme {
    Color name    = Color::hex(0xCDD6F4);
    Color version = Color::hex(0x585B70);
    Color author  = Color::hex(0x7F849C);
    Color desc    = Color::hex(0x9399B2);
    Color star     = Color::hex(0xF9E2AF);
    Color installed = Color::hex(0xA6E3A1);
    Color disabled  = Color::hex(0x6C7086);
    Color install    = Color::hex(0x89B4FA);
    Color active      = Color::hex(0x232634);
    Color icon         = Color::hex(0x89B4FA);
};

class ExtensionsList {
public:
    ExtensionsList& ext(std::string name, std::string ver, std::string author,
                        std::string desc, float stars, bool installed, bool enabled) {
        rows_.push_back({std::move(name), std::move(ver), std::move(author),
                         std::move(desc), stars, installed, enabled}); return *this;
    }
    ExtensionsList& active(int i) { active_ = i; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> out;
        for (size_t i = 0; i < rows_.size(); ++i)
            out.push_back(card(rows_[i], static_cast<int>(i) == active_));
        return dsl::v(std::move(out)).build();
    }

private:
    struct E { std::string name, ver, author, desc; float stars; bool installed, enabled; };
    std::vector<E> rows_;
    int            active_ = -1;
    ExtensionsListTheme theme;

    Element card(const E& e, bool on) const {
        const Color shade = on ? theme.active : Color{};
        auto tint = [on, shade](Style st){ return on ? st.with_bg(shade) : st; };

        // line 1: icon name ver ... status
        std::string l1; std::vector<StyledRun> r1;
        auto p1=[&](std::string_view t, Style st){ if(t.empty())return;
            r1.push_back({l1.size(),t.size(),tint(st)}); l1+=t; };
        p1("\xef\x84\xa6 ", Style{}.with_fg(theme.icon));        //  puzzle
        p1(e.name, Style{}.with_fg(theme.name).with_bold());
        p1(" v" + e.ver, Style{}.with_fg(theme.version));
        std::string status = !e.installed ? "Install"
                            : e.enabled    ? "\xef\x81\x92 Enabled" : "Disabled"; //  check
        Color scol = !e.installed ? theme.install : e.enabled ? theme.installed : theme.disabled;

        // line 2: description  ·  ★ rating  publisher
        char rat[8]; std::snprintf(rat, sizeof rat, "%.1f", e.stars);
        std::string l2; std::vector<StyledRun> r2;
        auto p2=[&](std::string_view t, Style st){ if(t.empty())return;
            r2.push_back({l2.size(),t.size(),tint(st)}); l2+=t; };
        p2("  " + e.desc, Style{}.with_fg(theme.desc));
        p2("  \xe2\x98\x85" , Style{}.with_fg(theme.star)); // ★
        p2(rat, Style{}.with_fg(theme.star));
        p2("  " + e.author, Style{}.with_fg(theme.author));

        return Element{ComponentElement{
            .render=[l1=std::move(l1), r1=std::move(r1), l2=std::move(l2), r2=std::move(r2),
                     status=std::move(status), scol, on, shade](int w, int)->Element{
                const Style fill = on ? Style{}.with_bg(shade) : Style{};
                // line 1 with right-aligned status
                std::string s1=l1; std::vector<StyledRun> runs1=r1;
                int gap=std::max(1,w-string_width(s1)-string_width(status));
                runs1.push_back({s1.size(),(size_t)gap,fill}); s1.append((size_t)gap,' ');
                Style ss=Style{}.with_fg(scol).with_bold(); if(on) ss=ss.with_bg(shade);
                runs1.push_back({s1.size(),status.size(),ss}); s1+=status;
                int t1=string_width(s1); if(t1<w){ runs1.push_back({s1.size(),(size_t)(w-t1),fill});
                                                   s1.append((size_t)(w-t1),' '); }
                // line 2 padded
                std::string s2=l2; std::vector<StyledRun> runs2=r2;
                int t2=string_width(s2); if(t2<w){ runs2.push_back({s2.size(),(size_t)(w-t2),fill});
                                                   s2.append((size_t)(w-t2),' '); }
                Element e1{TextElement{ .content=std::move(s1), .style=fill,
                                        .wrap=TextWrap::NoWrap, .runs=std::move(runs1) }};
                Element e2{TextElement{ .content=std::move(s2), .style=fill,
                                        .wrap=TextWrap::NoWrap, .runs=std::move(runs2) }};
                return dsl::v(e1, e2).build();
            },
            .measure=[](int mw)->Size{ return Size{Columns(mw),Rows(1)}; },
        }};
    }
};

} // namespace maya
