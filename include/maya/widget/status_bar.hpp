#pragma once
// maya::widget::StatusBar — bottom-of-app activity / status / shortcuts row.
//
// Five-row panel composed of nested sub-widget Configs. Hosts fill
// each sub-Config (or one Config builder per sub-widget — the moha
// adapters do this) and StatusBar drives the layout:
//
//   ▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔   ← PhaseAccent (top, dim)
//    TitleChip ·  PhaseChip   …    TokenStreamSparkline   ●Model · ContextGauge
//   ▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁   ← PhaseAccent (bottom)
//
// When `status_banner.text` is non-empty the activity row is replaced
// by a full-width toast in the same vertical slot — bright foreground
// + accent background span the whole row so transient status ("context
// compacted", "retrying in 3s…", "error: rate limited") is impossible
// to miss. The toast collapses back to the regular activity row the
// moment its TTL expires; height stays constant either way.
//
// Width-adaptive: the activity row's optional pieces (breadcrumb,
// token stream slot, ctx bar) drop progressively below their
// thresholds.
//
//   maya::StatusBar{{
//       .phase_color   = phase_color(m.s.phase),
//       .breadcrumb    = title_chip_config(m),
//       .phase         = phase_chip_config(m),
//       .token_stream  = token_stream_sparkline_config(m),
//       .model_badge   = model_badge_config(m).build(),
//       .context       = context_gauge_config(m),
//       .status_banner = status_banner_config(m),  // toast — hides activity row when present
//   }}.build();

#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../style/color.hpp"

#include "context_gauge.hpp"
#include "phase_accent.hpp"
#include "phase_chip.hpp"
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
        Element                      model_badge;     // default-empty
        ContextGauge::Config         context;          // max=0 = hide

        // Status row — takes over the activity_row slot when present.
        StatusBanner::Config         status_banner;

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

        // Toast takeover: when a status message is set, the middle row
        // becomes a full-width banner instead of the regular activity
        // strip. Same height, same surrounding accents — just the
        // payload swaps. Maximally visible: bright fg + accent bg span
        // the entire content row so the message reads as a screen-wide
        // alert, not a tucked-in subtitle. Once the TTL expires the
        // host clears `status_banner.text` and the activity row
        // returns. Five-row layout collapses to three rows now that
        // ShortcutRow is gone (shortcuts moved to the welcome screen).
        std::vector<Element> rows;
        rows.reserve(3);
        rows.push_back(PhaseAccent{{.color = pcolor,
                                    .position = PhaseAccent::Position::Top}}.build());
        rows.push_back(cfg_.status_banner.text.empty()
                           ? activity_row()
                           : toast_row());
        rows.push_back(PhaseAccent{{.color = pcolor,
                                    .position = PhaseAccent::Position::Bottom}}.build());
        return v(std::move(rows)).build();
    }

private:
    Config cfg_;

    [[nodiscard]] Element activity_row() const {
        using namespace dsl;
        return component([cfg = cfg_](int w, int /*h*/) -> Element {
            using namespace dsl;
            if (w <= 0) return blank().build();

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
        });
    }

    static Style fg_dim_(Color c) {
        const bool already_muted =
            c.kind() == Color::Kind::Named
            && c.index() == static_cast<uint8_t>(AnsiColor::BrightBlack);
        return already_muted
            ? Style{}.with_fg(c)
            : Style{}.with_fg(c).with_dim();
    }

    // Full-width toast row — the activity slot's high-visibility twin.
    // A leading rail glyph + bold message text, painted on a colored
    // background that spans the entire row so the notification is
    // impossible to miss. The background reflects the banner's `kind`:
    //   Info  → phase_color (cyan/green/… — "things are happening")
    //   Warn  → yellow      (retry, awaiting permission — "attention")
    //   Error → red         (rate-limited, transport gave up)
    // The foreground is then chosen for legible contrast against each
    // bg: dark text on the bright info/warn tints (bright_white on cyan
    // is the failure mode this widget existed to fix), bright_white on
    // the deep error red. Picking from the named-ANSI set keeps the
    // user's terminal theme in charge of the actual hue.
    [[nodiscard]] Element toast_row() const {
        using namespace dsl;
        using Kind = StatusBanner::Kind;
        const Kind kind = cfg_.status_banner.effective_kind();

        Color bg;
        Color fg;
        switch (kind) {
            case Kind::Error:
                bg = cfg_.status_banner.error_color;   // red
                fg = Color::bright_white();            // white-on-red: classic alert contrast
                break;
            case Kind::Warn:
                bg = cfg_.status_banner.warn_color;    // yellow
                fg = Color::black();                   // dark-on-amber: the only readable choice
                break;
            case Kind::Info:
            default:
                bg = cfg_.phase_color;                 // tracks the phase (streaming=cyan, awaiting=yellow, …)
                fg = Color::black();                   // bright_white on bright_cyan washes out on most themes
                break;
        }

        const std::string& msg = cfg_.status_banner.text;
        const char* glyph = (kind == Kind::Info)
            ? " \xe2\x96\xb6  "   // ▶
            : " \xe2\x9a\xa0  ";  // ⚠

        return component([bg, fg, msg, glyph](int w, int /*h*/) -> Element {
            using namespace dsl;
            if (w <= 0) return blank().build();
            // Compose a single TextElement that spans the full row
            // width — the bg attribute paints every cell, including
            // the trailing pad, so the toast reads as one continuous
            // colored band rather than a chip floating on the panel.
            const std::string prefix = glyph;
            std::string content = prefix + msg;
            const int used = static_cast<int>(content.size());
            if (used < w) content.append(static_cast<std::size_t>(w - used), ' ');
            else if (used > w) content.resize(static_cast<std::size_t>(w));
            return text(std::move(content),
                        Style{}.with_fg(fg).with_bg(bg).with_bold()).build();
        });
    }
};

} // namespace maya
