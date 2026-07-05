#pragma once
// maya::widget::TokenStreamSparkline — compact tok/s rate + sparkline + total.
//
// Stable-width slot used by the status bar during streaming:
//
//   ⚡ 23.4 t/s ▁▂▃▄▅▆▇█▇▆ 1234
//
// 37 cells total: ⚡ ▕rate 5▏ t/s ▕spark 16▏ ▕total 5▏
//
// Every segment is fixed display width so the slot occupies the same
// cells whether rate is 0.5 or 1234, total is 0 or 12.3M — surrounding
// chips (model badge, CTX gauge) don't shove leftward as numbers tick.
//
// `live`: when false, sparkline + rate dim — signalling "frozen at
// last sample" during ExecutingTool / idle.
//
//   maya::TokenStreamSparkline{{
//       .rate    = 23.4f,
//       .total   = 1234,
//       .history = rate_history_vec,
//       .color   = Color::cyan(),
//       .live    = is_streaming,
//   }}.build();

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

class TokenStreamSparkline {
public:
    struct Config {
        float              rate    = 0.0f;
        int                total   = 0;
        std::vector<float> history;
        Color              color   = Color::cyan();
        bool               live    = false;     // false = dim (frozen)

        // Adaptive width. When true, build() returns a grow-tagged
        // component that sizes the sparkline to FILL its allocated
        // slot (the status bar hands it the dead space between the
        // phase chip and the model badge), clamped to
        // [8, max_spark_cells] and right-pinned so the newest sample
        // stays glued to the chips on its right. When false (default)
        // the legacy fixed 16-cell layout is byte-identical to before.
        bool               adaptive        = false;
        int                max_spark_cells = 64;
    };

    explicit TokenStreamSparkline(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        if (cfg_.adaptive) {
            // Adaptive: defer the spark-cell count to paint time, when
            // the layout engine has allocated this component its real
            // slot width. grow(1) is what makes the slot the free
            // space: the status bar places this between the left
            // group and the right chips instead of a blank spacer.
            return Element{component([cfg = cfg_](int w, int /*h*/) -> Element {
                using namespace dsl;
                // Fixed overhead: "⚡ "(2) + rate(5) + " t/s "(5) = 12,
                // plus 3 cols breathing room on the left so the chip
                // never butts against the phase chip / breadcrumb.
                constexpr int kOverhead = 12 + 3;
                const int cells = std::clamp(
                    w - kOverhead, 8, std::max(8, cfg.max_spark_cells));
                // Right-pin: when the slot is wider than the capped
                // chip, the leading spacer absorbs the slack so the
                // spark stays adjacent to the chips on its right.
                return h(spacer(),
                         render_chip_(cfg, cells)).build();
            // flex: 1 1 0 — basis 0 + grow 1 means this slot's size
            // is EXACTLY the row's leftover space after the left group
            // and right chips take their natural widths. Without the
            // zero basis, the component's auto-measure would report
            // its full constraint as natural width and force the
            // neighbouring chips to shrink/clip.
            }).grow(1.0f).basis(Dimension::fixed(0))};
        }

        return render_chip_(cfg_, 16);
    }

private:
    Config cfg_;

    [[nodiscard]] static Element render_chip_(const Config& cfg,
                                              int spark_cells) {
        using namespace dsl;
        const Color muted = Color::bright_black();

        static constexpr const char* kBlocks[8] = {
            "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
            "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88",
        };
        const int kSparkCells = std::max(1, spark_cells);

        float rate = std::max(0.0f, cfg.rate);
        Color rc = (rate > 50.0f)  ? Color::green()
                 : (rate >= 20.0f) ? Color::yellow()
                                   : Color::red();

        // Rate field — always exactly 5 display columns. The decimal
        // point stays visible across the entire streaming range so it
        // doesn't pop in/out as the rate crosses a format boundary.
        //
        // Two snprintf gotchas drive the threshold choices:
        //   1. %5.1f rounds: rate ∈ [999.95, 1000.0) prints as "1000.0"
        //      — 6 chars, overflows the 5-char slot and shifts the dot
        //      from col 3 to col 4. So we cap %5.1f at 999.5 (pre-rounding).
        //   2. %5.0f for the kilo range drops the dot. We avoid that by
        //      switching to %4.1fk (e.g. " 1.0k", " 9.9k") which keeps
        //      a dot through 9999.5 tok/s.
        // Above 9999.5 we accept losing the dot — rates that high are
        // off the chart for any sensible token-per-second display.
        char rate_buf[16];
        if      (rate <   999.5f) std::snprintf(rate_buf, sizeof(rate_buf), "%5.1f",  static_cast<double>(rate));
        else if (rate <  9999.5f) std::snprintf(rate_buf, sizeof(rate_buf), "%4.1fk", static_cast<double>(rate) / 1000.0);
        else if (rate < 99999.5f) std::snprintf(rate_buf, sizeof(rate_buf), "%4.0fk", static_cast<double>(rate) / 1000.0);
        else                      std::snprintf(rate_buf, sizeof(rate_buf), " 100k");

        // Sparkline — pad on LEFT with lowest block so right edge stays
        // pinned and new samples appear on the right.
        std::span<const float> hist{cfg.history.data(), cfg.history.size()};
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

        Style spark_style = cfg.live ? Style{}.with_fg(cfg.color)
                                     : Style{}.with_fg(cfg.color).with_dim();
        Style rate_style  = cfg.live ? Style{}.with_fg(rc).with_bold()
                                     : Style{}.with_fg(rc).with_dim();

        return h(
            text("\xe2\x9a\xa1 ", Style{}.with_fg(rc)),                  // ⚡
            text(std::string{rate_buf}, rate_style),
            text(" t/s ", fg_dim_(muted)),
            text(std::move(spark), spark_style)
        ).build();
    }

    // 5-char tokens: "999.9" / "99.9k" / "9.9M". Space-padded on the left.
    // Retained for callers / future revisions; not used by the
    // current build() (total field was dropped from the chip).
    [[maybe_unused]] static std::string format_tokens_(int n) {
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
