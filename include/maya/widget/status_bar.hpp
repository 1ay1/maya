#pragma once
// maya::widget::StatusBar — bottom-of-app activity / status / shortcuts row.
//
// Five-row panel:
//   ▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔   ← phase accent (top)
//    ▎ Title  ·  ▌ ⠋ Streaming   4.2s    …   ⚡ 23.4 ▏▂▃▄… 1234   ● Opus 4.7 · CTX 18.0k/200.0k █████░░░░░  9%
//    ⚠  status banner if any (else blank — fixed-height to prevent jitter)
//    ^K palette   ^J threads   ^T todo   …   ^C quit
//   ▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁   ← phase accent (bottom)
//
// Width-adaptive: the activity row's right group drops items
// progressively, and the shortcut row drops less-important bindings
// + labels on narrow widths. The status row is ALWAYS 1 cell tall
// so the composer above never bobs vertically when a toast appears
// or disappears.
//
//   maya::StatusBar{{
//       .phase = {.glyph="\xe2\xa0\x8b", .verb="Streaming",
//                 .color=Color::cyan(), .breathing=true,
//                 .frame=spinner_frame, .elapsed_secs=4.2f},
//       .breadcrumb_title = "implement /loop dynamic mode",
//       .token_stream = {.show=true, .rate=23.4f, .total=1234,
//                        .history=hist_vec, .color=Color::cyan(),
//                        .live=true},
//       .model_badge = ModelBadge{model_id}.set_compact(true),
//       .context = {.used=18432, .max=200000},
//       .status_text = "",  // empty = blank slot
//       .shortcuts = { {.key="^K", .label="palette", .priority=10}, … },
//   }}.build();

#include <algorithm>
#include <cstdio>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

#include "context_gauge.hpp"
#include "phase_accent.hpp"
#include "phase_chip.hpp"
#include "shortcut_row.hpp"

namespace maya {

class StatusBar {
public:
    struct PhaseSpec {
        std::string glyph;
        std::string verb;
        Color       color        = Color::cyan();
        bool        breathing    = false;       // active state — animate glyph
        int         frame        = 0;
        float       elapsed_secs = -1.0f;       // < 0 = omit
    };

    struct TokenStreamSpec {
        bool                show    = false;
        float               rate    = 0.0f;
        int                 total   = 0;
        std::vector<float>  history;
        Color               color   = Color::cyan();
        bool                live    = false;     // dim when paused
    };

    struct ContextSpec {
        int used = 0;
        int max  = 0;
    };

    struct Config {
        PhaseSpec       phase;
        std::string     breadcrumb_title;        // empty = hide
        TokenStreamSpec token_stream;
        Element         model_badge{TextElement{}};
        ContextSpec     context;

        // Transient banner: empty `status_text` = blank slot (still 1
        // row tall, so layout above doesn't bob).
        std::string     status_text;
        bool            status_is_error = false;

        std::vector<ShortcutRow::Binding> shortcuts;

        // Width thresholds — sensible defaults baked in.
        int breadcrumb_min_width = 130;          // raise to 160 while streaming
        int token_stream_min_width = 110;
        int ctx_bar_min_width = 55;

        Color text_color = Color::bright_white();
    };

    explicit StatusBar(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        const Color pcolor = cfg_.phase.color;

        std::vector<Element> rows;
        rows.push_back(PhaseAccent{{.color = pcolor,
                                    .position = PhaseAccent::Position::Top}}.build());
        rows.push_back(activity_row());
        rows.push_back(status_row());
        rows.push_back(shortcut_row());
        rows.push_back(PhaseAccent{{.color = pcolor,
                                    .position = PhaseAccent::Position::Bottom}}.build());
        return v(std::move(rows)).build();
    }

private:
    Config cfg_;

