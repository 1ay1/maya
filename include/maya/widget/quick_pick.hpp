#pragma once
// maya::widget::QuickPick — two-pane fuzzy picker (background-free)
//
// The generic Ctrl-P/quick-open overlay: a prompt with the query, a list of
// fuzzy-matched items (label + dim detail) with the selection shaded, and an
// optional right-side preview pane. Rounded border.
//
// Usage:
//   QuickPick p; p.query = "rope";
//   p.item("rope.cpp", "src").item("rope.hpp", "include").select(0)
//    .preview({"struct Rope {", "  std::string text;", "};"});
//   Element ui = p | dsl::width(70);

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/builder.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"
#include "fuzzy_line.hpp"

namespace maya {

struct QuickPickTheme {
    Color border  = Color::hex(0x313244);
    Color prompt  = Color::hex(0x89B4FA);
    Color label   = Color::hex(0xBAC2DE);
    Color match   = Color::hex(0xF9E2AF);
    Color detail   = Color::hex(0x585B70);
    Color preview   = Color::hex(0x9399B2);
    Color shade      = Color::hex(0x313244);
};

struct QuickPick {
    struct Item { std::string label, detail; };
    std::string          query;
    std::vector<Item>    items;
    std::vector<std::string> preview_;
    int                  selected_ = 0;
    int                  max_rows  = 8;
    QuickPickTheme       theme;

    QuickPick& item(std::string label, std::string detail = {}) {
        items.push_back({std::move(label), std::move(detail)}); return *this;
    }
    QuickPick& select(int i) { selected_ = i; return *this; }
    QuickPick& preview(std::vector<std::string> p) { preview_ = std::move(p); return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        const int n = static_cast<int>(items.size());
        const int sel = n ? std::clamp(selected_, 0, n - 1) : 0;
        std::vector<Element> list;
        {   // prompt
            std::string s; std::vector<StyledRun> r;
            auto put=[&](std::string_view t, Style st){ if(t.empty())return;
                r.push_back({s.size(),t.size(),st}); s+=t; };
            put("\xef\x80\x82  ", Style{}.with_fg(theme.prompt)); // 
            put(query.empty() ? "Search files\xe2\x80\xa6" : query,
                query.empty() ? Style{}.with_fg(theme.detail).with_italic()
                              : Style{}.with_fg(theme.label));
            put("\xe2\x96\x8f", Style{}.with_fg(theme.prompt)); // ▏ caret
            list.push_back(Element{TextElement{ .content=std::move(s), .style=Style{},
                                                .wrap=TextWrap::NoWrap, .runs=std::move(r) }});
        }
        int shown = std::min(max_rows, n);
        int top = std::clamp(sel - shown/2, 0, std::max(0, n - shown));
        for (int i = top; i < top + shown; ++i) list.push_back(item_row(items[static_cast<size_t>(i)], i == sel));

        Element left = dsl::v(std::move(list)).build();
        Element content = left;
        if (!preview_.empty()) {
            std::vector<Element> pv;
            for (auto& l : preview_) pv.push_back(Element{TextElement{
                .content = l, .style = Style{}.with_fg(theme.preview), .wrap = TextWrap::NoWrap }});
            content = dsl::h(left | dsl::width(30),
                             Element{TextElement{.content="\xe2\x94\x82", .style=Style{}.with_fg(theme.border)}},
                             (dsl::v(std::move(pv)) | dsl::grow(1))).build();
        }
        return maya::detail::box().border(BorderStyle::Round).border_color(theme.border)
            .padding(0,1,0,1)(content);
    }

private:
    Element item_row(const Item& it, bool sel) const {
        const Color shade = sel ? theme.shade : Color{};
        auto tint = [sel, shade](Style st){ return sel ? st.with_bg(shade) : st; };
        std::string s; std::vector<StyledRun> r;
        auto put=[&](std::string_view t, Style st){ if(t.empty())return;
            r.push_back({s.size(),t.size(),tint(st)}); s+=t; };
        put(sel ? "\xe2\x96\xb8 " : "  ", Style{}.with_fg(theme.prompt)); // ▸
        auto hits = FuzzyLine::match(it.label, query);
        size_t mi=0;
        for (size_t i=0;i<it.label.size();++i){
            bool m = (mi<hits.positions.size() && (int)i==hits.positions[mi]); if(m)++mi;
            put(std::string_view{it.label}.substr(i,1),
                m ? Style{}.with_fg(theme.match).with_bold() : Style{}.with_fg(theme.label));
        }
        std::string left = std::move(s); std::vector<StyledRun> lr = std::move(r);
        std::string det = it.detail;
        return Element{ComponentElement{
            .render=[left=std::move(left), lr=std::move(lr), det=std::move(det),
                     sel, shade, dc=theme.detail](int w, int) -> Element {
                const Style fill = sel ? Style{}.with_bg(shade) : Style{};
                std::string s=left; std::vector<StyledRun> runs=lr;
                int gap = std::max(1, w - string_width(s) - string_width(det));
                runs.push_back({s.size(), (size_t)gap, fill}); s.append((size_t)gap,' ');
                if(!det.empty()){ Style ds=Style{}.with_fg(dc); if(sel)ds=ds.with_bg(shade);
                                  runs.push_back({s.size(), det.size(), ds}); s+=det; }
                int t=string_width(s); if(t<w){ runs.push_back({s.size(),(size_t)(w-t),fill});
                                                s.append((size_t)(w-t),' '); }
                return Element{TextElement{ .content=std::move(s), .style=fill,
                                            .wrap=TextWrap::NoWrap, .runs=std::move(runs) }};
            },
            .measure=[](int mw)->Size{ return Size{Columns(mw),Rows(1)}; },
        }};
    }
};

} // namespace maya
