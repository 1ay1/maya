#pragma once
// maya::widget::context_window — AI context window usage visualization
//
// Segmented bar meter showing token usage across context segments.
// Sub-character precision using Unicode eighth-blocks.
//
//   ContextWindow ctx(200000);
//   ctx.add_segment("System",   12400, Color::blue());
//   ctx.add_segment("History",  89200, Color::magenta());
//   ctx.add_segment("Tools",    32100, Color::yellow());
//   ctx.add_segment("Response", 11534, Color::green());
//   auto ui = ctx.build();

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../dsl.hpp"
#include "../style/style.hpp"

namespace maya {

struct ContextSegment {
    std::string label;
    int         tokens = 0;
    Color       color  = Color::blue();
};

class ContextWindow {
    std::vector<ContextSegment> segments_;
    int  max_tokens_   = 200000;
    int  width_        = 40;
    bool show_labels_  = true;
    bool show_percent_ = true;

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

    // Build a filled bar string of N full blocks + optional fractional block
    static std::string make_bar(double frac_cells) {
        if (frac_cells <= 0.0) return {};
        int full = static_cast<int>(frac_cells);
        double rem = frac_cells - full;
        std::string s;
        for (int i = 0; i < full; ++i) s += "\xe2\x96\x88"; // █
        if (rem >= 0.0625) {
            static constexpr const char* eighths[] = {
                "\xe2\x96\x8f", "\xe2\x96\x8e", "\xe2\x96\x8d", "\xe2\x96\x8c",
                "\xe2\x96\x8b", "\xe2\x96\x8a", "\xe2\x96\x89", "\xe2\x96\x88",
            };
            int level = std::clamp(static_cast<int>(rem * 8.0), 0, 7);
            s += eighths[level];
        }
        return s;
    }

    static std::string make_empty(int cols) {
        std::string s;
        for (int i = 0; i < cols; ++i) s += "\xe2\x96\x91"; // ░
        return s;
    }

    static std::string make_dots(int count) {
        std::string s;
        for (int i = 0; i < count; ++i) s += "\xc2\xb7"; // ·
        return s;
    }

public:
    explicit ContextWindow(int max_tokens = 200000)
        : max_tokens_(std::max(1, max_tokens)) {}

    void set_max_tokens(int n) { max_tokens_ = std::max(1, n); }
    void set_width(int w) { width_ = std::max(4, w); }
    void set_show_labels(bool b) { show_labels_ = b; }
    void set_show_percent(bool b) { show_percent_ = b; }

    void add_segment(std::string label, int tokens, Color color = Color::blue()) {
        segments_.push_back({std::move(label), tokens, color});
    }

    void clear_segments() { segments_.clear(); }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        int w = std::max(4, width_);
        int max_tok = max_tokens_;

        int total_used = 0;
        for (auto& seg : segments_)
            total_used += seg.tokens;
        total_used = std::min(total_used, max_tok);

        int free_tokens = max_tok - total_used;
        double used_pct = static_cast<double>(total_used) / max_tok * 100.0;

        // Color coding: green <50%, yellow 50-80%, red >80%
        Color pct_color = used_pct > 80.0 ? Color::red()
                        : used_pct > 50.0 ? Color::yellow()
                        :                    Color::green();

        auto muted = Style{}.with_dim();
        auto dim   = Style{}.with_dim();

        std::vector<Element> rows;

        // ── Header: "72% used  145,234 / 200,000" ────────────────────
        {
            char pct_buf[16];
            std::snprintf(pct_buf, sizeof(pct_buf), "%.0f%%", used_pct);

            auto counts = "  " + format_with_commas(total_used)
                        + " / " + format_with_commas(max_tok);

            rows.push_back(h(
                text(std::string(pct_buf), Style{}.with_fg(pct_color).with_bold()),
                text(" used", muted),
                text(std::move(counts), Style{})
            ).build());
        }

        // ── Segmented bar ────────────────────────────────────────────
        {
            std::vector<Element> bar_parts;
            double chars_used = 0.0;

            for (auto& seg : segments_) {
                if (seg.tokens <= 0) continue;
                double frac = static_cast<double>(seg.tokens) / max_tok * w;
                if (frac < 0.125) frac = 0.125;
                frac = std::min(frac, static_cast<double>(w) - chars_used);

                auto s = make_bar(frac);
                if (!s.empty())
                    bar_parts.push_back(text(std::move(s), Style{}.with_fg(seg.color)));
                chars_used += std::ceil(frac);
            }

            int used_cols = static_cast<int>(chars_used);
            if (used_cols < w) {
                bar_parts.push_back(text(make_empty(w - used_cols), dim));
            }

            rows.push_back(h(std::move(bar_parts)).build());
        }

        // ── Per-segment legend ───────────────────────────────────────
        if (show_labels_ && !segments_.empty()) {
            for (auto& seg : segments_) {
                auto tok_str = format_with_commas(seg.tokens);
                double seg_pct = static_cast<double>(seg.tokens) / max_tok * 100.0;
                char pct_buf[16];
                std::snprintf(pct_buf, sizeof(pct_buf), "%5.1f%%", seg_pct);

                std::string suffix = tok_str + " " + pct_buf;
                int label_cols = 2 + static_cast<int>(seg.label.size());
                int suffix_cols = static_cast<int>(suffix.size());
                int dot_count = std::max(2, w - label_cols - suffix_cols - 1);

                rows.push_back(h(
                    text("\xe2\x96\x90 ", Style{}.with_fg(seg.color)),  // ▐
                    text(seg.label, Style{}),
                    text(" " + make_dots(dot_count) + " ", dim),
                    text(std::move(suffix), muted)
                ).build());
            }

            // Available capacity
            {
                auto free_str = format_with_commas(free_tokens);
                double free_pct = static_cast<double>(free_tokens) / max_tok * 100.0;
                char pct_buf[16];
                std::snprintf(pct_buf, sizeof(pct_buf), "%5.1f%%", free_pct);

                std::string suffix = free_str + " " + pct_buf;
                int label_cols = static_cast<int>(std::string_view("  Available").size());
                int suffix_cols = static_cast<int>(suffix.size());
                int dot_count = std::max(2, w - label_cols - suffix_cols - 1);

                auto green = Style{}.with_fg(Color::green());
                rows.push_back(h(
                    text("  Available", green),
                    text(" " + make_dots(dot_count) + " ", dim),
                    text(std::move(suffix), Style{}.with_fg(Color::green()).with_dim())
                ).build());
            }
        }

        return v(std::move(rows)).build();
    }
};

} // namespace maya
