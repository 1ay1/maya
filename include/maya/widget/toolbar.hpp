#pragma once
// maya::widget::Toolbar — icon button row (background-free)
//
// A horizontal strip of icon buttons for an editor toolbar / view header:
// each button an icon (+ optional label), with toggled buttons accented,
// disabled ones dimmed, and thin │ separators between groups.
//
// Usage:
//   Toolbar t;
//   t.button("\uf0c7", "Save").toggle("\uf070", "Hide", true).sep()
//    .button("\uf021", "Refresh").disabled("\uf1f8", "Delete");
//   Element ui = t;

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct ToolbarTheme {
    Color icon     = Color::hex(0xBAC2DE);
    Color label    = Color::hex(0x9399B2);
    Color active   = Color::hex(0x89B4FA);
    Color disabled = Color::hex(0x494D64);
    Color sep       = Color::hex(0x313244);
};

struct Toolbar {
    struct Btn { std::string icon; std::string label; bool active; bool disabled; bool sep; };
    std::vector<Btn> btns;
    ToolbarTheme     theme;

    Toolbar& button(std::string icon, std::string label = {}) {
        btns.push_back({std::move(icon), std::move(label), false, false, false}); return *this;
    }
    Toolbar& toggle(std::string icon, std::string label, bool on) {
        btns.push_back({std::move(icon), std::move(label), on, false, false}); return *this;
    }
    Toolbar& disabled(std::string icon, std::string label = {}) {
        btns.push_back({std::move(icon), std::move(label), false, true, false}); return *this;
    }
    Toolbar& sep() { btns.push_back({{}, {}, false, false, true}); return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::string s; std::vector<StyledRun> r;
        auto put = [&](std::string_view t, Style st){ if(t.empty())return;
            r.push_back({s.size(), t.size(), st}); s += t; };
        put(" ", Style{});
        for (const auto& b : btns) {
            if (b.sep) { put(" \xe2\x94\x82 ", Style{}.with_fg(theme.sep)); continue; } // │
            Color c = b.disabled ? theme.disabled : b.active ? theme.active : theme.icon;
            Style st = Style{}.with_fg(c);
            if (b.active) st = st.with_bold();
            put(b.icon, st);
            if (!b.label.empty())
                put(" " + b.label, Style{}.with_fg(b.disabled ? theme.disabled
                                                   : b.active ? theme.active : theme.label));
            put("   ", Style{});
        }
        return Element{TextElement{ .content = std::move(s), .style = Style{},
                                    .wrap = TextWrap::NoWrap, .runs = std::move(r) }};
    }
};

} // namespace maya
