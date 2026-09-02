#pragma once
// maya::widget::TokenStreamSparkline — compact tok/s rate + sparkline + total.
//
// Stable-width slot used by the status bar during streaming:
//
//   23.4 t/s ▁▂▃▄▅▆▇█▇▆ 1234
//
// 35 cells total: ▕rate 5▏ t/s ▕spark 16▏ ▕total 5▏
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
    // The chip's fixed (non-sparkline) parts, and the width they occupy.
    // Derived from the literals rather than restated as an integer: the
    // adaptive slot's cell budget subtracts this, so a hand-copied number
    // that drifted from the painted text would silently mis-size the spark.
    //
    // The chip opens on the rate token itself — no leading icon. The ⚡ that
    // used to sit here was decoration the label already carries: "t/s" says
    // what the number is, so the glyph spent two columns restating it in the
    // most width-contested row of the UI. It was also the status bar's only
    // East-Asian-wide (2-column) glyph, i.e. the one character a terminal is
    // most likely to disagree with us about the advance of.
    // The unit, and nothing else: no padding baked into the literal. The
    // single space before it is emitted with the unit so that "23.4 t/s"
    // stays one word at every magnitude; the field's slack lands AFTER it.
    static constexpr std::string_view kUnit = "t/s";
    // Width of the whole "<number> t/s" token, trailing pad included.
    // The number is at most "105.2" (5 cols; the kilo forms are shorter),
    // then one space, then the unit — and one more column so the token is
    // never flush against the spark that follows. Derived from the parts,
    // not hand-tallied, so it stays honest if any of them change.
    static constexpr int kRateNumCols = 5;                  // "105.2"
    static constexpr int kRateTokCols =
        kRateNumCols + 1 + unicode::str_width(kUnit) + 1;   // + trailing gap
    static constexpr int kFixedCols = kRateTokCols;
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
                // kFixedCols is derived from the painted literals (see
                // the class constants), plus 3 cols breathing room on the
                // left so the chip never butts against the phase chip /
                // breadcrumb.
                constexpr int kOverhead = kFixedCols + 3;
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
        // The width is LOAD-BEARING here, unlike the composer's ambient
        // counters (which were tabular-padded for no reason and got
        // un-padded). This value is spring-smoothed and updates EVERY FRAME
        // while streaming, and a sparkline is pinned immediately to its
        // right — so a variable-width rate would shove the spark, the · and
        // the CTX gauge left-right on every frame of every stream. Padding
        // to a fixed field is what keeps the row still.
        //
        // But WHERE the slack sits matters. It used to sit between the
        // number and the unit (the number left-aligned in a fixed field),
        // so "23.4  t/s" showed two spaces and "105.2 t/s" one. A quantity
        // and its unit read as a single word, and a gap that changes width
        // inside that word is the same flicker the fixed field exists to
        // prevent — just moved somewhere the eye rests on it. So the number
        // is formatted TIGHT, the unit is glued to it with exactly one
        // space, and the whole "<number> t/s" token is padded to
        // kRateTokCols. Total width is unchanged, the spark still never
        // moves, and ⚡ stays tight against its number.
        //
        // Two snprintf gotchas drive the threshold choices:
        //   1. %.1f rounds: rate ∈ [999.95, 1000.0) prints as "1000.0"
        //      — 6 chars, overflowing the 5-col number slot. So we cap it
        //      at 999.5 (pre-rounding).
        //   2. %.0f for the kilo range drops the dot. We avoid that by
        //      switching to %.1fk (e.g. "1.0k", "9.9k") which keeps
        //      a dot through 9999.5 tok/s.
        // Above 9999.5 we accept losing the dot — rates that high are
        // off the chart for any sensible token-per-second display.
        char rate_buf[16];
        if (rate < 999.5f) {
            std::snprintf(rate_buf, sizeof(rate_buf), "%.1f",
                          static_cast<double>(rate));
        } else {
            char num[16];
            if (rate < 9999.5f)
                std::snprintf(num, sizeof(num), "%.1fk",
                              static_cast<double>(rate) / 1000.0);
            else if (rate < 99999.5f)
                std::snprintf(num, sizeof(num), "%.0fk",
                              static_cast<double>(rate) / 1000.0);
            else
                std::snprintf(num, sizeof(num), "100k");
            std::snprintf(rate_buf, sizeof(rate_buf), "%s", num);
        }

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

        // The rate and its unit are ONE token: "23.4 t/s", a single space
        // between them at every magnitude. The field's slack is spent AFTER
        // the unit, where it separates the chip from the spark.
        //
        // It used to be spent between the number and the unit — the number
        // was left-aligned inside a fixed 6-column field, so a short rate
        // printed "23.4  t/s" (two spaces) while a long one printed
        // "105.2 t/s" (one). A quantity and its unit are read as a single
        // word, so a gap that changes width inside that word is a seam that
        // moves while the eye is resting on it — the flicker the fixed field
        // exists to prevent, relocated rather than removed.
        //
        // Padding the WHOLE "number + unit" token instead keeps the total
        // width identical (so the spark still never moves), keeps ⚡ tight
        // against its number, and keeps the unit tight against it too. The
        // pad is sized so it is never empty, so the chip cannot collide with
        // the spark that follows.
        //
        // ⚡ likewise carries no trailing space of its own: it is an
        // East-Asian-wide glyph (2 cols) and the rate that follows supplies
        // its own leading space via the field.
        // Number and unit are built as two styled runs (the number takes the
        // live/dim rate color, the unit is muted), so the pad rides on the
        // UNIT's tail rather than being sliced back out of a joined string.
        std::string num_tok  = std::string{rate_buf};
        std::string unit_tok = " " + std::string{kUnit};
        {
            const int w = unicode::str_width(num_tok)
                        + unicode::str_width(unit_tok);
            if (w < kRateTokCols)
                unit_tok.append(
                    static_cast<std::size_t>(kRateTokCols - w), ' ');
        }

        return h(
            text(std::move(num_tok), rate_style),
            text(std::move(unit_tok), fg_dim_(muted)),
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
