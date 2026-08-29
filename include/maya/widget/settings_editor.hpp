#pragma once
// maya::widget::SettingsEditor — searchable settings list (background-free)
//
// The Settings UI: a search box + a list of settings, each with a title, the
// setting id (dim), a description, and a right-aligned type-aware control —
// a toggle (●/○), a value, or a choice. Active row shaded.
//
// Usage:
//   SettingsEditor s; s.query = "font";
//   s.toggle("editor.wordWrap", "Word Wrap", "Wrap long lines", true)
//    .number("editor.fontSize", "Font Size", "Editor font size in px", "14")
//    .choice("editor.theme", "Color Theme", "", "Catppuccin Mocha")
//    .active(0);
//   Element ui = s | dsl::width(64);

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct SettingsEditorTheme {
    Color prompt = Color::hex(0x89B4FA);
    Color title  = Color::hex(0xCDD6F4);
    Color id     = Color::hex(0x585B70);
    Color desc   = Color::hex(0x7F849C);
    Color on      = Color::hex(0xA6E3A1);
    Color off      = Color::hex(0x6C7086);
    Color value     = Color::hex(0xF9E2AF);
    Color active     = Color::hex(0x232634);
};

class SettingsEditor {
public:
    enum class Kind : uint8_t { Toggle, Number, Text, Choice };
    std::string query;

    SettingsEditor& toggle(std::string id, std::string title, std::string desc, bool on) {
        rows_.push_back({Kind::Toggle, std::move(id), std::move(title), std::move(desc),
                         on ? "on" : "off", on}); return *this;
    }
    SettingsEditor& number(std::string id, std::string title, std::string desc, std::string v) {
        rows_.push_back({Kind::Number, std::move(id), std::move(title), std::move(desc), std::move(v), false}); return *this;
    }
    SettingsEditor& text(std::string id, std::string title, std::string desc, std::string v) {
        rows_.push_back({Kind::Text, std::move(id), std::move(title), std::move(desc), std::move(v), false}); return *this;
    }
    SettingsEditor& choice(std::string id, std::string title, std::string desc, std::string v) {
        rows_.push_back({Kind::Choice, std::move(id), std::move(title), std::move(desc), std::move(v), false}); return *this;
    }
    SettingsEditor& active(int i) { active_ = i; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> out;
        {   // search box
            std::string s; std::vector<StyledRun> r;
            auto put=[&](std::string_view t, Style st){ if(t.empty())return;
                r.push_back({s.size(),t.size(),st}); s+=t; };
            put("\xef\x80\x82  ", Style{}.with_fg(theme.prompt)); // 
            put(query.empty() ? "Search settings\xe2\x80\xa6" : query,
                query.empty() ? Style{}.with_fg(theme.desc).with_italic()
                              : Style{}.with_fg(theme.title));
            out.push_back(Element{TextElement{ .content=std::move(s), .style=Style{},
                                               .wrap=TextWrap::NoWrap, .runs=std::move(r) }});
            out.push_back(Element{TextElement{}});
        }
        for (size_t i = 0; i < rows_.size(); ++i)
            out.push_back(row(rows_[i], static_cast<int>(i) == active_));
        return dsl::v(std::move(out)).build();
    }

private:
    struct S { Kind kind; std::string id, title, desc, value; bool on; };
    std::vector<S> rows_;
    int            active_ = -1;
    SettingsEditorTheme theme;

    Element row(const S& it, bool on_row) const {
        const Color shade = on_row ? theme.active : Color{};
        auto tint = [on_row, shade](Style st){ return on_row ? st.with_bg(shade) : st; };
        // left: title  id
        std::string left; std::vector<StyledRun> lr;
        auto put=[&](std::string_view t, Style st){ if(t.empty())return;
            lr.push_back({left.size(),t.size(),tint(st)}); left+=t; };
        put(it.title, Style{}.with_fg(theme.title).with_bold());
        put("  " + it.id, Style{}.with_fg(theme.id));

        // right control
        std::string right; Color rcol;
        if (it.kind == Kind::Toggle) { right = it.on ? "\xe2\x97\x8f on" : "\xe2\x97\x8b off";
                                       rcol = it.on ? theme.on : theme.off; }
        else if (it.kind == Kind::Choice) { right = it.value + "  \xef\x84\x85"; rcol = theme.value; } // ▾
        else { right = "\xe2\x9d\xb4 " + it.value + " \xe2\x9d\xb5"; rcol = theme.value; }

        std::string desc = it.desc;
        return Element{ComponentElement{
            .render=[left=std::move(left), lr=std::move(lr), right=std::move(right), rcol,
                     desc=std::move(desc), on_row, shade, dc=theme.desc](int w, int)->Element{
                const Style fill = on_row ? Style{}.with_bg(shade) : Style{};
                std::string s=left; std::vector<StyledRun> runs=lr;
                int gap=std::max(1,w-string_width(s)-string_width(right));
                runs.push_back({s.size(),(size_t)gap,fill}); s.append((size_t)gap,' ');
                Style rs=Style{}.with_fg(rcol); if(on_row) rs=rs.with_bg(shade);
                runs.push_back({s.size(),right.size(),rs}); s+=right;
                int tot=string_width(s); if(tot<w){ runs.push_back({s.size(),(size_t)(w-tot),fill});
                                                    s.append((size_t)(w-tot),' '); }
                if(desc.empty()) return Element{TextElement{ .content=std::move(s), .style=fill,
                                                             .wrap=TextWrap::NoWrap, .runs=std::move(runs) }};
                Element top{TextElement{ .content=std::move(s), .style=fill,
                                         .wrap=TextWrap::NoWrap, .runs=std::move(runs) }};
                Element sub{TextElement{ .content="  " + desc,
                                         .style=Style{}.with_fg(dc), .wrap=TextWrap::NoWrap }};
                return dsl::v(top, sub).build();
            },
            .measure=[](int mw)->Size{ return Size{Columns(mw),Rows(1)}; },
        }};
    }
};

} // namespace maya
