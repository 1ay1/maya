#pragma once
// maya::widget::ActivityIndicator — single-row "still doing something" badge.
//
// Compact one-liner: coloured edge bar + spinner glyph + italic label
// with trailing ellipsis. Floats at the bottom of a chat thread or a
// sidebar when the host is mid-stream and hasn't yet rendered visible
// per-step output.
//
//   ▎ ⠋ streaming…
//
// The host advances spinner frames at whatever cadence its tick
// subscription uses and passes the current glyph in.
//
//   maya::ActivityIndicator{{
//       .edge_color    = phase_color,
//       .spinner_glyph = std::string{spinner.current_frame()},
//       .label         = "streaming",
//   }}.build();

#include <string>
#include <utility>

#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../style/color.hpp"

namespace maya {

class ActivityIndicator {
public:
    struct Config {
        Color       edge_color = Color::cyan();
        std::string spinner_glyph;
        std::string label;
    };

    explicit ActivityIndicator(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;
        const Color muted = Color::bright_black();
        return (h(
            text("\xe2\x96\x8e") | fgc(cfg_.edge_color),                   // ▎
            text(" "),
            text(cfg_.spinner_glyph) | fgc(cfg_.edge_color) | Bold,
            text(" "),
            text(cfg_.label + "\xe2\x80\xa6") | fgc(muted) | Italic         // …
        ) | padding(0, 0, 0, 1)).build();
    }

private:
    Config cfg_;
};

} // namespace maya
