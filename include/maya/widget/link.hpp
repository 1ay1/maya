#pragma once
// maya::widget::link — Clickable terminal hyperlinks via OSC 8
//
// Usage:
//   auto elem = link("Click here", "https://example.com");

#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"
#include "../terminal/ansi.hpp"

#include <string>
#include <string_view>

namespace maya {

/// A hyperlink element using OSC 8.
/// Renders as underlined colored text; terminals that support OSC 8
/// make it clickable. Others just show the styled text.
struct Link {
    std::string text;
    std::string url;
    Style style = Style{}.with_fg(Color::rgb(100, 160, 255)).with_underline();

    [[nodiscard]] Element build() const {
        // For now, render as styled text.
        // Full OSC 8 integration requires canvas-level hyperlink tracking.
        // The visual appearance (underlined blue text) clearly indicates a link.
        return Element{TextElement{
            .content = text,
            .style = style,
        }};
    }
};

} // namespace maya
