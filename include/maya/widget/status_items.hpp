#pragma once
// maya::widget::StatusItems — status-bar readout segments (background-free)
//
// A horizontal run of small status readouts (icon + text), each in its own
// colour — the individual clickable items of a status bar (line-ending,
// encoding, indentation, language, errors/warnings, feedback…). Use it to
// build a custom status bar or a compact info strip.
//
// Usage:
//   StatusItems s;
//   s.item("\uf188", "0", Color::hex(0xF38BA8)).item("\uf071", "2", Color::hex(0xE2B341))
//    .item("", "UTF-8").item("", "LF").item("", "Spaces: 4").item("\uf1c9", "C++");
//   Element ui = s;

#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct StatusItems {
    struct Item { std::string icon, text; Color color; };
    std::vector<Item> items;
    Color             def = Color::hex(0x9399B2);

    StatusItems& item(std::string icon, std::string text) {
        items.push_back({std::move(icon), std::move(text), def}); return *this;
    }
    StatusItems& item(std::string icon, std::string text, Color c) {
        items.push_back({std::move(icon), std::move(text), c}); return *this;
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::string s; std::vector<StyledRun> r;
        auto put=[&](std::string_view t, Style st){ if(t.empty())return;
            r.push_back({s.size(),t.size(),st}); s+=t; };
        for (size_t i = 0; i < items.size(); ++i) {
            if (i) put("   ", Style{});
            if (!items[i].icon.empty()) put(items[i].icon + " ", Style{}.with_fg(items[i].color));
            put(items[i].text, Style{}.with_fg(items[i].color));
        }
        return Element{TextElement{ .content=std::move(s), .style=Style{},
                                    .wrap=TextWrap::NoWrap, .runs=std::move(r) }};
    }
};

} // namespace maya
