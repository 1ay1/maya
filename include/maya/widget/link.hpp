#pragma once
// maya::widget::link — Styled terminal hyperlink
//
// Renders as underlined colored text with an optional icon.
// Terminals supporting OSC 8 make it clickable.
//
//   🔗 Click here
//   📄 src/main.cpp:42
//
// Usage:
//   Link lnk("documentation", "https://example.com/docs");
//   auto ui = lnk.build();

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../element/builder.hpp"
#include "../style/style.hpp"

namespace maya {

struct Link {
    std::string text;
    std::string url;
    Style link_style = Style{}.with_fg(Color::rgb(97, 175, 239)).with_underline();
    bool show_icon = false;

    Link() = default;
    Link(std::string text, std::string url)
        : text(std::move(text)), url(std::move(url)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::string content;
        std::vector<StyledRun> runs;

        if (show_icon) {
            std::string icon = "\xf0\x9f\x94\x97 ";  // 🔗 + space
            runs.push_back(StyledRun{0, icon.size(), Style{}});
            content += icon;
        }

        runs.push_back(StyledRun{content.size(), text.size(), link_style});
        content += text;

        if (!url.empty()) {
            // Show URL dimmed in parentheses
            std::string url_display = " (" + url + ")";
            runs.push_back(StyledRun{content.size(), url_display.size(), Style{}.with_dim()});
            content += url_display;
        }

        return Element{TextElement{
            .content = std::move(content),
            .style = {},
            .wrap = TextWrap::NoWrap,
            .runs = std::move(runs),
        }};
    }
};

} // namespace maya
