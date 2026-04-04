#pragma once
// maya::components::Timeline — Vertical event timeline (CI/pipeline style)
//
//   Timeline({.events = {
//       {.label = "Read auth middleware", .detail = "Read 42 lines",
//        .duration = "1.2s", .status = TaskStatus::Completed},
//       {.label = "Edit auth.ts", .detail = "Replaced session middleware",
//        .duration = "2.0s", .status = TaskStatus::InProgress,
//        .bar_width = 10},
//       {.label = "Run tests", .detail = "Waiting...",
//        .status = TaskStatus::Pending},
//   }})

#include "core.hpp"

namespace maya::components {

struct TimelineEvent {
    std::string label;
    std::string detail    = "";      // secondary description
    std::string duration  = "";      // e.g. "1.2s", "340ms"
    TaskStatus  status    = TaskStatus::Pending;
    int         bar_width = 0;       // optional: duration bar width (0-20), 0 = no bar
};

struct TimelineProps {
    std::vector<TimelineEvent> events = {};
    bool show_connector = true;    // vertical line between events
    bool compact        = false;   // single line per event
    int  frame          = 0;       // animation frame for spinners
    int  track_width    = 40;      // dot-track width between label and duration
};

inline Element Timeline(TimelineProps props) {
    using namespace maya::dsl;

    auto& p = palette();
    std::vector<Element> rows;

    for (std::size_t i = 0; i < props.events.size(); ++i) {
        auto& ev = props.events[i];
        Color sc = status_color(ev.status);
        bool is_last = (i + 1 == props.events.size());

        // ── Icon ─────────────────────────────────────────────────────────
        Element icon_el = (ev.status == TaskStatus::InProgress)
            ? Spinner({.frame = props.frame, .color = sc})
            : text(status_icon(ev.status), Style{}.with_bold().with_fg(sc));

        // ── Connector dash for InProgress ────────────────────────────────
        std::string prefix = (ev.status == TaskStatus::InProgress) ? "\u2500" : " ";

        // ── Dot track between label and duration ─────────────────────────
        int label_len = static_cast<int>(ev.label.size());
        int dur_len   = static_cast<int>(ev.duration.size());
        int dots_len  = props.track_width - label_len - dur_len;
        if (dots_len < 2) dots_len = 2;

        std::string dots;
        dots.reserve(dots_len * 3);
        dots += ' ';
        for (int d = 1; d < dots_len - 1; ++d) dots += "\u00B7";
        dots += ' ';

        // ── Build main line ──────────────────────────────────────────────
        std::vector<Element> line;
        line.push_back(text("  ", Style{}));
        line.push_back(std::move(icon_el));
        line.push_back(text(prefix, Style{}.with_fg(sc)));
        line.push_back(text(ev.label, Style{}.with_fg(p.text)));

        if (!ev.duration.empty()) {
            line.push_back(text(dots, Style{}.with_dim().with_fg(p.muted)));
            line.push_back(text(ev.duration, Style{}.with_fg(p.muted)));
        }

        // ── Duration bar ─────────────────────────────────────────────────
        if (ev.bar_width > 0) {
            int bw = std::clamp(ev.bar_width, 1, 20);
            // For InProgress, show partial fill; for Completed, full fill
            int filled = (ev.status == TaskStatus::Completed) ? bw
                       : (ev.status == TaskStatus::InProgress)
                           ? static_cast<int>(bw * 0.7)
                           : 0;
            int empty_w = bw - filled;

            std::string filled_str;
            for (int b = 0; b < filled; ++b) filled_str += "\u2588";
            std::string empty_str;
            for (int b = 0; b < empty_w; ++b) empty_str += "\u2591";

            line.push_back(text("  ", Style{}));
            if (!filled_str.empty())
                line.push_back(text(filled_str, Style{}.with_fg(sc)));
            if (!empty_str.empty())
                line.push_back(text(empty_str, Style{}.with_fg(p.dim)));
        }

        rows.push_back(hstack()(std::move(line)));

        // ── Connector + detail line ──────────────────────────────────────
        if (props.show_connector && !is_last) {
            if (!props.compact && !ev.detail.empty()) {
                rows.push_back(
                    hstack()(
                        text("  \u2502   ", Style{}.with_dim().with_fg(p.dim)),
                        text(ev.detail, Style{}.with_fg(p.muted))
                    )
                );
            } else if (!props.compact && ev.detail.empty()) {
                rows.push_back(
                    text("  \u2502", Style{}.with_dim().with_fg(p.dim))
                );
            }
        } else if (!is_last && !props.compact && !ev.detail.empty()) {
            // No connector but still show detail indented
            rows.push_back(
                hstack()(
                    text("      ", Style{}),
                    text(ev.detail, Style{}.with_fg(p.muted))
                )
            );
        }

        // ── Connector-only line between events (after detail) ────────────
        if (props.show_connector && !is_last && !props.compact && !ev.detail.empty()) {
            // Extra connector line after detail, before next event
        }
    }

    return vstack()(std::move(rows));
}

} // namespace maya::components