    // ── Activity row — width-adaptive ─────────────────────────────────────
    [[nodiscard]] Element activity_row() const {
        // Capture by value so the ComponentElement lambda outlives build().
        Config cfg = cfg_;
        return Element{ComponentElement{
            .render = [cfg = std::move(cfg)](int w, int /*h*/) -> Element {
                using namespace dsl;
                if (w <= 0) return Element{TextElement{}};

                const Color muted = Color::bright_black();
                const Color pcolor = cfg.phase.color;
                const bool  active = cfg.phase.breathing;

                // Verb width: drop verb entirely below 50 cols.
                int verb_width = (w < 50) ? 0 : 10;
                // Elapsed only when there's room (≥ 80 cols).
                float elapsed = (w >= 80) ? cfg.phase.elapsed_secs : -1.0f;

                auto phase_pill = PhaseChip{{
                    .glyph        = cfg.phase.glyph,
                    .verb         = cfg.phase.verb,
                    .color        = pcolor,
                    .breathing    = cfg.phase.breathing,
                    .frame        = cfg.phase.frame,
                    .verb_width   = verb_width,
                    .elapsed_secs = elapsed,
                }}.build();

                // ── Left group: breadcrumb + ▌ rail + phase chip.
                std::vector<Element> lparts;
                lparts.push_back(text(" "));
                if (!cfg.breadcrumb_title.empty()
                    && w >= cfg.breadcrumb_min_width) {
                    std::size_t budget = (w >= 170) ? 28
                                       : (w >= 150) ? 20
                                                    : 14;
                    lparts.push_back(h(
                        text("\xe2\x96\x8e", Style{}.with_fg(pcolor)),  // ▎
                        text(" " + truncate_middle(cfg.breadcrumb_title, budget),
                             Style{}.with_fg(cfg.text_color).with_bold()),
                        text("   \xc2\xb7   ", fg_dim_(muted))           // ·
                    ).build());
                }
                Style rail_style = active
                    ? Style{}.with_fg(pcolor).with_bold()
                    : Style{}.with_fg(pcolor).with_dim();
                lparts.push_back(text("\xe2\x96\x8c", rail_style));      // ▌
                lparts.push_back(text(" "));
                lparts.push_back(phase_pill);

                auto left = h(std::move(lparts));

                // ── Right group: tok-stream + model + ctx.
                std::vector<Element> rparts;

                if (cfg.token_stream.show
                    && w >= cfg.token_stream_min_width) {
                    rparts.push_back(token_stream_compact(
                        cfg.token_stream.rate,
                        cfg.token_stream.total,
                        std::span<const float>{cfg.token_stream.history.data(),
                                               cfg.token_stream.history.size()},
                        cfg.token_stream.color,
                        cfg.token_stream.live));
                    rparts.push_back(sep_dot_());
                }

                rparts.push_back(cfg.model_badge);

                if (cfg.context.max > 0) {
                    rparts.push_back(sep_thin_());
                    rparts.push_back(ContextGauge{{
                        .used     = cfg.context.used,
                        .max      = cfg.context.max,
                        .show_bar = (w >= cfg.ctx_bar_min_width),
                    }}.build());
                }

                rparts.push_back(text(" "));
                auto right = h(std::move(rparts));

                return h(left, spacer(), right).build();
            },
            .layout = {},
        }};
    }

    // ── Status row — fixed-height (1 cell) so composer above doesn't bob.
    [[nodiscard]] Element status_row() const {
        using namespace dsl;
        const Color muted = Color::bright_black();

        if (cfg_.status_text.empty()) {
            return text(" ");                   // blank-but-present
        }
        const Color bc = cfg_.status_is_error
            ? Color::red()
            : muted;
        return h(
            text(" "),
            text("\xe2\x96\x8e", Style{}.with_fg(bc)),    // ▎
            text(cfg_.status_is_error ? " \xe2\x9a\xa0  " : "  ",
                 Style{}.with_fg(bc)),
            text(cfg_.status_text, Style{}.with_fg(bc).with_italic())
        ).build();
    }

    // ── Shortcut row — delegate to ShortcutRow widget.
    [[nodiscard]] Element shortcut_row() const {
        return ShortcutRow{{
            .bindings        = cfg_.shortcuts,
            .label_min_width = 110,
            .full_min_width  = 55,
            .text_color      = cfg_.text_color,
        }}.build();
    }

