#pragma once
// maya::widget::SnippetPreview — snippet with tabstops (background-free)
//
// Renders a snippet body with its tabstops ($1, $2, ${1:name}) highlighted:
// the active tabstop is a reverse-video pill, the others are accent-underlined
// placeholders. Literal text renders plain. This is the snippet-insertion
// decoration you see while cycling tabstops with Tab.
//
// Usage:
//   SnippetPreview s;
//   s.lit("for (int ").stop(1, "i").lit(" = 0; ").stop(1, "i").lit(" < ")
//    .stop(2, "n").lit("; ++").stop(1, "i").lit(") {").active(1);
//   Element ui = s;

#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct SnippetPreviewTheme {
    Color literal     = Color::hex(0xC9D1D9);
    Color placeholder = Color::hex(0x89B4FA);
    Color final_stop  = Color::hex(0xA6E3A1); // $0
};

struct SnippetPreview {
    struct Seg { std::string text; int stop; }; // stop < 0 => literal
    std::vector<Seg>    segs;
    int                 active_ = 1;
    SnippetPreviewTheme theme;

    SnippetPreview& lit(std::string t)  { segs.push_back({std::move(t), -1}); return *this; }
    SnippetPreview& stop(int n, std::string placeholder) { segs.push_back({std::move(placeholder), n}); return *this; }
    SnippetPreview& active(int n) { active_ = n; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::string s; std::vector<StyledRun> r;
        auto put=[&](std::string_view t, Style st){ if(t.empty())return;
            r.push_back({s.size(),t.size(),st}); s+=t; };
        for (const auto& seg : segs) {
            if (seg.stop < 0) { put(seg.text, Style{}.with_fg(theme.literal)); continue; }
            Color c = (seg.stop == 0) ? theme.final_stop : theme.placeholder;
            if (seg.stop == active_)
                put(seg.text.empty() ? " " : seg.text, Style{}.with_inverse()); // active pill
            else
                put(seg.text.empty() ? "\xe2\x96\xab" : seg.text, // ▫ empty stop
                    Style{}.with_fg(c).with_underline());
        }
        return Element{TextElement{ .content=std::move(s), .style=Style{},
                                    .wrap=TextWrap::NoWrap, .runs=std::move(r) }};
    }
};

} // namespace maya
