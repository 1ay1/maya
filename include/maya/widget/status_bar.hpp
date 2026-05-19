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
#include <cmath>

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
    // bg by `pick_fg_for_bg` (luminance-based), which fixes the
    // black-on-grey invisibility bug in the Idle phase where
    // `phase_color` is `bright_black`.
    [[nodiscard]] Element toast_row() const {
        using namespace dsl;
        using Kind = StatusBanner::Kind;
        const Kind kind = cfg_.status_banner.effective_kind();

        // Pick a safer Info bg: if the phase color is too dark or too
        // light to host black/white text comfortably, fall back to a
        // neutral mid-tone derived from the phase color. Warn/Error
        // keep their semantic colors — they're chosen for visibility.
        Color bg = (kind == Kind::Error) ? cfg_.status_banner.error_color
                 : (kind == Kind::Warn)  ? cfg_.status_banner.warn_color
                                         : info_bg(cfg_.phase_color);
        Color fg = pick_fg_for_bg(bg);

        const std::string& msg = cfg_.status_banner.text;
        const char* glyph =
            (kind == Kind::Error) ? " \xe2\x9c\x97  "   // ✗
          : (kind == Kind::Warn)  ? " \xe2\x9a\xa0  "   // ⚠
                                  : " \xe2\x96\xb6  ";  // ▶

        // No `with_bold()` on the painted band: on many terminals bold
        // remaps fg to the bright variant of its ANSI slot, which
        // sabotages the luminance contrast we just computed
        // (e.g. bold black → bright_black on dark amber).
        return component([bg, fg, msg, glyph](int w, int /*h*/) -> Element {
            using namespace dsl;
            if (w <= 0) return blank().build();

            // Truly width-responsive composition. All sizing goes
            // through `text::string_width` (display columns, wide-char
            // aware) instead of byte length — a 3-byte glyph like "▶"
            // is 1 column, and CJK in the message body is 2 columns.
            //
            // Budget order (shrink most-disposable first):
            //   1. full:     " ▶  message..."      (glyph + 2sp gap + msg)
            //   2. tight:    " ▶ msg…"             (glyph + 1sp + truncated)
            //   3. tiny:     " msg…"                (drop glyph entirely)
            //   4. minimal:  "…" or even blank      (still a colored band)
            // The trailing pad is painted at the bg color so the
            // toast reads as one continuous band the full row wide.
            const std::string g_str{glyph};
            const int g_w   = string_width(g_str);
            const int msg_w = string_width(msg);

            std::string body;
            if (g_w + msg_w + 1 <= w) {
                // Full fit with the original glyph + leading space.
                body = g_str + msg;
            } else if (g_w + 2 < w) {
                // Glyph fits with at least 1 char of message + ellipsis.
                const int budget = w - g_w;     // room left for message
                body = g_str + truncate_end(msg, budget);
            } else if (w >= 3) {
                // Drop the glyph entirely; just truncate the message.
                // Lead with a single space so the colored band has a
                // bit of breathing room before the text.
                body = " " + truncate_end(msg, w - 1);
            } else {
                // Pathologically narrow (w ≤ 2). Render only the
                // background band — better an unmistakable colored
                // strip than a clipped, unreadable glyph.
                body.clear();
            }

            // Pad to exactly `w` display columns with bg-painted spaces.
            const int used = string_width(body);
            if (used < w) body.append(static_cast<std::size_t>(w - used), ' ');
            // No `else` for over-budget: truncate_end already capped us.

            return text(std::move(body),
                        Style{}.with_fg(fg).with_bg(bg)).build();
        });
    }

    // Approximate sRGB relative luminance of `c`, in [0, 1].
    // For named ANSI colors we use a tuned table (terminal palettes
    // vary, but the rank order — black darkest, bright_white
    // lightest — is stable). For RGB/indexed we compute properly.
    [[nodiscard]] static float luminance(Color c) noexcept {
        switch (c.kind()) {
            case Color::Kind::Named: {
                // 16-slot ANSI luminance, hand-tuned to typical
                // xterm/iterm/wezterm/alacritty defaults.
                static constexpr float kAnsiLum[16] = {
                    0.00f, // 0  black
                    0.30f, // 1  red
                    0.45f, // 2  green
                    0.55f, // 3  yellow
                    0.20f, // 4  blue
                    0.30f, // 5  magenta
                    0.50f, // 6  cyan
                    0.75f, // 7  white       (typically light grey)
                    0.35f, // 8  bright_black (typically dark grey)
                    0.50f, // 9  bright_red
                    0.70f, // 10 bright_green
                    0.85f, // 11 bright_yellow
                    0.40f, // 12 bright_blue
                    0.50f, // 13 bright_magenta
                    0.75f, // 14 bright_cyan
                    1.00f, // 15 bright_white
                };
                return kAnsiLum[c.index() & 0xF];
            }
            case Color::Kind::Rgb: {
                auto srgb = [](uint8_t v) {
                    float x = static_cast<float>(v) / 255.0f;
                    return x <= 0.03928f ? x / 12.92f
                                         : std::pow((x + 0.055f) / 1.055f, 2.4f);
                };
                return 0.2126f * srgb(c.r())
                     + 0.7152f * srgb(c.g())
                     + 0.0722f * srgb(c.b());
            }
            case Color::Kind::Indexed: {
                // Reasonable approximation for the 6x6x6 cube + greys.
                uint8_t i = c.index();
                if (i < 16) {
                    Color named{static_cast<AnsiColor>(i)};
                    return luminance(named);
                }
                if (i >= 232) {
                    // 24-step greyscale ramp
                    float v = (static_cast<float>(i - 232) + 1.0f) / 25.0f;
                    return v;
                }
                int n  = i - 16;
                int r6 = (n / 36) % 6;
                int g6 = (n / 6) % 6;
                int b6 = n % 6;
                auto ramp = [](int s) -> uint8_t {
                    static constexpr uint8_t k[6] = {0, 95, 135, 175, 215, 255};
                    return k[s];
                };
                return luminance(Color::rgb(ramp(r6), ramp(g6), ramp(b6)));
            }
            case Color::Kind::Default:
                // Caller's terminal default — assume mid-grey so we
                // pick a fg that's readable either way (bright_white).
                return 0.5f;
        }
        return 0.5f;
    }

    // Pick black or bright_white for foreground based on bg luminance.
    // Threshold ~0.55: leans toward bright_white so muted/dark info
    // bgs (bright_black in Idle) render as white-on-grey rather than
    // the previous black-on-grey (invisible).
    [[nodiscard]] static Color pick_fg_for_bg(Color bg) noexcept {
        return luminance(bg) >= 0.55f ? Color::black() : Color::bright_white();
    }

    // If the phase color sits in the no-contrast band (very dark, like
    // `bright_black` in Idle), swap to a richer mid-tone so the toast
    // is visibly distinct from the surrounding panel chrome.
    [[nodiscard]] static Color info_bg(Color phase) noexcept {
        float L = luminance(phase);
        if (L < 0.25f) {
            // Dark phase color (Idle/muted) → use blue for an
            // unmistakable "info" band rather than dark-grey-on-panel.
            return Color::blue();
        }
        return phase;
    }
};

} // namespace maya
