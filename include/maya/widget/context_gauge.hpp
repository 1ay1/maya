#pragma once
// maya::widget::ContextGauge — fuel-gauge bar for context-window fullness.
//
// Stable-width slot showing how full the model's context is, with a
// gradient bar coloured by zone (green safe → amber squeeze → red
// cliff). When `used == 0` the slot still occupies the same display
// columns but renders a dim placeholder ("CTX  ────────  ──%") so
// the surrounding right-group chips don't shove leftward when the
// first usage event arrives mid-stream.
//
//   maya::ContextGauge{{
//       .used     = 18'432,
//       .max      = 200'000,
//       .show_bar = true,        // false on narrow widths
//   }}.build();

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

class ContextGauge {
public:
    struct Config {
        int  used        = 0;
        int  max         = 0;
        int  cells       = 10;       // bar width in cells
        bool show_bar    = true;     // false → drop the bar (percent only)
        bool show_tokens = true;     // false → drop the raw "used/max" counts
                                     //         (compact: bar graph + percent)
    };

    explicit ContextGauge(Config c) : cfg_(c) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;
        const Color muted = Color::bright_black();

        if (cfg_.max <= 0) return blank().build();

        const bool has_tokens = cfg_.used > 0;
        const int  pct = has_tokens
            ? std::min(100, cfg_.used * 100 / cfg_.max)
            : 0;
        const Color zone = has_tokens ? threshold_color(pct) : muted;

        std::vector<Element> parts;
        parts.push_back(text("CTX ", Style{}.with_fg(muted).with_bold()));

        if (cfg_.show_bar) {
            // The raw "used/max" token counts are the most verbose part of
            // the gauge; show_tokens=false drops just those (keeping the bar
            // + percent) for a compact "CTX \u2588\u2588\u2588\u2588\u258c\u2591\u2591\u2591\u2591\u2591 46%" that fits a
            // narrow / phone-width status bar.
            if (has_tokens) {
                if (cfg_.show_tokens) {
                    std::string used_str = format_tokens(cfg_.used) + "/"
                                         + format_tokens(cfg_.max) + " ";
                    parts.push_back(text(used_str, fg_dim_(muted)));
                }
                parts.push_back(bar(pct, cfg_.cells));
            } else {
                // Placeholder numbers, same 13 cols as live: "  ——/  ——  "
                if (cfg_.show_tokens) {
                    parts.push_back(text(
                        "  \xe2\x80\x94\xe2\x80\x94/  \xe2\x80\x94\xe2\x80\x94  ",
                        fg_dim_(muted)));
                }
                parts.push_back(bar(0, cfg_.cells));   // dim track only
            }
        }

        if (has_tokens) {
            parts.push_back(text(" " + tabular_int_(pct, 3) + "%",
                                 Style{}.with_fg(zone).with_bold()));
        } else {
            // " ———%" — same 5 cells as live " 100%".
            parts.push_back(text(
                " \xe2\x80\x94\xe2\x80\x94\xe2\x80\x94%",
                Style{}.with_fg(zone).with_dim()));
        }

        return h(std::move(parts)).build();
    }

private:
    Config cfg_;

    // ── Smooth 1/8-gradation bar with per-cell color ──────────────────────
    //
    // Visual: `█████▆░░░░`  →  green green green green warn-amber dim dim dim dim
    [[nodiscard]] static Element bar(int pct, int cells) {
        static constexpr std::string_view kPartials[8] = {
            "", "\xe2\x96\x8f", "\xe2\x96\x8e", "\xe2\x96\x8d",
            "\xe2\x96\x8c", "\xe2\x96\x8b", "\xe2\x96\x8a", "\xe2\x96\x89",
        };
        const Color muted = Color::bright_black();

        pct = std::clamp(pct, 0, 100);
        int total_eighths = pct * cells * 8 / 100;

        std::string content;
        std::vector<StyledRun> runs;
        runs.reserve(static_cast<std::size_t>(cells));
        content.reserve(static_cast<std::size_t>(cells) * 3);

        for (int i = 0; i < cells; ++i) {
            int filled = std::max(0, total_eighths - i * 8);
            std::string_view ch;
            if      (filled >= 8) ch = "\xe2\x96\x88";  // █ full block
            else if (filled >  0) ch = kPartials[filled];
            else                  ch = "\xe2\x96\x91";  // ░ light shade

            // Threshold by cell position: [0,0.6) safe, [0.6,0.8) warn, rest danger.
            float cell_t = static_cast<float>(i + 1) / static_cast<float>(cells);
            Color cc;
            if      (filled == 0)    cc = muted;
            else if (cell_t <= 0.6f) cc = Color::green();
            else if (cell_t <= 0.8f) cc = Color::yellow();
            else                     cc = Color::red();

            std::size_t off = content.size();
            content.append(ch);
            Style st = (filled == 0) ? Style{}.with_fg(cc).with_dim()
                                     : Style{}.with_fg(cc);
            runs.push_back(StyledRun{off, ch.size(), st});
        }
        return Element{TextElement{
            .content = std::move(content),
            .style   = {},
            .runs    = std::move(runs),
        }};
    }

    static Color threshold_color(int pct) {
        if (pct < 60)  return Color::green();
        if (pct <= 80) return Color::yellow();
        return Color::red();
    }

    // 5-char tokens: "999.9" / "99.9k" / "9.9M". Space-padded on the left.
    static std::string format_tokens(int n) {
        char buf[16];
        if (n >= 1'000'000) {
            std::snprintf(buf, sizeof(buf), "%5.1fM", static_cast<double>(n) / 1'000'000.0);
        } else if (n >= 1000) {
            std::snprintf(buf, sizeof(buf), "%5.1fk", static_cast<double>(n) / 1000.0);
        } else {
            std::snprintf(buf, sizeof(buf), "%5d", n);
        }
        return buf;
    }

    static std::string tabular_int_(int n, int width) {
        std::string s = std::to_string(n);
        if (static_cast<int>(s.size()) >= width) return s;
        return std::string(static_cast<std::size_t>(width - static_cast<int>(s.size())), ' ')
             + s;
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
