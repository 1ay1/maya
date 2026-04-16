#pragma once
// maya::widget::token_stream — Live token generation rate visualization
//
// Sparkline + rate display with stats. Supports compact (1-line) and
// full (4-line) modes.
//
//   TokenStream ts;
//   ts.set_total_tokens(3421);
//   ts.set_tokens_per_sec(82.3f);
//   ts.set_peak_rate(94.1f);
//   ts.set_elapsed(12.4f);
//   ts.set_rate_history({40, 55, 70, 82, 78, 90, 82});
//   auto ui = ts.build();

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../dsl.hpp"
#include "../style/style.hpp"

namespace maya {

class TokenStream {
    int   total_tokens_   = 0;
    float tokens_per_sec_ = 0.f;
    float peak_rate_      = 0.f;
    float elapsed_secs_   = 0.f;
    std::vector<float> rate_history_;
    Color color_   = Color::blue();
    bool  compact_ = false;

    static std::string format_with_commas(int n) {
        if (n < 0) return "-" + format_with_commas(-n);
        auto s = std::to_string(n);
        int len = static_cast<int>(s.size());
        std::string result;
        result.reserve(static_cast<size_t>(len + (len - 1) / 3));
        for (int i = 0; i < len; ++i) {
            if (i > 0 && (len - i) % 3 == 0) result += ',';
            result += s[static_cast<size_t>(i)];
        }
        return result;
    }

    static Color rate_color(float rate) {
        if (rate > 50.f) return Color::green();
        if (rate >= 20.f) return Color::yellow();
        return Color::red();
    }

    [[nodiscard]] std::string build_sparkline(size_t width) const {
        static constexpr const char* blocks[] = {
            "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
            "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88",
        };

        if (rate_history_.empty()) {
            std::string s;
            for (size_t i = 0; i < width; ++i) s += blocks[0];
            return s;
        }

        size_t start = rate_history_.size() > width ? rate_history_.size() - width : 0;
        float lo = *std::min_element(rate_history_.begin() + static_cast<long>(start),
                                     rate_history_.end());
        float hi = *std::max_element(rate_history_.begin() + static_cast<long>(start),
                                     rate_history_.end());
        float range = hi - lo;
        if (range < 0.001f) range = 1.f;

        std::string result;
        for (size_t i = start; i < rate_history_.size() && (i - start) < width; ++i) {
            float norm = std::clamp((rate_history_[i] - lo) / range, 0.f, 1.f);
            int level = std::clamp(static_cast<int>(norm * 7.f + 0.5f), 0, 7);
            result += blocks[level];
        }
        size_t used = rate_history_.size() - start;
        for (size_t i = used; i < width; ++i) result += blocks[0];
        return result;
    }

public:
    TokenStream() = default;

    void set_total_tokens(int n) { total_tokens_ = n; }
    void set_tokens_per_sec(float r) { tokens_per_sec_ = r; }
    void set_peak_rate(float r) { peak_rate_ = r; }
    void set_elapsed(float s) { elapsed_secs_ = s; }
    void set_rate_history(std::vector<float> h) { rate_history_ = std::move(h); }
    void set_color(Color c) { color_ = c; }
    void set_compact(bool b) { compact_ = b; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        auto muted = Style{}.with_dim();
        auto txt   = Style{};
        Color rc   = rate_color(tokens_per_sec_);

        char rate_buf[32];
        std::snprintf(rate_buf, sizeof(rate_buf), "%.1f", static_cast<double>(tokens_per_sec_));
        auto total_str = format_with_commas(total_tokens_);
        auto spark_str = build_sparkline(16);

        // ── Compact: single line ─────────────────────────────────────
        if (compact_) {
            return h(
                text("\xe2\x9a\xa1 ", Style{}.with_fg(rc)), // ⚡
                text(std::string(rate_buf), Style{}.with_fg(rc).with_bold()),
                text(" tok/s ", muted),
                text(spark_str, Style{}.with_fg(color_)),
                text(" " + total_str + " tokens", muted)
            ).build();
        }

        // ── Full: multi-line ─────────────────────────────────────────
        std::vector<Element> rows;

        // Title
        rows.push_back(text("Token Stream", Style{}.with_fg(color_).with_bold()));

        // Sparkline + rate
        rows.push_back(h(
            text("  "),
            text(spark_str, Style{}.with_fg(color_)),
            text("  "),
            text(std::string(rate_buf), Style{}.with_fg(rc).with_bold()),
            text(" tok/s", muted)
        ).build());

        // Stats: Total / Peak / Elapsed
        {
            char peak_buf[32], elapsed_buf[32];
            std::snprintf(peak_buf, sizeof(peak_buf), "%.1f",
                static_cast<double>(peak_rate_));
            std::snprintf(elapsed_buf, sizeof(elapsed_buf), "%.1fs",
                static_cast<double>(elapsed_secs_));

            rows.push_back(h(
                text("  Total ", muted),
                text(total_str + " tokens", txt),
                text("  Peak ", muted),
                text(std::string(peak_buf) + " tok/s", txt),
                text("  Elapsed ", muted),
                text(std::string(elapsed_buf), txt)
            ).build());
        }

        // Average
        {
            float avg = (elapsed_secs_ > 0.f)
                ? static_cast<float>(total_tokens_) / elapsed_secs_
                : 0.f;
            char avg_buf[32];
            std::snprintf(avg_buf, sizeof(avg_buf), "%.1f", static_cast<double>(avg));

            rows.push_back(h(
                text("  Avg ", muted),
                text(std::string(avg_buf) + " tok/s", txt)
            ).build());
        }

        return v(std::move(rows)).build();
    }
};

} // namespace maya
