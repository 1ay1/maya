#pragma once
// maya::components::TokenStream — Live token generation rate visualization
//
//   TokenStream({.total_tokens = 3421, .tokens_per_sec = 82.3f,
//                .peak_rate = 94.1f, .elapsed_secs = 12.4f,
//                .rate_history = history_vec})
//
//   TokenStream({.total_tokens = 500, .tokens_per_sec = 42.0f,
//                .rate_history = history_vec, .compact = true})

#include "core.hpp"

namespace maya::components {

struct TokenStreamProps {
    int                total_tokens  = 0;       // total generated so far
    float              tokens_per_sec = 0.f;    // current rate
    float              peak_rate     = 0.f;     // peak observed rate
    float              elapsed_secs  = 0.f;     // time since start
    std::vector<float> rate_history  = {};       // historical tok/s values for sparkline
    Color              color         = {};       // default: palette().primary
    bool               compact       = false;   // single-line mode
};

namespace detail {

inline std::string format_with_commas(int n) {
    if (n < 0) return "-" + format_with_commas(-n);
    auto s = std::to_string(n);
    int len = static_cast<int>(s.size());
    std::string result;
    result.reserve(len + (len - 1) / 3);
    for (int i = 0; i < len; ++i) {
        if (i > 0 && (len - i) % 3 == 0) result += ',';
        result += s[i];
    }
    return result;
}

inline Color rate_color(float rate) {
    if (rate > 50.f) return Color::rgb(80, 220, 120);   // green
    if (rate >= 20.f) return Color::rgb(240, 200, 60);   // yellow
    return Color::rgb(240, 80, 80);                       // red
}

inline Color resolve_color(Color c) {
    if (c.kind() == Color::Kind::Named && c.r() == 7 && c.g() == 0 && c.b() == 0)
        return palette().primary;
    return c;
}

} // namespace detail

inline Element TokenStream(TokenStreamProps props) {
    using namespace maya::dsl;
    auto& p = palette();

    Color base_color = detail::resolve_color(props.color);
    Color rc = detail::rate_color(props.tokens_per_sec);

    auto rate_str = fmt("%6.1f tok/s", static_cast<double>(props.tokens_per_sec));
    auto total_str = fmt("%6s tokens", detail::format_with_commas(props.total_tokens).c_str());

    // Build sparkline element
    auto spark = Sparkline({
        .data  = props.rate_history,
        .width = 16,
        .color = base_color,
    });

    // ── Compact mode ────────────────────────────────────────────────────
    if (props.compact) {
        return hstack().gap(1)(
            text("⚡", Style{}.with_fg(rc)),
            text(rate_str, Style{}.with_fg(rc)),
            std::move(spark),
            text(total_str, Style{}.with_fg(p.muted))
        );
    }

    // ── Full mode ───────────────────────────────────────────────────────
    Style label_style = Style{}.with_fg(p.muted);

    float avg = (props.elapsed_secs > 0.f)
        ? static_cast<float>(props.total_tokens) / props.elapsed_secs
        : 0.f;

    auto peak_str = fmt("%.1f tok/s", static_cast<double>(props.peak_rate));
    auto elapsed_str = fmt("%.1fs", static_cast<double>(props.elapsed_secs));
    auto avg_str = fmt("%.1f tok/s", static_cast<double>(avg));

    // Line 1: title
    auto title = text("Token Stream", Style{}.with_fg(base_color).with_bold(true));

    // Line 2: sparkline + current rate
    auto spark_line = hstack().gap(1)(
        text("  "),
        std::move(spark),
        text(rate_str, Style{}.with_fg(rc))
    );

    // Line 3: Total / Peak / Elapsed
    auto stats_line = hstack()(
        text("  Total: ", label_style),
        text(total_str, Style{}.with_fg(p.text)),
        text("  Peak: ", label_style),
        text(peak_str, Style{}.with_fg(p.text)),
        text("  Elapsed: ", label_style),
        text(elapsed_str, Style{}.with_fg(p.text))
    );

    // Line 4: Average
    auto avg_line = hstack()(
        text("  Avg: ", label_style),
        text(avg_str, Style{}.with_fg(p.text))
    );

    return vstack()(
        std::move(title),
        std::move(spark_line),
        std::move(stats_line),
        std::move(avg_line)
    );
}

} // namespace maya::components
