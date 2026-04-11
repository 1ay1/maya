#pragma once
// maya::widget::api_usage — API rate limit and usage tracking display
//
// Shows current rate limits, request counts, and latency stats
// for the Claude API. Color-coded warnings when approaching limits.
//
//   APIUsage usage;
//   usage.set_requests(42, 60);      // used, limit
//   usage.set_tokens(150000, 200000);
//   usage.set_latency_ms(340);
//   auto ui = usage.build();

#include <cstdint>
#include <string>
#include <vector>

#include "../dsl.hpp"
#include "../style/style.hpp"

namespace maya {

class APIUsage {
    int req_used_     = 0;
    int req_limit_    = 0;
    int tok_used_     = 0;
    int tok_limit_    = 0;
    int latency_ms_   = 0;
    int error_count_  = 0;
    bool compact_     = false;

    static std::string fmt_tokens(int n) {
        if (n >= 1000000) {
            int whole = n / 1000000;
            int frac  = (n % 1000000) / 100000;
            return std::to_string(whole) + "." + std::to_string(frac) + "M";
        }
        if (n >= 1000) {
            int whole = n / 1000;
            int frac  = (n % 1000) / 100;
            return std::to_string(whole) + "." + std::to_string(frac) + "k";
        }
        return std::to_string(n);
    }

    // Color based on usage percentage: green→yellow→red
    static Color usage_color(int used, int limit) {
        if (limit <= 0) return Color::rgb(127, 132, 142);
        float pct = static_cast<float>(used) / static_cast<float>(limit);
        if (pct < 0.6f)  return Color::rgb(152, 195, 121); // green
        if (pct < 0.85f) return Color::rgb(229, 192, 123); // yellow
        return Color::rgb(224, 108, 117);                   // red
    }

    // Mini bar: [████░░░░] style
    static std::string mini_bar(int used, int limit, int width = 10) {
        if (limit <= 0) {
            std::string s;
            for (int i = 0; i < width; ++i) s += "\xe2\x96\x91"; // ░
            return s;
        }
        int filled = (used * width) / limit;
        if (filled > width) filled = width;
        std::string bar;
        for (int i = 0; i < filled; ++i)       bar += "\xe2\x96\x88"; // █
        for (int i = filled; i < width; ++i)    bar += "\xe2\x96\x91"; // ░
        return bar;
    }

public:
    APIUsage() = default;

    void set_requests(int used, int limit) { req_used_ = used; req_limit_ = limit; }
    void set_tokens(int used, int limit) { tok_used_ = used; tok_limit_ = limit; }
    void set_latency_ms(int ms) { latency_ms_ = ms; }
    void set_error_count(int n) { error_count_ = n; }
    void set_compact(bool b) { compact_ = b; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        auto lbl = Style{}.with_fg(Color::rgb(127, 132, 142));
        auto val = Style{}.with_fg(Color::rgb(200, 204, 212));
        auto dim = Style{}.with_fg(Color::rgb(62, 68, 81));

        if (compact_) {
            std::vector<Element> parts;

            if (req_limit_ > 0) {
                Color rc = usage_color(req_used_, req_limit_);
                parts.push_back(text("req ", lbl));
                parts.push_back(text(std::to_string(req_used_) + "/" + std::to_string(req_limit_),
                    Style{}.with_fg(rc)));
            }
            if (tok_limit_ > 0) {
                Color tc = usage_color(tok_used_, tok_limit_);
                if (!parts.empty()) parts.push_back(text("  ", dim));
                parts.push_back(text("tok ", lbl));
                parts.push_back(text(fmt_tokens(tok_used_) + "/" + fmt_tokens(tok_limit_),
                    Style{}.with_fg(tc)));
            }
            if (latency_ms_ > 0) {
                if (!parts.empty()) parts.push_back(text("  ", dim));
                Color lc = latency_ms_ < 500 ? Color::rgb(152, 195, 121)
                         : latency_ms_ < 2000 ? Color::rgb(229, 192, 123)
                         : Color::rgb(224, 108, 117);
                parts.push_back(text(std::to_string(latency_ms_) + "ms", Style{}.with_fg(lc)));
            }

            return h(std::move(parts)).build();
        }

        // ── Full mode ───────────────────────────────────────────────
        std::vector<Element> rows;

        // Requests
        if (req_limit_ > 0) {
            Color rc = usage_color(req_used_, req_limit_);
            rows.push_back(h(
                text("  Requests  ", lbl),
                text(mini_bar(req_used_, req_limit_), Style{}.with_fg(rc)),
                text("  " + std::to_string(req_used_) + "/" + std::to_string(req_limit_), val)
            ).build());
        }

        // Tokens
        if (tok_limit_ > 0) {
            Color tc = usage_color(tok_used_, tok_limit_);
            rows.push_back(h(
                text("  Tokens    ", lbl),
                text(mini_bar(tok_used_, tok_limit_), Style{}.with_fg(tc)),
                text("  " + fmt_tokens(tok_used_) + "/" + fmt_tokens(tok_limit_), val)
            ).build());
        }

        // Latency
        if (latency_ms_ > 0) {
            Color lc = latency_ms_ < 500 ? Color::rgb(152, 195, 121)
                     : latency_ms_ < 2000 ? Color::rgb(229, 192, 123)
                     : Color::rgb(224, 108, 117);
            rows.push_back(h(
                text("  Latency   ", lbl),
                text(std::to_string(latency_ms_) + "ms", Style{}.with_fg(lc))
            ).build());
        }

        // Errors
        if (error_count_ > 0) {
            rows.push_back(h(
                text("  Errors    ", lbl),
                text(std::to_string(error_count_),
                    Style{}.with_fg(Color::rgb(224, 108, 117)).with_bold())
            ).build());
        }

        if (rows.empty()) return text("  No API usage data", lbl);

        return v(std::move(rows)).build();
    }
};

} // namespace maya
