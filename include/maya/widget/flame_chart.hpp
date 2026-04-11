#pragma once
// maya::widget::flame_chart — Flame graph for nested execution spans
//
// Renders spans as colored bars at depth levels with a time axis.
// Warm-to-cool coloring by depth. Labels centered under bars.
//
//   FlameChart flame(10.2f);
//   flame.add_span("total",      0.0f, 10.2f, 0);
//   flame.add_span("thinking",   0.0f,  4.2f, 1);
//   flame.add_span("responding", 6.4f,  3.8f, 1);
//   auto ui = flame.build();

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../dsl.hpp"
#include "../style/style.hpp"

namespace maya {

struct FlameSpan {
    std::string label;
    float       start    = 0.f;
    float       duration = 0.f;
    int         depth    = 0;
    Color       color    = {};
};

class FlameChart {
    std::vector<FlameSpan> spans_;
    int   width_      = 60;
    float time_scale_ = 0.f;
    bool  show_times_ = true;
    int   max_depth_  = 8;

    // Warm → cool by depth
    static constexpr Color depth_colors[] = {
        Color::rgb(224, 108, 117),  // red
        Color::rgb(229, 160,  90),  // orange
        Color::rgb(229, 192, 123),  // amber
        Color::rgb(152, 195, 121),  // green
        Color::rgb( 97, 175, 239),  // blue
        Color::rgb(198, 120, 221),  // purple
    };
    static constexpr int depth_n = 6;

    static bool is_default_color(Color c) { return c == Color{}; }

public:
    FlameChart() = default;
    explicit FlameChart(float time_scale) : time_scale_(time_scale) {}

    void set_width(int w) { width_ = std::max(4, w); }
    void set_time_scale(float s) { time_scale_ = s; }
    void set_show_times(bool b) { show_times_ = b; }
    void set_max_depth(int d) { max_depth_ = d; }

    void add_span(std::string label, float start, float duration,
                  int depth = 0, Color color = {}) {
        spans_.push_back({std::move(label), start, duration, depth, color});
    }

    void clear() { spans_.clear(); }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        if (spans_.empty()) return text("");

        float total_time = time_scale_;
        if (total_time <= 0.f) {
            for (auto& s : spans_)
                total_time = std::max(total_time, s.start + s.duration);
        }
        if (total_time <= 0.f) total_time = 1.f;

        int w = std::max(4, width_);
        int max_d = 0;
        for (auto& s : spans_)
            if (s.depth > max_d) max_d = s.depth;
        max_d = std::min(max_d, max_depth_ - 1);

        auto axis_style = Style{}.with_fg(Color::rgb(92, 99, 112));
        auto tick_style = Style{}.with_fg(Color::rgb(62, 68, 81));

        std::vector<Element> rows;

        // ── Time axis header ─────────────────────────────────────────
        {
            std::string line(static_cast<size_t>(w), ' ');
            for (int q = 0; q <= 4; ++q) {
                float t = total_time * q / 4.0f;
                char buf[16];
                if (t < 1.0f)
                    std::snprintf(buf, sizeof(buf), "%dms", static_cast<int>(t * 1000));
                else
                    std::snprintf(buf, sizeof(buf), "%.1fs", static_cast<double>(t));
                int pos = static_cast<int>(static_cast<float>(w) * q / 4.0f);
                std::string lbl = buf;
                int start = (q == 0) ? pos
                          : (q == 4) ? std::max(0, pos - static_cast<int>(lbl.size()))
                          : std::max(0, pos - static_cast<int>(lbl.size()) / 2);
                start = std::min(start, w - static_cast<int>(lbl.size()));
                if (start < 0) start = 0;
                for (int j = 0; j < static_cast<int>(lbl.size()) && start + j < w; ++j)
                    line[static_cast<size_t>(start + j)] = lbl[static_cast<size_t>(j)];
            }
            rows.push_back(text(std::move(line), axis_style));
        }

        // ── Depth layers ─────────────────────────────────────────────
        for (int d = 0; d <= max_d; ++d) {
            struct Range {
                int col_start, col_end;
                Color color;
                std::string label_text;
            };
            std::vector<Range> ranges;

            for (auto& sp : spans_) {
                if (sp.depth != d) continue;
                int cs = std::clamp(static_cast<int>((sp.start / total_time) * w), 0, w - 1);
                int ce = std::clamp(static_cast<int>(((sp.start + sp.duration) / total_time) * w),
                                    cs + 1, w);
                Color c = is_default_color(sp.color) ? depth_colors[d % depth_n] : sp.color;

                std::string ltxt = sp.label;
                if (show_times_) {
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), " %.1fs", static_cast<double>(sp.duration));
                    ltxt += buf;
                }
                ranges.push_back({cs, ce, c, std::move(ltxt)});
            }
            if (ranges.empty()) continue;

