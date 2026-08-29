#pragma once
// maya::widget::TagInput — removable tag chips + input (background-free)
//
// A field of tag chips (each with a ✕ remove affordance) followed by a text
// input with a caret — the token/label editor used for filters, scopes, and
// keyword fields.
//
// Usage:
//   TagInput t; t.tag("bug").tag("ui").tag("good-first-issue").input("perf");
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

struct TagInputTheme {
    Color chip   = Color::hex(0xCDD6F4);
    Color edge   = Color::hex(0x45475A);
    Color remove = Color::hex(0x6C7086);
    Color input  = Color::hex(0xE6EDF3);
    Color prompt  = Color::hex(0x585B70);
    Color caret    = Color::hex(0x89B4FA);
};

struct TagInput {
    std::vector<std::string> tags;
    std::string              input_;
    std::string              placeholder = "add tag\xe2\x80\xa6";
    TagInputTheme            theme;

    TagInput& tag(std::string t) { tags.push_back(std::move(t)); return *this; }
    TagInput& input(std::string s) { input_ = std::move(s); return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::string s; std::vector<StyledRun> r;
        auto put=[&](std::string_view t, Style st){ if(t.empty())return;
            r.push_back({s.size(),t.size(),st}); s+=t; };
        for (const auto& t : tags) {
            put("\xe2\x9d\xb4", Style{}.with_fg(theme.edge));        // ❴
            put(t, Style{}.with_fg(theme.chip));
            put(" \xc3\x97", Style{}.with_fg(theme.remove));         // ×
            put("\xe2\x9d\xb5 ", Style{}.with_fg(theme.edge));       // ❵
        }
        if (input_.empty()) put(placeholder, Style{}.with_fg(theme.prompt).with_italic());
        else                put(input_, Style{}.with_fg(theme.input));
        put("\xe2\x96\x8f", Style{}.with_fg(theme.caret));           // ▏ caret
        return Element{TextElement{ .content=std::move(s), .style=Style{},
                                    .wrap=TextWrap::NoWrap, .runs=std::move(r) }};
    }
};

} // namespace maya
