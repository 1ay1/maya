#pragma once
// maya::widget::Overlay — base layer + optional centered floating element.
//
// Z-stack composition for modal-style UIs: the `base` always renders;
// when `overlay` is present (set `present=true`) it floats horizontally
// centered, vertically pinned to the bottom edge, with a solid default
// background so the base content underneath doesn't bleed through.
// Pass `present=false` and the widget collapses to just the base —
// callers don't need an `if` branch.
//
//   maya::Overlay{{
//       .base    = main_view_element,
//       .overlay = modal_element,
//       .present = m.ui.has_modal_open,
//   }}.build();

#include <utility>

#include "../element/builder.hpp"
#include "../element/element.hpp"
#include "../style/color.hpp"

namespace maya {

class Overlay {
public:
    struct Config {
        Element base;       // default-empty
        Element overlay;    // default-empty
        bool    present = false;
    };

    explicit Overlay(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        if (!cfg_.present) return cfg_.base;
        return detail::zstack({
            cfg_.base,
            detail::vstack()
                .align_items(Align::Center)
                .justify(Justify::End)(
                    detail::vstack()
                        .bg(Color::default_color())(cfg_.overlay))
        });
    }

private:
    Config cfg_;
};

} // namespace maya
