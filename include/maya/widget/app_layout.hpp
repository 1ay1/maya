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

        // Vertical layout discipline:
        //   • Thread grows to fill the leftover height, shrinks freely
        //     down to min_height=0 if the bottom siblings need their
        //     full natural sizes.
        //   • ChangesStrip, Composer, StatusBar are PINNED at their
        //     natural heights via shrink(0). Without this, flex would
        //     proportionally shrink every child when content exceeds
        //     the available height — making the composer vanish during
        //     streaming until a terminal resize forces a relayout.
        //
        // Mirrors the messenger example's discipline (build_messages_box
        // grows; header_box / composer_box use fixed heights + shrink(0)).
        auto thread_box = (vstack()
            .grow(1.0f)
            .shrink(1.0f)
            .min_height(Dimension::fixed(0))
            .overflow(Overflow::Hidden)
            (Thread{cfg_.thread}.build())).build();

        auto strip_box = (vstack()
            .grow(0).shrink(0)
            (ChangesStrip{cfg_.changes_strip}.build())).build();

        auto composer_box = (vstack()
            .grow(0).shrink(0)
            (Composer{cfg_.composer}.build())).build();

        auto status_box = (vstack()
            .grow(0).shrink(0)
            (StatusBar{cfg_.status_bar}.build())).build();

        // In inline mode the root is auto-height: a plain v(...)
        // sizes to natural content, leaving a gap below the status
        // bar when content is shorter than the viewport. Pin the
        // outer vstack to at least viewport height so thread_box's
        // grow(1) eats the slack and composer + status sit at the
        // bottom edge. When content overflows viewport, min_height
        // is non-binding and the layout grows past it normally
        // (inline mode then scrolls upper content into native
        // scrollback as expected).
        const int term_h = available_height();
        auto base = (vstack()
            .grow(1.0f)
            .min_height(Dimension::fixed(term_h))
            .padding(1)
            (
                std::move(thread_box),
                std::move(strip_box),
                std::move(composer_box),
                std::move(status_box)
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
