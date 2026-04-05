#include "maya/components/waterfall.hpp"

#include <algorithm>

namespace maya::components {

Element Waterfall(WaterfallProps props) {
    using namespace maya::dsl;

    if (props.entries.empty()) return text("");

    auto& p = palette();
    int w = std::max(4, props.bar_width);

    // Auto-scale: find max(start + duration)
    float scale = props.time_scale;
    if (scale <= 0.f) {
        for (auto& e : props.entries)
            scale = std::max(scale, e.start + e.duration);
        if (scale <= 0.f) scale = 1.f;
    }

    // Find max label width for alignment
    int max_label = 0;
    if (props.show_labels) {
        for (auto& e : props.entries)
            max_label = std::max(max_label, static_cast<int>(e.label.size()));
    }

    std::vector<Element> rows;
    rows.reserve(props.entries.size());

    for (auto& entry : props.entries) {
        std::vector<Element> line;

        // ---- Label column (right-padded) ------------------------------------
        if (props.show_labels) {
            std::string lbl = entry.label;
            if (static_cast<int>(lbl.size()) < max_label)
                lbl.append(max_label - lbl.size(), ' ');
            line.push_back(text(std::move(lbl), Style{}.with_fg(p.text)));
            line.push_back(text("  ", Style{}));
        }

        // ---- Determine bar color --------------------------------------------
        Color bar_color = (entry.color == Color{}) ? p.primary : entry.color;

        // ---- Compute bar positions ------------------------------------------
        float start_frac = std::clamp(entry.start / scale, 0.f, 1.f);
        float end_frac   = std::clamp((entry.start + entry.duration) / scale, 0.f, 1.f);

        int bar_start = static_cast<int>(start_frac * w);
        int bar_end   = static_cast<int>(end_frac * w);
        if (bar_end <= bar_start && entry.duration > 0.f) bar_end = bar_start + 1;
        bar_end = std::min(bar_end, w);

        int pre_len    = bar_start;
        int filled_len = bar_end - bar_start;
        int post_len   = w - bar_end;

        // ---- Build the bar --------------------------------------------------
        if (pre_len > 0) {
            std::string s;
            for (int i = 0; i < pre_len; ++i) s += "\u2591";
            line.push_back(text(std::move(s), Style{}.with_fg(p.dim)));
        }

        if (filled_len > 0) {
            // For InProgress, reserve last cell for spinner
            int solid = (entry.status == TaskStatus::InProgress && filled_len > 1)
                            ? filled_len - 1
                            : filled_len;
            std::string s;
            for (int i = 0; i < solid; ++i) s += "\u2588";
            line.push_back(text(std::move(s), Style{}.with_fg(bar_color)));

            if (entry.status == TaskStatus::InProgress) {
                line.push_back(
                    text(spin(props.frame), Style{}.with_bold().with_fg(bar_color)));
            }
        }

        if (post_len > 0) {
            std::string s;
            for (int i = 0; i < post_len; ++i) s += "\u2591";
            line.push_back(text(std::move(s), Style{}.with_fg(p.dim)));
        }

        // ---- Duration text --------------------------------------------------
        if (props.show_times) {
            double dur = static_cast<double>(entry.duration);
            std::string dur_str = (dur < 1.0)
                ? fmt("  %3.0fms", dur * 1000.0)
                : fmt("  %.1fs", dur);
            line.push_back(text(std::move(dur_str), Style{}.with_fg(p.muted)));
        }

        rows.push_back(hstack()(std::move(line)));
    }

    return vstack()(std::move(rows));
}

} // namespace maya::components
