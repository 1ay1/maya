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
        // The wrapper that owns bg + horizontal centering used to be
        // an `auto`-width vstack hugging cfg_.overlay. That's fine for
        // a fixed-size overlay, but it traps any `Dimension::percent`
        // settings inside `cfg_.overlay`: percent resolves against the
        // immediate parent's WIDTH, and an auto-width parent's width
        // is the overlay's NATURAL content size — circular, so a
        // percent gets you "85% of natural" instead of "85% of
        // terminal". Giving the bg-vstack an explicit `percent(100)`
        // width breaks the cycle: it spans the full screen, percent
        // settings on cfg_.overlay now resolve against the terminal,
        // and `align_items(Center)` keeps the overlay visually
        // centered in the wide bg strip. No API change for callers —
        // a fixed-width overlay still renders the same.
        return detail::zstack({
            cfg_.base,
            detail::vstack()
                .align_items(Align::Center)
                .justify(Justify::End)(
                    detail::vstack()
                        .width(Dimension::percent(100))
                        .align_items(Align::Center)
                        .bg(Color::default_color())(cfg_.overlay))
        });
    }

private:
    Config cfg_;
};

} // namespace maya
