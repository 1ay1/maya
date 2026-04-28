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
    };

    explicit TokenStreamSparkline(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;
        const Color muted = Color::bright_black();

        static constexpr const char* kBlocks[8] = {
            "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
            "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88",
        };
        constexpr int kSparkCells = 16;

        float rate = std::max(0.0f, cfg_.rate);
        Color rc = (rate > 50.0f)  ? Color::green()
                 : (rate >= 20.0f) ? Color::yellow()
                                   : Color::red();

        // Rate field — always 5 display columns.
        char rate_buf[16];
        if      (rate <    100.0f) std::snprintf(rate_buf, sizeof(rate_buf), "%5.1f",  static_cast<double>(rate));
        else if (rate <  10000.0f) std::snprintf(rate_buf, sizeof(rate_buf), "%5.0f",  static_cast<double>(rate));
        else                       std::snprintf(rate_buf, sizeof(rate_buf), "%4.0fk", static_cast<double>(rate) / 1000.0);

        // Sparkline — pad on LEFT with lowest block so right edge stays
        // pinned and new samples appear on the right.
        std::span<const float> hist{cfg_.history.data(), cfg_.history.size()};
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

        Style spark_style = cfg_.live ? Style{}.with_fg(cfg_.color)
                                      : Style{}.with_fg(cfg_.color).with_dim();
        Style rate_style  = cfg_.live ? Style{}.with_fg(rc).with_bold()
                                      : Style{}.with_fg(rc).with_dim();

        return h(
            text("\xe2\x9a\xa1 ", Style{}.with_fg(rc)),                  // ⚡
            text(std::string{rate_buf}, rate_style),
            text(" t/s ", fg_dim_(muted)),
            text(std::move(spark), spark_style),
            text(" "),
            text(format_tokens_(cfg_.total), fg_dim_(muted))
        ).build();
    }

private:
    Config cfg_;

    // 5-char tokens: "999.9" / "99.9k" / "9.9M". Space-padded on the left.
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
