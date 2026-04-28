#pragma once
// maya::widget::StatusBar — bottom-of-app activity / status / shortcuts row.
//
// Five-row panel composed of nested sub-widget Configs. Hosts fill
// each sub-Config (or one Config builder per sub-widget — the moha
// adapters do this) and StatusBar drives the layout:
//
//   ▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔   ← PhaseAccent (top, dim)
//    TitleChip ·  PhaseChip   …    TokenStreamSparkline   ●Model · ContextGauge
//    StatusBanner  (always 1 row tall — no jitter)
//    ShortcutRow
//   ▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁   ← PhaseAccent (bottom)
//
// Width-adaptive: the activity row's optional pieces (breadcrumb,
// token stream slot, ctx bar) drop progressively below their
// thresholds. ShortcutRow handles its own width adaptation.
//
//   maya::StatusBar{{
//       .phase_color   = phase_color(m.s.phase),
//       .breadcrumb    = title_chip_config(m),
//       .phase         = phase_chip_config(m),
//       .token_stream  = token_stream_sparkline_config(m),
//       .model_badge   = model_badge_config(m).build(),
//       .context       = context_gauge_config(m),
//       .status_banner = status_banner_config(m),
//       .shortcuts     = shortcut_row_config(m),
//   }}.build();

#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../style/color.hpp"

#include "context_gauge.hpp"
#include "phase_accent.hpp"
#include "phase_chip.hpp"
#include "shortcut_row.hpp"
#include "status_banner.hpp"
#include "title_chip.hpp"
#include "token_stream_sparkline.hpp"

namespace maya {

class StatusBar {
public:
    struct Config {
        // Frame coloring — drives the top/bottom PhaseAccent strips
        // and the leading rail glyph next to the phase chip.
        Color phase_color = Color::cyan();

        // Activity row sub-widget configs.
        TitleChip::Config            breadcrumb;       // empty title = hide
        PhaseChip::Config            phase;
        TokenStreamSparkline::Config token_stream;
        Element                      model_badge{TextElement{}};
        ContextGauge::Config         context;          // max=0 = hide

        // Status row.
        StatusBanner::Config         status_banner;

        // Shortcut row.
        ShortcutRow::Config          shortcuts;

        // Width thresholds for activity-row pieces.
        int breadcrumb_min_width    = 130;
        int token_stream_min_width  = 110;
        int ctx_bar_min_width       = 55;
        int phase_verb_min_width    = 50;     // < this drops phase verb
        int phase_elapsed_min_width = 80;     // < this drops phase elapsed
    };

    explicit StatusBar(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        const Color pcolor = cfg_.phase_color;

        std::vector<Element> rows;
        rows.reserve(5);
        rows.push_back(PhaseAccent{{.color = pcolor,
                                    .position = PhaseAccent::Position::Top}}.build());
        rows.push_back(activity_row());
        rows.push_back(StatusBanner{cfg_.status_banner}.build());
        rows.push_back(ShortcutRow{cfg_.shortcuts}.build());
        rows.push_back(PhaseAccent{{.color = pcolor,
                                    .position = PhaseAccent::Position::Bottom}}.build());
        return v(std::move(rows)).build();
    }

private:
    Config cfg_;

    [[nodiscard]] Element activity_row() const {
        Config cfg = cfg_;
        return Element{ComponentElement{
            .render = [cfg = std::move(cfg)](int w, int /*h*/) -> Element {
                using namespace dsl;
                if (w <= 0) return Element{TextElement{}};

                const Color muted = Color::bright_black();
                const Color pcolor = cfg.phase_color;
                const bool  active = cfg.phase.breathing;

                // Width-adaptive copies of the sub-configs.
                PhaseChip::Config pc = cfg.phase;
                pc.verb_width   = (w < cfg.phase_verb_min_width)    ? 0    : 10;
                if (w < cfg.phase_elapsed_min_width) pc.elapsed_secs = -1.0f;

                ContextGauge::Config ctx = cfg.context;
                ctx.show_bar = (w >= cfg.ctx_bar_min_width);

                // ── Left group: breadcrumb (when wide enough) + ▌ rail + phase chip.
                std::vector<Element> lparts;
                lparts.push_back(text(" "));
                if (!cfg.breadcrumb.title.empty()
                    && w >= cfg.breadcrumb_min_width) {
                    TitleChip::Config bc = cfg.breadcrumb;
                    bc.max_chars = (w >= 170) ? 28 : (w >= 150) ? 20 : 14;
                    lparts.push_back(TitleChip{bc}.build());
                    lparts.push_back(text("   \xc2\xb7   ", fg_dim_(muted)));   // ·
                }
                Style rail_style = active
                    ? Style{}.with_fg(pcolor).with_bold()
                    : Style{}.with_fg(pcolor).with_dim();
                lparts.push_back(text("\xe2\x96\x8c", rail_style));             // ▌
                lparts.push_back(text(" "));
                lparts.push_back(PhaseChip{pc}.build());
                auto left = h(std::move(lparts));

                // ── Right group: tok-stream + model + ctx.
                std::vector<Element> rparts;
                if (w >= cfg.token_stream_min_width) {
                    rparts.push_back(TokenStreamSparkline{cfg.token_stream}.build());
                    rparts.push_back(text("   \xc2\xb7   ", fg_dim_(muted)));
                }
                rparts.push_back(cfg.model_badge);
                if (cfg.context.max > 0) {
                    rparts.push_back(text(" \xc2\xb7 ", fg_dim_(muted)));
                    rparts.push_back(ContextGauge{ctx}.build());
                }
                rparts.push_back(text(" "));
                auto right = h(std::move(rparts));

                return h(left, spacer(), right).build();
            },
            .layout = {},
        }};
    }

    static Style fg_dim_(Color c) {
        const bool already_muted =
            c.kind() == Color::Kind::Named
            && c.index() == static_cast<uint8_t>(AnsiColor::BrightBlack);
        return already_muted
            ? Style{}.with_fg(c)
            : Style{}.with_fg(c).with_dim();
    }
};

} // namespace maya
