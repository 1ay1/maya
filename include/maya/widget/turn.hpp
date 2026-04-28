#pragma once
// maya::widget::Turn — one conversation turn.
//
// Visual identity of a single speaker turn:
//
//   ┃ ❯ You                                        12:34  ·  turn 1
//   ┃
//   ┃ <body content from Config::body — markdown, agent panel, etc.>
//   ┃
//   ┃ ⚠  optional inline error
//
// Owned by the widget:
//   - Left-only Bold border in the speaker's color (the rail).
//   - Header row: glyph + bold label + spacer + dim meta line, right-pinned.
//   - One blank line between header and body.
//   - Variable number of body slots — supplied via Config::body.
//   - Optional error banner with leading ⚠ glyph.
//
//   maya::Turn{{
//       .glyph      = "\xe2\x9c\xa6",                      // ✦
//       .label      = "Opus 4.7",
//       .rail_color = Color::magenta(),
//       .meta       = "12:34  \xc2\xb7  4.2s  \xc2\xb7  turn 3",
//       .body       = { markdown_element, agent_timeline_element },
//       .error      = "stream cut off mid-tool",           // optional
//   }}.build();

#include <string>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/builder.hpp"
#include "../element/element.hpp"
#include "../style/border.hpp"
#include "../style/color.hpp"

namespace maya {

class Turn {
public:
    struct Config {
        std::string          glyph;
        std::string          label;
        Color                rail_color = Color::cyan();
        std::string          meta;
        std::vector<Element> body;
        std::string          error;            // empty = no error banner
    };

    explicit Turn(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;
        const Color muted = Color::bright_black();

        auto header = h(
            text(cfg_.glyph) | fgc(cfg_.rail_color),
            text(" "),
            text(cfg_.label) | fgc(cfg_.rail_color) | Bold,
            spacer(),
            text(cfg_.meta) | fgc(muted),
            text(" ")
        ) | grow(1.0f);

        auto error_row = h(
            text("\xe2\x9a\xa0  ") | fgc(Color::red()) | Bold,        // ⚠
            text(cfg_.error) | fgc(Color::red()) | Dim | Italic
        );

        auto inner = v(
            header,
            blank(),
            cfg_.body,
            when(!cfg_.error.empty(), v(blank(), error_row))
        ) | grow(1.0f);

        return maya::detail::box()
            .direction(FlexDirection::Row)
            .border(BorderStyle::Bold, cfg_.rail_color)
            .border_sides({.top = false, .right = false,
                           .bottom = false, .left = true})
            .padding(0, 0, 0, 2)
            .grow(1.0f)
          (inner.build());
    }

private:
    Config cfg_;
};

} // namespace maya