            // Map columns to ranges
            std::vector<int> col_map(static_cast<size_t>(w), -1);
            for (int r = 0; r < static_cast<int>(ranges.size()); ++r)
                for (int i = ranges[static_cast<size_t>(r)].col_start;
                     i < ranges[static_cast<size_t>(r)].col_end; ++i)
                    col_map[static_cast<size_t>(i)] = r;

            // ── Bar line ─────────────────────────────────────────────
            {
                std::vector<Element> bar;
                int i = 0;
                while (i < w) {
                    if (col_map[static_cast<size_t>(i)] < 0) {
                        int j = i;
                        while (j < w && col_map[static_cast<size_t>(j)] < 0) ++j;
                        bar.push_back(text(std::string(static_cast<size_t>(j - i), ' ')));
                        i = j;
                    } else {
                        int r = col_map[static_cast<size_t>(i)];
                        auto& rng = ranges[static_cast<size_t>(r)];
                        int bw = rng.col_end - rng.col_start;

                        std::string s;
                        if (bw >= 3) {
                            s += "\xe2\x96\x90"; // ▐
                            for (int k = 1; k < bw - 1; ++k) s += "\xe2\x96\x88"; // █
                            s += "\xe2\x96\x8c"; // ▌
                        } else if (bw == 2) {
                            s += "\xe2\x96\x90\xe2\x96\x8c"; // ▐▌
                        } else {
                            s += "\xe2\x96\x88"; // █
                        }
                        bar.push_back(text(std::move(s), Style{}.with_fg(rng.color)));
                        i = rng.col_end;
                    }
                }
                rows.push_back(h(std::move(bar)).build());
            }

            // ── Label line ───────────────────────────────────────────
            struct Placement { int start, end, range_idx; };
            std::vector<Placement> placements;

            for (int r = 0; r < static_cast<int>(ranges.size()); ++r) {
                auto& rng = ranges[static_cast<size_t>(r)];
                int bw = rng.col_end - rng.col_start;
                int llen = static_cast<int>(rng.label_text.size());
                if (llen == 0 || (bw < 3 && llen > bw)) continue;

                int center = (rng.col_start + rng.col_end) / 2;
                int ls = std::max(0, center - llen / 2);
                int le = ls + llen;
                if (le > w) { le = w; ls = std::max(0, w - llen); }

                bool overlaps = false;
                for (auto& p : placements)
                    if (ls < p.end + 1 && le > p.start - 1) { overlaps = true; break; }
                if (!overlaps)
                    placements.push_back({ls, le, r});
            }

            if (!placements.empty()) {
                std::vector<Element> lbl_parts;
                int pos = 0;
                for (auto& p : placements) {
                    if (p.start > pos) {
                        lbl_parts.push_back(
                            text(std::string(static_cast<size_t>(p.start - pos), ' ')));
                        pos = p.start;
                    }
                    int avail = p.end - p.start;
                    auto ltxt = ranges[static_cast<size_t>(p.range_idx)].label_text;
                    if (static_cast<int>(ltxt.size()) > avail)
                        ltxt = ltxt.substr(0, static_cast<size_t>(avail));
                    lbl_parts.push_back(text(std::move(ltxt),
                        Style{}.with_fg(ranges[static_cast<size_t>(p.range_idx)].color).with_dim()));
                    pos = p.end;
                }
                rows.push_back(h(std::move(lbl_parts)).build());
            }
        }

        // ── Bottom tick ruler ────────────────────────────────────────
        {
            std::string line;
            for (int i = 0; i < w; ++i) {
                bool is_tick = false;
                for (int q = 0; q <= 4; ++q)
                    if (i == static_cast<int>(static_cast<float>(w) * q / 4.0f))
                        is_tick = true;
                line += is_tick ? "\xe2\x94\xbc" : "\xe2\x94\x80"; // ┼ or ─
            }
            rows.push_back(text(std::move(line), tick_style));
        }

        return v(std::move(rows)).build();
    }
};

} // namespace maya
