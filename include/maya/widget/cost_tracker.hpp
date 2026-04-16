#pragma once
// maya::widget::cost_tracker — Per-turn and cumulative token/cost display
//
// Detailed breakdown of API usage: input tokens, output tokens,
// cache reads/writes, cost per turn and running total.
//
//   CostTracker cost;
//   cost.add_turn({.input_tokens = 2340, .output_tokens = 1820,
//                  .cache_read = 12000, .cost_usd = 0.12f});
//   auto ui = cost.build();

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../dsl.hpp"
#include "../style/style.hpp"

namespace maya {

struct TurnUsage {
    int   input_tokens  = 0;
    int   output_tokens = 0;
    int   cache_read    = 0;
    int   cache_write   = 0;
    float cost_usd      = 0.f;
    float latency_ms    = 0.f;
};

class CostTracker {
    std::vector<TurnUsage> turns_;
    bool compact_ = false;

    static std::string fmt_tokens(int n) {
        if (n >= 1000000) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.1fM", static_cast<double>(n) / 1e6);
            return buf;
        }
        if (n >= 1000) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.1fk", static_cast<double>(n) / 1e3);
            return buf;
        }
        return std::to_string(n);
    }

    static std::string fmt_cost(float usd) {
        char buf[16];
        if (usd < 0.01f)
            std::snprintf(buf, sizeof(buf), "$%.4f", static_cast<double>(usd));
        else
            std::snprintf(buf, sizeof(buf), "$%.2f", static_cast<double>(usd));
        return buf;
    }

public:
    CostTracker() = default;

    void add_turn(TurnUsage t) { turns_.push_back(t); }
    void clear() { turns_.clear(); }
    void set_compact(bool b) { compact_ = b; }

    [[nodiscard]] int total_input() const {
        int t = 0; for (auto& u : turns_) t += u.input_tokens; return t;
    }
    [[nodiscard]] int total_output() const {
        int t = 0; for (auto& u : turns_) t += u.output_tokens; return t;
    }
    [[nodiscard]] float total_cost() const {
        float t = 0; for (auto& u : turns_) t += u.cost_usd; return t;
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        auto lbl   = Style{}.with_dim();
        auto val   = Style{};
        auto accent = Style{}.with_fg(Color::blue());
        auto dim   = Style{}.with_dim();
        auto green = Style{}.with_fg(Color::green());

        int ti = total_input(), to = total_output();
        int total_cache_r = 0, total_cache_w = 0;
        float total_lat = 0.f;
        for (auto& u : turns_) {
            total_cache_r += u.cache_read;
            total_cache_w += u.cache_write;
            total_lat += u.latency_ms;
        }

        // ── Compact: single line ─────────────────────────────────────
        if (compact_) {
            return h(
                text("\xe2\x86\x91", accent), text(fmt_tokens(ti) + " ", val), // ↑
                text("\xe2\x86\x93", green), text(fmt_tokens(to) + " ", val),  // ↓
                text(fmt_cost(total_cost()) + " ", Style{}.with_fg(Color::yellow())),
                text(std::to_string(turns_.size()) + " turns", lbl)
            ).build();
        }

        // ── Full: multi-line breakdown ───────────────────────────────
        std::vector<Element> rows;

        // Header
        rows.push_back(text("Usage", accent.with_bold()));

        // Token totals
        rows.push_back(h(
            text("  Input  ", lbl),
            text(fmt_tokens(ti), val),
            text("  Output  ", lbl),
            text(fmt_tokens(to), val),
            text("  Total  ", lbl),
            text(fmt_tokens(ti + to), val.with_bold())
        ).build());

        // Cache stats (if any)
        if (total_cache_r > 0 || total_cache_w > 0) {
            rows.push_back(h(
                text("  Cache read  ", lbl),
                text(fmt_tokens(total_cache_r), val),
                text("  Cache write  ", lbl),
                text(fmt_tokens(total_cache_w), val)
            ).build());
        }

        // Cost + turns
        rows.push_back(h(
            text("  Cost  ", lbl),
            text(fmt_cost(total_cost()), Style{}.with_fg(Color::yellow()).with_bold()),
            text("  Turns  ", lbl),
            text(std::to_string(turns_.size()), val),
            text("  Avg latency  ", lbl),
            text(turns_.empty() ? "---"
                : [&]{ char b[16]; std::snprintf(b, sizeof(b), "%.0fms",
                       static_cast<double>(total_lat / turns_.size())); return std::string(b); }(),
                val)
        ).build());

        // Per-turn breakdown (last 5)
        if (turns_.size() > 1) {
            rows.push_back(text(""));
            rows.push_back(text("  Recent turns", lbl));

            int start = std::max(0, static_cast<int>(turns_.size()) - 5);
            for (int i = start; i < static_cast<int>(turns_.size()); ++i) {
                auto& t = turns_[static_cast<size_t>(i)];
                char buf[64];
                std::snprintf(buf, sizeof(buf), "  #%d  ", i + 1);

                rows.push_back(h(
                    text(std::string(buf), dim),
                    text("\xe2\x86\x91", accent), text(fmt_tokens(t.input_tokens) + " ", val),
                    text("\xe2\x86\x93", green), text(fmt_tokens(t.output_tokens) + " ", val),
                    text(fmt_cost(t.cost_usd), Style{}.with_fg(Color::yellow()))
                ).build());
            }
        }

        return v(std::move(rows)).build();
    }
};

} // namespace maya
