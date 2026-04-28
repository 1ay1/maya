#pragma once
// maya::widget::StatusBanner — transient toast row with leading edge mark.
//
// Single-row banner used in the status bar's slot for ephemeral
// status / error messages:
//
//   ▎ status text                   (info — muted edge mark, italic)
//   ▎⚠  status text                  (error — red edge + glyph)
//
// When `text` is empty, renders a 1-cell blank placeholder so the
// surrounding panel's row count stays fixed (no vertical jitter when
// a toast appears or disappears).
//
//   maya::StatusBanner{{
//       .text     = m.s.status,
//       .is_error = m.s.status.starts_with("error:"),
//   }}.build();

#include <string>
#include <utility>

#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

class StatusBanner {
public:
    struct Config {
        std::string text;                 // empty = blank slot
        bool        is_error = false;
        Color       muted_color = Color::bright_black();
        Color       error_color = Color::red();
    };

    explicit StatusBanner(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;
        if (cfg_.text.empty()) return text(" ");      // blank-but-present

        const Color bc = cfg_.is_error ? cfg_.error_color : cfg_.muted_color;
        return h(
            text(" "),
            text("\xe2\x96\x8e", Style{}.with_fg(bc)),                       // ▎
            text(cfg_.is_error ? " \xe2\x9a\xa0  " : "  ",                   // ⚠
                 Style{}.with_fg(bc)),
            text(cfg_.text, Style{}.with_fg(bc).with_italic())
        ).build();
    }

private:
    Config cfg_;
};

} // namespace maya
