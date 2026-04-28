#pragma once
// maya::widget::AppLayout — top-level chat-app frame.
//
// Composes the four canonical chat-app rows + overlay slot:
//
//   ┌─────────────────────────────────────────┐  ↑
//   │                                         │  │
//   │  thread_panel (grows to fill)           │  │
//   │                                         │  │
//   │ ┌─────────────────────────────────────┐ │  │ pad<1>
//   │ │  changes_strip (optional)           │ │  │
//   │ ├─────────────────────────────────────┤ │  │
//   │ │  composer                           │ │  │
//   │ └─────────────────────────────────────┘ │  │
//   │  status_bar                             │  │
//   └─────────────────────────────────────────┘  ↓
//
// When `overlay_present` is true, `overlay` floats over the base
// (horizontally centered, pinned to the bottom edge, with an opaque
// background). The widget delegates that to maya::Overlay.
//
// All four section elements are accepted as pre-built Elements —
// callers typically build them from their own widget Configs
// (Composer, StatusBar, ChangesStrip, …). Pass an empty Element
// for `changes_strip` to hide the slot entirely.
//
//   maya::AppLayout{{
//       .thread_panel    = thread_panel_element,
//       .changes_strip   = changes_strip_element,    // empty = hide
//       .composer        = composer_element,
//       .status_bar      = status_bar_element,
//       .overlay         = modal_element,            // when open
//       .overlay_present = m.has_modal_open,
//   }}.build();

#include <utility>

#include "../dsl.hpp"
#include "../element/element.hpp"

#include "overlay.hpp"

namespace maya {

class AppLayout {
public:
    struct Config {
        Element thread_panel{TextElement{}};
        Element changes_strip{TextElement{}};
        Element composer{TextElement{}};
        Element status_bar{TextElement{}};

        // Overlay slot — empty Element + present=false collapses to base.
        Element overlay{TextElement{}};
        bool    overlay_present = false;
    };

    explicit AppLayout(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        auto base = (v(
            v(cfg_.thread_panel) | grow(1.0f),
            cfg_.changes_strip,
            cfg_.composer,
            cfg_.status_bar
        ) | pad<1> | grow(1.0f)).build();

        return Overlay{{
            .base    = std::move(base),
            .overlay = cfg_.overlay,
            .present = cfg_.overlay_present,
        }}.build();
    }

private:
    Config cfg_;
};

} // namespace maya
