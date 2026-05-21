#pragma once
// maya::widget::AppLayout — top-level chat-app frame.
//
// Composes the four canonical chat-app rows + overlay slot:
//
//   ┌─────────────────────────────────────────┐  ↑
//   │                                         │  │
//   │  Thread (grows to fill)                 │  │
//   │                                         │  │
//   │ ┌─────────────────────────────────────┐ │  │ pad<1>
//   │ │  ChangesStrip (optional)            │ │  │
//   │ ├─────────────────────────────────────┤ │  │
//   │ │  Composer                           │ │  │
//   │ └─────────────────────────────────────┘ │  │
//   │  StatusBar                              │  │
//   └─────────────────────────────────────────┘  ↓
//
// Every section is a nested widget Config — host apps construct one
// `AppLayout::Config` per frame and `AppLayout::build()` invokes the
// sub-widgets internally. No Element construction in the host.
//
// `overlay_present == true` floats `overlay` (centered, bottom-pinned,
// opaque background) over the base. Empty Element + present=false
// collapses cleanly to just the base — host doesn't need an `if`.
//
//   maya::AppLayout{{
//       .thread        = thread_cfg(m),
//       .changes_strip = changes_strip_cfg(m),
//       .composer      = composer_cfg(m),
//       .status_bar    = status_bar_cfg(m),
//       .overlay         = pick_overlay(m),       // Element (modal etc.)
//       .overlay_present = m.has_modal_open,
//   }}.build();

#include <optional>
#include <utility>

#include "../core/render_context.hpp"
#include "../dsl.hpp"
#include "../element/element.hpp"

#include "changes_strip.hpp"
#include "composer.hpp"
#include "overlay.hpp"
#include "status_bar.hpp"
#include "thread.hpp"

namespace maya {

class AppLayout {
public:
    struct Config {
        Thread::Config        thread;
        ChangesStrip::Config  changes_strip;
        Composer::Config      composer;
        StatusBar::Config     status_bar;

        // Overlay slot — nullopt collapses cleanly to just the base.
        std::optional<Element> overlay;
    };

    explicit AppLayout(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        // Structural shape — mirrors maya/examples/agent_session.cpp,
        // the reference inline-mode app. Children sit DIRECTLY in the
        // outer vstack at their natural heights. No per-child wrapper
        // boxes, no flex grow/shrink on children, no overflow setting
        // anywhere in this widget.
        //
        // Why this is the only correct shape for inline mode:
        //   The maya inline renderer commits overflow row-by-row into
        //   the terminal's native scrollback (see serialize.cpp). For
        //   that to work, the layout MUST be allowed to grow past the
        //   viewport — any ancestor that clips (overflow:Hidden) or
        //   compresses (shrink>0) the thread forces content to pile
        //   up inside the same viewport slot, and stale cells from a
        //   previously-taller frame leak through the new shorter slot
        //   at the thread↔composer seam. That was the live-streaming
        //   ghosting bug.
        //
        // Thread grows internally (Conversation::build() sets
        // grow(1.0f) + trailing spacer), which eats slack on short
        // content. Composer + StatusBar are natural-height by
        // construction (their build()s don't set grow), so they sit
        // at the end of the vstack and ride the viewport bottom.
        //
        // DO NOT wrap any child in a vstack/box here. DO NOT add
        // overflow(). DO NOT set shrink on the outer or any child.
        // The min_height + padding below are the only outer flex
        // properties this widget owns.
        const int term_h = available_height();
        auto base = (vstack()
            .min_height(Dimension::fixed(term_h))
            .padding(1)
            (
                Thread{cfg_.thread}.build(),
                ChangesStrip{cfg_.changes_strip}.build(),
                Composer{cfg_.composer}.build(),
                StatusBar{cfg_.status_bar}.build()
            )).build();

        Overlay::Config oc;
        oc.base = std::move(base);
        if (cfg_.overlay) {
            oc.overlay = *cfg_.overlay;
            oc.present = true;
        }
        return Overlay{std::move(oc)}.build();
    }

private:
    Config cfg_;
};

} // namespace maya
