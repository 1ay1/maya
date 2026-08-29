#pragma once
// maya::widget::KeyCaps — keyboard shortcut chips (background-free)
//
// Renders a key chord as little bracketed key caps joined by +, e.g. ⌃ ⇧ P.
//
// Usage:  KeyCaps{"Ctrl","Shift","P"};   or  KeyCaps::chord("Ctrl+K Ctrl+S");

#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct KeyCapsTheme {
    Color cap  = Color::hex(0xE6EDF3); // key label
    Color edge = Color::hex(0x45475A); // bracket
    Color plus = Color::hex(0x585B70); // + separator
};

struct KeyCaps {
    std::vector<std::string> keys;
    KeyCapsTheme             theme;

    KeyCaps() = default;
    KeyCaps(std::initializer_list<std::string> ks) : keys(ks) {}
    KeyCaps& key(std::string k) { keys.push_back(std::move(k)); return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::string s; std::vector<StyledRun> r;
        auto put = [&](std::string_view t, Style st){ if(t.empty())return;
            r.push_back({s.size(), t.size(), st}); s += t; };
        for (size_t i = 0; i < keys.size(); ++i) {
            if (i) put(" + ", Style{}.with_fg(theme.plus));
            put("\xe2\x9d\xb4", Style{}.with_fg(theme.edge));            // ❴ left
            put(keys[i], Style{}.with_fg(theme.cap).with_bold());
            put("\xe2\x9d\xb5", Style{}.with_fg(theme.edge));            // ❵ right
        }
        return Element{TextElement{ .content = std::move(s), .style = Style{},
                                    .wrap = TextWrap::NoWrap, .runs = std::move(r) }};
    }
};

} // namespace maya
