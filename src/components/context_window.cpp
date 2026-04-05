#include "maya/components/context_window.hpp"
#include "maya/components/token_stream.hpp"

namespace maya::components {

namespace detail {

bool is_default_color(Color c) {
    return c == Color{};
}

} // namespace detail

Element ContextWindow(ContextWindowProps props) {
    using namespace maya::dsl;
    auto& p = palette();

    int w = std::max(4, props.width);
    int max_tok = std::max(1, props.max_tokens);

    // Compute total used tokens
    int total_used = 0;
    for (auto& seg : props.segments)
        total_used += seg.tokens;
    total_used = std::min(total_used, max_tok);

    int free_tokens = max_tok - total_used;
    double used_pct = static_cast<double>(total_used) / max_tok * 100.0;
    double free_pct = static_cast<double>(free_tokens) / max_tok * 100.0;

    std::vector<Element> rows;

    // ── Header line: "Context: 145,234 / 200,000 tokens (72%)" ──────────
    {
        auto used_str = detail::format_with_commas(total_used);
        auto max_str  = detail::format_with_commas(max_tok);
        // Right-pad used to match max width for stable layout
        while (used_str.size() < max_str.size())
            used_str = " " + used_str;

        std::vector<Element> header;
        header.push_back(text(used_str,
                              Style{}.with_fg(p.text).with_bold(true)));
        header.push_back(text(" / " + max_str,
                              Style{}.with_fg(p.muted)));
        if (props.show_percent) {
            header.push_back(text(fmt(" (%3.0f%%)", used_pct),
                                  Style{}.with_fg(p.muted)));
        }
        rows.push_back(hstack()(std::move(header)));
    }

    // ── Segmented bar ───────────────────────────────────────────────────
    {
        std::vector<Element> bar_parts;

        int chars_used = 0;
        for (auto& seg : props.segments) {
            Color seg_color = detail::is_default_color(seg.color) ? p.primary : seg.color;
            int seg_chars = static_cast<int>(
                static_cast<double>(seg.tokens) / max_tok * w + 0.5);
            seg_chars = std::max(seg.tokens > 0 ? 1 : 0, seg_chars);
            // Don't exceed total bar width
            seg_chars = std::min(seg_chars, w - chars_used);

            if (seg_chars > 0) {
                std::string s;
                for (int i = 0; i < seg_chars; ++i) s += "█";
                bar_parts.push_back(text(std::move(s), Style{}.with_fg(seg_color)));
                chars_used += seg_chars;
            }
        }

        // Fill remainder with empty
        if (chars_used < w) {
            std::string s;
            for (int i = 0; i < w - chars_used; ++i) s += "░";
            bar_parts.push_back(text(std::move(s), Style{}.with_fg(p.dim)));
        }

        rows.push_back(hstack()(std::move(bar_parts)));
    }

    // ── Legend ───────────────────────────────────────────────────────────
    if (props.show_labels && !props.segments.empty()) {
        std::vector<Element> legend;

        for (auto& seg : props.segments) {
            Color seg_color = detail::is_default_color(seg.color) ? p.primary : seg.color;
            legend.push_back(text("■", Style{}.with_fg(seg_color)));
            legend.push_back(text(" " + seg.label, Style{}.with_fg(p.text)));
            legend.push_back(text(" " + detail::format_with_commas(seg.tokens),
                                  Style{}.with_fg(p.muted)));
            legend.push_back(text("   ", Style{}));
        }

        rows.push_back(hstack()(std::move(legend)));

        // Free tokens line, right-aligned feel
        {
            std::vector<Element> free_line;
            free_line.push_back(text("Free: ", Style{}.with_fg(p.muted)));
            free_line.push_back(text(detail::format_with_commas(free_tokens),
                                     Style{}.with_fg(p.text)));
            free_line.push_back(text(fmt(" (%.0f%%)", free_pct),
                                     Style{}.with_fg(p.muted)));
            rows.push_back(hstack()(std::move(free_line)));
        }
    }

    return vstack()(std::move(rows));
}

} // namespace maya::components
