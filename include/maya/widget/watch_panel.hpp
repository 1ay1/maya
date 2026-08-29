#pragma once
// maya::widget::WatchPanel — debugger watch expressions (background-free)
//
// The Watch panel: user-added expressions, each showing the expression, its
// evaluated value (kind-coloured), or an error, plus a dim "＋ Add Expression"
// affordance at the bottom. Active row shaded.
//
// Usage:
//   WatchPanel w;
//   w.watch("rope->weight", "8", VarKind::Number)
//    .watch("rope->text", "\"hello\"", VarKind::String)
//    .error("rope->parent", "no member named 'parent'")
//    .active(0);
//   Element ui = w | dsl::width(46);

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"
#include "variables_tree.hpp"   // VarKind

namespace maya {

struct WatchPanelTheme {
    Color expr   = Color::hex(0xBAC2DE);
    Color eq     = Color::hex(0x585B70);
    Color number = Color::hex(0xFAB387);
    Color string = Color::hex(0xA6E3A1);
    Color boolean = Color::hex(0xCBA6F7);
    Color null    = Color::hex(0x6C7086);
    Color pointer = Color::hex(0x89DCEB);
    Color error    = Color::hex(0xF38BA8);
    Color add       = Color::hex(0x585B70);
    Color active     = Color::hex(0x232634);
};

class WatchPanel {
public:
    WatchPanel& watch(std::string expr, std::string value, VarKind kind) {
        rows_.push_back({std::move(expr), std::move(value), kind, false}); return *this;
    }
    WatchPanel& error(std::string expr, std::string msg) {
        rows_.push_back({std::move(expr), std::move(msg), VarKind::Other, true}); return *this;
    }
    WatchPanel& active(int i) { active_ = i; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> out;
        for (size_t i = 0; i < rows_.size(); ++i)
            out.push_back(row(rows_[i], static_cast<int>(i) == active_));
        out.push_back(Element{TextElement{ .content = "\xef\x81\xa7 Add Expression", //  plus
                                           .style = Style{}.with_fg(theme.add).with_italic(),
                                           .wrap = TextWrap::NoWrap }});
        return dsl::v(std::move(out)).build();
    }

private:
    struct W { std::string expr, value; VarKind kind; bool err; };
    std::vector<W> rows_;
    int            active_ = -1;
    WatchPanelTheme theme;

    Color vc(VarKind k) const {
        switch (k) { case VarKind::Number: return theme.number; case VarKind::String: return theme.string;
                     case VarKind::Bool: return theme.boolean; case VarKind::Null: return theme.null;
                     case VarKind::Pointer: return theme.pointer; default: return theme.expr; }
    }

    Element row(const W& w, bool on) const {
        const Color shade = on ? theme.active : Color{};
        auto tint = [on, shade](Style st){ return on ? st.with_bg(shade) : st; };
        std::string left; std::vector<StyledRun> lr;
        auto put=[&](std::string_view t, Style st){ if(t.empty())return;
            lr.push_back({left.size(),t.size(),tint(st)}); left+=t; };
        put(w.expr, Style{}.with_fg(theme.expr));
        put(w.err ? ": " : " = ", Style{}.with_fg(theme.eq));
        put(w.value, Style{}.with_fg(w.err ? theme.error : vc(w.kind)));
        if (!on)
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
