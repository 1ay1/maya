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
        // Optional trailing detail (e.g. "3.4s", "42 tok/s"). Rendered
        // in dim-italic after the label, separated by a hair-space
        // bullet. Empty by default — hosts opt in.
        std::string detail;
    };

    explicit ActivityIndicator(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;
        const Color muted     = Color::bright_black();
        // Layout: maya::Conversation wraps the indicator in
        // `padding(0, 1)`, and every Turn draws a left rail at
        // column 0 of that padded box. The old design echoed that
        // rail with its own `▎` in the SAME column as the turn's
        // rail — two parallel bars stacked vertically read as a
        // visual stutter, especially when the turn's rail color
        // (role-tinted, e.g. pink for assistant) differed from
        // the indicator's edge color (phase-tinted, cyan while
        // streaming). We drop the rail entirely and indent to the
        // turn's BODY column instead, so the indicator reads as a
        // continuation of the assistant's turn rather than a
        // competing chrome element.
        //
        //   <turn rail> <2-col gap> <spinner> <label> <detail>
        //         ▌            ⠋ thinking · 3.4s
        //
        // The spinner takes the host accent (bright, bold) so the
        // animated glyph is the one bright thing on the row; the
        // label and detail recede into muted-italic so the eye
        // tracks motion, not text.
        Element row = text(cfg_.spinner_glyph) | fgc(cfg_.edge_color) | Bold;
        Element label_e =
            text(" " + cfg_.label + "\xe2\x80\xa6") | fgc(muted) | Italic;
        if (cfg_.detail.empty()) {
            return h(text("  "), std::move(row), std::move(label_e)).build();
        }
        // Middle dot · as the separator — fits the typographic
        // register of an ellipsis better than a hyphen and stays
        // in the muted layer so it doesn't compete with the
        // spinner for attention.
        Element detail_e =
            text(" \xc2\xb7 " + cfg_.detail) | fgc(muted) | Italic;
        return h(text("  "),
                 std::move(row),
                 std::move(label_e),
                 std::move(detail_e)).build();
    }

private:
    Config cfg_;
};

} // namespace maya
