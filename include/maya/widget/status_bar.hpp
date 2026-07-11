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
// Width-adaptive by MEASUREMENT: the activity row builds its real
// fragments at progressively leaner shapes and takes the richest one
// whose measured width fits — no thresholds, nothing to tune.
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

#include <algorithm>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../element/text.hpp"
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

        // NO width thresholds. The activity row is a measured
        // degradation ladder: every piece is built as its real styled
        // fragment and the row sheds detail step by step — breadcrumb
        // → ctx token counts → token-stream chip → phase elapsed → ctx
        // bar → phase verb → ctx gauge → model badge — until the
        // measured total fits the width the row was actually given.
        // The phase chip (what's happening now) is the last survivor.
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
        // The middle row (activity strip or toast) is hard-locked to
        // exactly one cell tall AND clipped. A squeezed sub-widget (the
        // context gauge, phase chip, model badge, breadcrumb) can word-
        // wrap its text to a second line when the terminal is narrow;
        // that 2-line child would otherwise paint past the bottom accent
        // and grow the whole status bar from 3 rows to 4. height(1)
        // fixes the slot to one cell and overflow:Hidden clips anything
        // the (already width-pruned) content can't fit — so the bar is a
        // stable single line at every width, truncated at the edge
        // rather than wrapped onto a phantom row.
        rows.push_back((cfg_.status_banner.text.empty()
                            ? activity_row()
                            : toast_row())
                       | height(1) | overflow(Overflow::Hidden));
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

            auto width_of = [w](const Element& el) {
                return measure_element(el, w > 0 ? w : 1).width.value;
            };

            // ── Measured degradation ladder ──
            // Build the row's REAL fragments at progressively leaner
            // shapes and take the richest one whose measured width
            // fits. No thresholds, no estimates — a relabeled model
            // badge or a longer verb re-decides by itself. Shed order
            // (first → last): ctx token counts · token-stream chip ·
            // phase elapsed · ctx bar · phase verb · ctx gauge · model
            // badge. The breadcrumb (most expendable of all) is added
            // LAST from whatever measured leftover remains.
            struct Shape {
                bool tokens, stream, elapsed, bar, verb, gauge, badge;
            };
            static constexpr Shape kLadder[] = {
                {true,  true,  true,  true,  true,  true,  true },
                {false, true,  true,  true,  true,  true,  true },
                {false, false, true,  true,  true,  true,  true },
                {false, false, false, true,  true,  true,  true },
                {false, false, false, false, true,  true,  true },
                {false, false, false, false, false, true,  true },
                {false, false, false, false, false, false, true },
                {false, false, false, false, false, false, false},
            };

            Element right, phase_el, middle;
            bool stream_on = false;
            int fixed_w = 0;
            for (const Shape& s : kLadder) {
                PhaseChip::Config pc = cfg.phase;
                pc.verb_width = s.verb ? 10 : 0;
                if (!s.elapsed) pc.elapsed_secs = -1.0f;

                ContextGauge::Config ctx = cfg.context;
                ctx.show_bar    = s.bar;
                ctx.show_tokens = s.tokens;

                std::vector<Element> rparts;
                bool emitted_right = false;
                if (s.stream && !cfg.token_stream.adaptive) {
                    rparts.push_back(TokenStreamSparkline{cfg.token_stream}.build());
                    rparts.push_back(text("   \xc2\xb7   ", fg_dim_(muted)));
                    emitted_right = true;
                }
                if (s.badge) {
                    rparts.push_back(cfg.model_badge);
                    emitted_right = true;
                }
                if (cfg.context.max > 0 && s.gauge) {
                    if (emitted_right)
                        rparts.push_back(text(" \xc2\xb7 ", fg_dim_(muted)));
                    rparts.push_back(ContextGauge{ctx}.build());
                    emitted_right = true;
                }
                rparts.push_back(text(" "));
                Element cand_right = h(std::move(rparts)).build();
                Element cand_phase = PhaseChip{pc}.build();

                // The adaptive stream chip rides the middle slot and
                // shrinks by itself — it only needs a nominal minimum.
                const int middle_min = (s.stream && cfg.token_stream.adaptive)
                    ? 8 + 7   // spark floor + "   ·   " separator
                    : 0;

                const int need = 1 /*lead sp*/ + 1 /*rail*/ + 1 /*sp*/
                    + width_of(cand_phase) + middle_min
                    + width_of(cand_right);
                const bool last = &s == &kLadder[std::size(kLadder) - 1];
                if (need <= w || last) {
                    right     = std::move(cand_right);
                    phase_el  = std::move(cand_phase);
                    stream_on = s.stream;
                    fixed_w   = need;
                    middle = (s.stream && cfg.token_stream.adaptive)
                        ? (h(TokenStreamSparkline{cfg.token_stream}.build(),
                             text("   \xc2\xb7   ", fg_dim_(muted))) | grow(1.0f)).build()
                        : Element{spacer().build()};
                    break;
                }
            }
            (void)stream_on;

            // ── Left group: breadcrumb + ▌ rail + phase chip. The
            //    breadcrumb gets exactly the measured leftover — it
            //    appears the moment its 14-cell floor + separator fit,
            //    capped at the config's max_chars.
            std::vector<Element> lparts;
            lparts.push_back(text(" "));
            if (!cfg.breadcrumb.title.empty()) {
                const int leftover = w - fixed_w - 7 /*  ·  sep*/;
                if (leftover >= 14) {
                    TitleChip::Config bc = cfg.breadcrumb;
                    bc.max_chars = static_cast<std::size_t>(
                        std::min(leftover, static_cast<int>(
                            cfg.breadcrumb.max_chars > 0 ? cfg.breadcrumb.max_chars
                                                         : 28)));
                    lparts.push_back(TitleChip{bc}.build());
                    lparts.push_back(text("   \xc2\xb7   ", fg_dim_(muted)));   // ·
                }
            }
            Style rail_style = active
                ? Style{}.with_fg(pcolor).with_bold()
                : Style{}.with_fg(pcolor).with_dim();
            lparts.push_back(text("\xe2\x96\x8c", rail_style));             // ▌
            lparts.push_back(text(" "));
            lparts.push_back(std::move(phase_el));
            auto left = h(std::move(lparts));

            // overflow:Hidden guarantees the activity row is ALWAYS
            // exactly one line: if even the leanest measured shape can't
            // fit an extremely narrow terminal, the content is clipped
            // at the right edge rather than wrapping onto a phantom
            // second row that would shove the Thread above it and
            // trigger a full-viewport repaint.
            return (h(left, std::move(middle), std::move(right))
                    | overflow(Overflow::Hidden)).build();
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
    // A leading rail glyph + message text, painted on an RGB-anchored
    // background that spans the entire row.
    //
    // We deliberately bypass the user's ANSI palette for the band's
    // bg/fg pair: terminal themes remap `bright_yellow`/`bright_cyan`
    // wildly (Gruvbox makes bright_yellow look like dark amber, Solarized
    // makes bright_cyan look teal), which means a luminance-based
    // contrast pick computed against the *default* xterm palette stops
    // matching reality. RGB triples render the same everywhere a
    // truecolor terminal is in use, so the band is guaranteed visible.
    //
    // Severity→palette:
    //   Error → deep red bg     + bright white fg
    //   Warn  → amber bg         + near-black fg
    //   Info  → desaturated indigo bg + bright white fg
    [[nodiscard]] Element toast_row() const {
        using namespace dsl;
        using Kind = StatusBanner::Kind;
        const Kind kind = cfg_.status_banner.effective_kind();

        // Theme-independent RGB palette. WCAG contrast ratios computed
        // against the chosen fg:
        //   error: #b91c1c on #ffffff → 7.6:1  (AAA)
        //   warn : #f59e0b on #0b0b0b → 9.4:1  (AAA)
        //   info : #3b4a6b on #ffffff → 8.9:1  (AAA)
        struct Palette { Color bg; Color fg; Color rail; };
        const Palette p =
            (kind == Kind::Error)
                ? Palette{ Color::rgb(185,  28,  28),   // crimson
                           Color::rgb(255, 255, 255),
                           Color::rgb(255, 220, 220) }  // pale-red rail
          : (kind == Kind::Warn)
                ? Palette{ Color::rgb(245, 158,  11),   // amber
                           Color::rgb( 17,  17,  17),
                           Color::rgb( 60,  40,   0) }  // dark-brown rail
                : Palette{ Color::rgb( 59,  74, 107),   // indigo-slate
                           Color::rgb(255, 255, 255),
                           Color::rgb(140, 180, 255) }; // sky rail

        const std::string& msg = cfg_.status_banner.text;
        const char* glyph =
            (kind == Kind::Error) ? "\xe2\x9c\x97"   // ✗
          : (kind == Kind::Warn)  ? "\xe2\x9a\xa0"   // ⚠
                                  : "\xe2\x96\xb6"; // ▶

        // Render as a two-segment row: a 1-cell rail in `rail` color,
        // then the body in `fg`-on-`bg`. The rail is painted on the
        // same bg so it reads as a brighter accent stripe inside the
        // band rather than an external chip floating on the panel.
        return component([p, msg, glyph](int w, int /*h*/) -> Element {
            using namespace dsl;
            if (w <= 0) return blank().build();

            // Layout: "▎ ▶  message..........."
            //          ^   ^^^                  trailing bg-painted pad
            //          |   glyph + 2sp
            //          rail (▎, 1 col)
            //
            // Reserved cells: rail(1) + sp(1) + glyph(1) + sp(2) = 5
            const std::string rail_g = "\xe2\x96\x8e";   // ▎ left-7eighths
            const std::string g_str  = glyph;
            const int reserved       = 5;

            std::string body;
            int rail_w = 0;
            int pre_w  = 0;   // width of everything before the message

            if (w >= reserved + 1) {
                // Full layout.
                body  = " " + g_str + "  ";
                rail_w = 1;
                pre_w  = 4;   // sp + glyph + 2sp (rail is rendered separately)
                const int avail = w - reserved;
                body += (string_width(msg) <= avail) ? msg : truncate_end(msg, avail);
            } else if (w >= 4) {
                // Tight: rail + glyph + sp + 1+ char of message.
                body  = " " + g_str + " ";
                rail_w = 1;
                pre_w  = 3;
                body += truncate_end(msg, w - 4);
            } else if (w >= 2) {
                // Very tight: just rail + truncated message.
                rail_w = 1;
                pre_w  = 0;
                body   = truncate_end(msg, w - 1);
            } else {
                // 1 cell — just paint the rail and stop.
                rail_w = 1;
            }

            // Pad body to fill the remaining columns with bg-painted spaces.
            const int used = string_width(body);
            const int total_after_rail = w - rail_w;
            if (used < total_after_rail)
                body.append(static_cast<std::size_t>(total_after_rail - used), ' ');

            // Two elements composed horizontally: the rail and the body.
            // Rail uses fg=rail-color on the band bg; body uses fg on bg.
            auto rail_el = (rail_w > 0)
                ? text(rail_g, Style{}.with_fg(p.rail).with_bg(p.bg)).build()
                : blank().build();
            auto body_el = text(std::move(body),
                                Style{}.with_fg(p.fg).with_bg(p.bg)).build();
            (void)pre_w;   // reserved for future right-aligned timestamp slot
            return h(std::move(rail_el), std::move(body_el)).build();
        });
    }

};

} // namespace maya
