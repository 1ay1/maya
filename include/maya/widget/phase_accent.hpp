#pragma once
// maya::widget::PhaseAccent — soft-edge horizontal rule in a state color.
//
// A row of ▔ (top edge) or ▁ (bottom edge) half-block glyphs spanning
// the full width, dim, in a phase color. Reads as a "soft shelf"
// rather than a hard line — modern app vibe, and the color carries
// app-state info without extra chrome characters.
//
//   maya::PhaseAccent{{ .color=Color::cyan(), .position=Position::Top }}.build();

#include "../element/element.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

class PhaseAccent {
public:
    enum class Position : std::uint8_t { Top, Bottom };

    struct Config {
        Color    color    = Color::cyan();
        Position position = Position::Top;
    };

    explicit PhaseAccent(Config c) : cfg_(c) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        return Element{ComponentElement{
            .render = [c = cfg_.color, p = cfg_.position](int w, int /*h*/) -> Element {
                if (w <= 0) return Element{TextElement{}};
                const char* glyph = (p == Position::Top)
                    ? "\xe2\x96\x94"   // ▔  upper-half block
                    : "\xe2\x96\x81";  // ▁  lower-half block
                std::string line;
                line.reserve(static_cast<std::size_t>(w) * 3);
                for (int i = 0; i < w; ++i) line += glyph;
                return Element{TextElement{
                    .content = std::move(line),
                    .style   = Style{}.with_fg(c).with_dim(),
                }};
            },
            .layout = {},
        }};
    }

private:
    Config cfg_;
};

} // namespace maya
