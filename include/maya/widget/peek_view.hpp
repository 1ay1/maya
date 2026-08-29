#pragma once
// maya::widget::PeekView — inline peek panel (background-free)
//
// The framed inline panel editors pop open for "Peek Definition / References":
// a titled rounded box (title on the top border, a right-aligned location on
// the same edge) wrapping any body Element — typically a CodeView snippet.
//
// Usage:
//   CodeView snip{code, {.lang = syntax::Lang::Cpp, .first_line = 40}};
//   PeekView p;
//   p.title(" 3 references ").location("rope.hpp:42").body(snip);
//   Element ui = p | dsl::width(70);

#include <string>
#include <utility>

#include "../dsl.hpp"
#include "../element/builder.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct PeekViewTheme {
    Color border   = Color::hex(0x89B4FA);
    Color title    = Color::hex(0xE6EDF3);
    Color location = Color::hex(0x89B4FA);
};

struct PeekView {
    std::string    title_;
    std::string    location_;
    Element        body_{TextElement{}};
    PeekViewTheme  theme;

    PeekView& title(std::string s)    { title_ = std::move(s); return *this; }
    PeekView& location(std::string s) { location_ = std::move(s); return *this; }
    PeekView& body(Element e)         { body_ = std::move(e); return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        auto box = maya::detail::box()
            .border(BorderStyle::Round)
            .border_color(theme.border)
            .padding(0, 1, 0, 1);
        if (!title_.empty())
            box = std::move(box).border_text(" \xef\x83\x85 " + title_ + " ", // 
                                             BorderTextPos::Top, BorderTextAlign::Start);
        if (!location_.empty())
            box = std::move(box).border_text_end(" " + location_ + " ",
                                                 BorderTextPos::Top, BorderTextAlign::End);
        return std::move(box)(body_);
    }
};

} // namespace maya
