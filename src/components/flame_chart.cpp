#include "maya/components/flame_chart.hpp"

namespace maya::components {

Element FlameChart(FlameChartProps props) {
    using namespace maya::dsl;

    if (props.spans.empty()) return text("");

    auto is_default_color = [](const Color& c) {
        return c.kind() == Color::Kind::Named && c.r() == 7 && c.g() == 0 && c.b() == 0;
    };

    // Warm palette for the classic flame look
    static const Color flame_colors[] = {
        Color::rgb(220, 50,  50),   // red
        Color::rgb(240, 100, 40),   // orange-red
        Color::rgb(240, 150, 30),   // orange
        Color::rgb(240, 190, 40),   // amber
        Color::rgb(240, 220, 60),   // yellow
        Color::rgb(250, 240, 100),  // light yellow
    };
    static constexpr int flame_n = 6;

    // Determine time scale
    float total_time = props.time_scale;
    if (total_time <= 0.f) {
        for (auto& s : props.spans) {
            float end = s.start + s.duration;
            if (end > total_time) total_time = end;
        }
    }
    if (total_time <= 0.f) total_time = 1.f;

    int w = std::max(4, props.width);

    // Find max depth present
    int max_d = 0;
    for (auto& s : props.spans) {
        if (s.depth > max_d) max_d = s.depth;
    }
    max_d = std::min(max_d, props.max_depth - 1);

    // Build rows: two lines per depth (bar line, label line)
    std::vector<Element> rows;

    for (int d = 0; d <= max_d; ++d) {
        // Collect spans at this depth
        std::vector<const FlameSpan*> layer;
        for (auto& s : props.spans) {
            if (s.depth == d) layer.push_back(&s);
        }
        if (layer.empty()) continue;

        // Bar line
        std::string bar_line(w, ' ');
        // Label line
        std::string lbl_line(w, ' ');

        struct BarRange {
            int col_start;
            int col_end;
            Color color;
            std::string label_text;
        };
        std::vector<BarRange> ranges;

        for (auto* sp : layer) {
            int col_start = static_cast<int>((sp->start / total_time) * w);
            int col_end   = static_cast<int>(((sp->start + sp->duration) / total_time) * w);
            col_start = std::clamp(col_start, 0, w - 1);
            col_end   = std::clamp(col_end, col_start + 1, w);

            int bar_w = col_end - col_start;

            Color c = is_default_color(sp->color)
                ? flame_colors[sp->depth % flame_n]
                : sp->color;

            // Fill bar characters
            if (bar_w >= 2) {
                bar_line[col_start] = '\x01';       // left edge marker
                bar_line[col_end - 1] = '\x02';     // right edge marker
                for (int i = col_start + 1; i < col_end - 1; ++i)
                    bar_line[i] = '\x03';            // fill marker
            } else {
                bar_line[col_start] = '\x03';
            }

            // Build label text
            std::string ltxt = sp->label;
            if (props.show_times) {
                ltxt += " " + fmt("%.1fs", static_cast<double>(sp->duration));
            }

            ranges.push_back({col_start, col_end, c, std::move(ltxt)});
        }

        // Build bar elements with proper coloring
        // We iterate character by character, grouping runs by color
        // First, build a color map
        std::vector<int> color_idx(w, -1);  // index into ranges
        for (int r = 0; r < static_cast<int>(ranges.size()); ++r) {
            for (int i = ranges[r].col_start; i < ranges[r].col_end; ++i)
                color_idx[i] = r;
        }

        std::vector<Element> bar_parts;
        int i = 0;
        while (i < w) {
            if (color_idx[i] < 0) {
                // Spaces — collect contiguous
                int j = i;
                while (j < w && color_idx[j] < 0) ++j;
                bar_parts.push_back(text(std::string(j - i, ' ')));
                i = j;
            } else {
                int r = color_idx[i];
                auto& rng = ranges[r];
                // Build the bar string for this range
                std::string s;
                int bar_w = rng.col_end - rng.col_start;
                if (bar_w >= 2) {
                    s += "\u2590";  // ▐ left edge
                    for (int k = 1; k < bar_w - 1; ++k)
                        s += "\u2588";  // █ fill
                    s += "\u258C";  // ▌ right edge
                } else {
                    s += "\u2588";  // █
                }
                bar_parts.push_back(text(std::move(s), Style{}.with_fg(rng.color)));
                i = rng.col_end;
            }
        }
        rows.push_back(hstack()(std::move(bar_parts)));

        // Label line: place labels centered under their bars
        // Build as colored segments
        std::vector<std::pair<int, int>> placed;  // col ranges used by labels

        struct LabelPlacement {
            int start;
            int end;
            int range_idx;
        };
        std::vector<LabelPlacement> placements;

        for (int r = 0; r < static_cast<int>(ranges.size()); ++r) {
            auto& rng = ranges[r];
            int bar_w = rng.col_end - rng.col_start;
            int ltxt_len = static_cast<int>(rng.label_text.size());

            if (ltxt_len == 0) continue;

            // Try to fit label centered under bar, with some overflow allowed
            int center = (rng.col_start + rng.col_end) / 2;
            int lbl_start = center - ltxt_len / 2;
            int lbl_end = lbl_start + ltxt_len;

            // Clamp to chart width
            if (lbl_start < 0) { lbl_start = 0; lbl_end = ltxt_len; }
            if (lbl_end > w) { lbl_end = w; lbl_start = w - ltxt_len; }
            if (lbl_start < 0) { lbl_start = 0; }

            // If bar is too narrow and label would be too truncated, skip
            int visible = lbl_end - lbl_start;
            if (visible < 3 && ltxt_len > 3) continue;

            // Check overlap with already placed labels
            bool overlaps = false;
            for (auto& p : placements) {
                if (lbl_start < p.end + 1 && lbl_end > p.start - 1) {
                    overlaps = true;
                    break;
                }
            }
            if (overlaps) continue;

            placements.push_back({lbl_start, lbl_end, r});
        }

        // Build label line from placements
        std::vector<Element> lbl_parts;
        int pos = 0;
        for (auto& p : placements) {
            if (p.start > pos) {
                lbl_parts.push_back(text(std::string(p.start - pos, ' ')));
            }
            int avail = p.end - p.start;
            std::string ltxt = ranges[p.range_idx].label_text;
            if (static_cast<int>(ltxt.size()) > avail) {
                ltxt = ltxt.substr(0, avail);
            }
            lbl_parts.push_back(text(std::move(ltxt),
                Style{}.with_fg(ranges[p.range_idx].color)));
            pos = p.end;
        }
        if (!lbl_parts.empty()) {
            rows.push_back(hstack()(std::move(lbl_parts)));
        }
    }

    return vstack()(std::move(rows));
}

} // namespace maya::components
