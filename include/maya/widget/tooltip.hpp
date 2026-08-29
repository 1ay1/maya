#pragma once
// maya::widget::Tooltip — small anchored hover bubble (background-free)
//
// A tiny rounded box with a message and an optional dim shortcut/hint on the
// right — the transient bubble shown on hover or focus.
//
// Usage:  Tooltip{"Go to Definition"}.shortcut("F12");

#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/builder.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct TooltipTheme {
    Color border   = Color::hex(0x45475A);
    Color text     = Color::hex(0xE6EDF3);
    Color shortcut = Color::hex(0x6C7086);
};

struct Tooltip {
    std::string    text_;
    std::string    shortcut_;
    TooltipTheme   theme;

    Tooltip() = default;
    explicit Tooltip(std::string t) : text_(std::move(t)) {}
    Tooltip& shortcut(std::string s) { shortcut_ = std::move(s); return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::string s; std::vector<StyledRun> r;
        auto put=[&](std::string_view t, Style st){ if(t.empty())return;
            r.push_back({s.size(),t.size(),st}); s+=t; };
        put(text_, Style{}.with_fg(theme.text));
        if (!shortcut_.empty()) put("   " + shortcut_, Style{}.with_fg(theme.shortcut));
        Element row{TextElement{ .content=std::move(s), .style=Style{},
                                 .wrap=TextWrap::NoWrap, .runs=std::move(r) }};
        return maya::detail::box().border(BorderStyle::Round).border_color(theme.border)
            .padding(0,1,0,1)(row);
    }
};

} // namespace maya
