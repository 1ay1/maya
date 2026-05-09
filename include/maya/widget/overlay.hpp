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
        // The bg-vstack used to be auto-width, hugging cfg_.overlay's
        // natural content size. That trapped any flex sizing inside
        // the overlay: `Dimension::percent` resolved against natural
        // content (circular), `align_self(Stretch)` matched the same
        // natural width, and a pickers list of long mp3 names with
        // `text(...) | clip` would never expand even on a 200-col
        // terminal — the parent's "available width" was just the
        // overlay's own natural width.
        //
        // The fix: the bg-vstack is now `width(percent(100))` so it
        // spans the full screen, with `padding(0, 2)` for breathing
        // room from the terminal edges. Default align_items=Stretch
        // (Yoga) means cfg_.overlay's natural sizing genuinely
        // stretches to the bg-vstack's content width — caller
        // controls actual width through `min_width` / `max_width` on
        // its own root vstack:
        //
        //   vstack()
        //     .min_width(Dimension::fixed(50))   // narrow-term floor
        //     .max_width(Dimension::fixed(180))  // wide-term ceiling
        //     ...                                // no .width() needed
        //
        // Existing fixed-width callers (legacy modals that set
        // .width(Dimension::fixed(N)) explicitly) keep working — the
        // child's explicit width overrides the stretch.
        return detail::zstack({
            cfg_.base,
            detail::vstack()
                .align_items(Align::Center)
                .justify(Justify::End)(
                    detail::vstack()
                        .width(Dimension::percent(100))
                        .padding(0, 2)
                        .bg(Color::default_color())(cfg_.overlay))
        });
    }

private:
    Config cfg_;
};

} // namespace maya