    // ── Compact tok/s + sparkline (status-bar specific layout) ────────────
    //
    // 37 cells: ⚡ ▕rate 5▏ t/s ▕spark 16▏ ▕total 5▏
    [[nodiscard]] static Element token_stream_compact(
        float rate, int total_tok,
        std::span<const float> hist, Color color, bool live)
    {
        using namespace dsl;
        const Color muted = Color::bright_black();
        static constexpr const char* kBlocks[8] = {
            "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
            "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88",
        };
        constexpr int kSparkCells = 16;

        if (rate < 0.0f) rate = 0.0f;
        Color rc = (rate > 50.0f)  ? Color::green()
                 : (rate >= 20.0f) ? Color::yellow()
                                   : Color::red();

        char rate_buf[16];
        if      (rate <    100.0f) std::snprintf(rate_buf, sizeof(rate_buf), "%5.1f",  static_cast<double>(rate));
        else if (rate <  10000.0f) std::snprintf(rate_buf, sizeof(rate_buf), "%5.0f",  static_cast<double>(rate));
        else                       std::snprintf(rate_buf, sizeof(rate_buf), "%4.0fk", static_cast<double>(rate) / 1000.0);

        std::string spark;
        spark.reserve(kSparkCells * 3);
        float lo = 0.0f, hi = 1.0f;
        if (!hist.empty()) {
            lo = *std::min_element(hist.begin(), hist.end());
            hi = *std::max_element(hist.begin(), hist.end());
            if (hi - lo < 0.001f) hi = lo + 1.0f;
        }
        int filled = std::min(kSparkCells, static_cast<int>(hist.size()));
        int pad    = kSparkCells - filled;
        for (int i = 0; i < pad; ++i) spark += kBlocks[0];
        for (int i = 0; i < filled; ++i) {
            std::size_t hidx = hist.size()
                             - static_cast<std::size_t>(filled)
                             + static_cast<std::size_t>(i);
            float norm = std::clamp((hist[hidx] - lo) / (hi - lo), 0.0f, 1.0f);
            int level = std::clamp(static_cast<int>(norm * 7.0f + 0.5f), 0, 7);
            spark += kBlocks[level];
        }

        Style spark_style = live ? Style{}.with_fg(color)
                                 : Style{}.with_fg(color).with_dim();
        Style rate_style  = live ? Style{}.with_fg(rc).with_bold()
                                 : Style{}.with_fg(rc).with_dim();

        return h(
            text("\xe2\x9a\xa1 ", Style{}.with_fg(rc)),                 // ⚡
            text(std::string{rate_buf}, rate_style),
            text(" t/s ", fg_dim_(muted)),
            text(std::move(spark), spark_style),
            text(" "),
            text(format_tokens_(total_tok), fg_dim_(muted))
        ).build();
    }

    static std::string format_tokens_(int n) {
        char buf[16];
        if (n >= 1'000'000) {
            std::snprintf(buf, sizeof(buf), "%5.1fM",
                          static_cast<double>(n) / 1'000'000.0);
        } else if (n >= 1000) {
            std::snprintf(buf, sizeof(buf), "%5.1fk",
                          static_cast<double>(n) / 1000.0);
        } else {
            std::snprintf(buf, sizeof(buf), "%5d", n);
        }
        return buf;
    }

    static std::string truncate_middle(std::string_view s, std::size_t max_chars) {
        if (s.size() <= max_chars) return std::string{s};
        if (max_chars <= 1) return "\xe2\x80\xa6";    // …
        return std::string{s.substr(0, max_chars - 1)} + "\xe2\x80\xa6";
    }

    static Element sep_dot_() {
        return dsl::text("   \xc2\xb7   ", fg_dim_(Color::bright_black())).build();
    }
    static Element sep_thin_() {
        return dsl::text(" \xc2\xb7 ", fg_dim_(Color::bright_black())).build();
    }

    static Style fg_dim_(Color c) {
        const bool is_already_muted =
            c.kind() == Color::Kind::Named
            && c.index() == static_cast<uint8_t>(AnsiColor::BrightBlack);
        return is_already_muted
            ? Style{}.with_fg(c)
            : Style{}.with_fg(c).with_dim();
    }
};

} // namespace maya
