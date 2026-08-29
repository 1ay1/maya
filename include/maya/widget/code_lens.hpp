#pragma once
// maya::widget::CodeLens — actionable lens line (background-free)
//
// The dim, clickable line editors float above a symbol: "3 references ·
// 2 implementations · Run | Debug". Reference/info actions render muted;
// primary actions (Run, Debug) get an accent.
//
// Usage:
//   CodeLens l; l.info("3 references").info("2 implementations")
//               .action("Run").action("Debug");
//   Element ui = l;

#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct CodeLensTheme {
    Color info   = Color::hex(0x6C7086);
    Color action = Color::hex(0x89B4FA);
    Color sep    = Color::hex(0x45475A);
    Color indent  = Color::hex(0x45475A);
};

struct CodeLens {
    struct Item { std::string label; bool primary; };
    std::vector<Item> items;
    int               indent = 0;
    CodeLensTheme     theme;

    CodeLens& info(std::string s)   { items.push_back({std::move(s), false}); return *this; }
    CodeLens& action(std::string s) { items.push_back({std::move(s), true}); return *this; }
    CodeLens& at(int col) { indent = col; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::string s; std::vector<StyledRun> r;
        auto put=[&](std::string_view t, Style st){ if(t.empty())return;
            r.push_back({s.size(),t.size(),st}); s+=t; };
        if (indent > 0) put(std::string(static_cast<size_t>(indent), ' '), Style{});
        bool first = true; bool prev_primary = false;
        for (const auto& it : items) {
            if (!first) put(it.primary && prev_primary ? "  \xe2\x94\x82  " : "  \xc2\xb7  ", // │ / ·
                            Style{}.with_fg(theme.sep));
            put(it.label, it.primary ? Style{}.with_fg(theme.action).with_bold()
                                     : Style{}.with_fg(theme.info));
            first = false; prev_primary = it.primary;
        }
        return Element{TextElement{ .content=std::move(s), .style=Style{},
                                    .wrap=TextWrap::NoWrap, .runs=std::move(r) }};
    }
};

} // namespace maya
