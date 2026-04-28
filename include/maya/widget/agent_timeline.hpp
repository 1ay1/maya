#pragma once
// maya::widget::agent_timeline — bordered Actions panel for agent tool turns.
//
// CI-pipeline-style log of tool events inside a single bordered card. Each
// event has a tree glyph, status icon, name + detail line + duration, an
// optional indented body under a `│` connector, and (when settled) a footer
// summary. The widget OWNS the chrome — borders, tree glyphs, status icons,
// duration formatting, body stripe, footer. Callers supply pure data:
// per-tool body element, category color, rich event status.
//
//   AgentTimeline tl;
//   tl.set_title(" ACTIONS  ·  3/5  ·  Bash ");
//   tl.set_border_color(Color::cyan());
//   tl.set_frame(spinner_frame);
//   tl.set_stats({{"INSPECT", 3, Color::blue()},
//                 {"MUTATE",  2, Color::magenta()}});
//   tl.add({.name="Bash", .detail="npm test  ·  exit 0",
//           .elapsed_seconds=1.2f, .category_color=Color::green(),
//           .status=AgentEventStatus::Done,
//           .body=preview_block_element});
//   tl.set_footer("\xe2\x9c\x93", "done", Color::green(), "3 actions   1.4s");
//   auto ui = tl.build();

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../style/border.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

// Lifecycle status for a single timeline event. Maps directly to the model's
// tool-call discriminator. "Approved" in the source domain folds into Running
// for visual purposes — both are "in flight."
enum class AgentEventStatus : std::uint8_t {
    Pending,    // queued, args still streaming → bright_yellow spinner
    Running,    // executing → bright_cyan spinner
    Done,       // success → bright_green ✓
    Failed,     // exception or non-zero exit → bright_red ✗
    Rejected,   // user denied permission → bright_yellow ⊘
};

struct AgentTimelineEvent {
    std::string         name;                 // Display name e.g. "Bash"
    std::string         detail;               // One-line summary
    float               elapsed_seconds = 0.0f;
    Color               category_color  = Color::blue();
    AgentEventStatus    status          = AgentEventStatus::Pending;
    Element             body{TextElement{}};  // empty by default; rendered under `│` stripe
};

struct AgentTimelineStat {
    std::string label;                        // raw — widget small-caps's it
    int         count = 0;
    Color       color = Color::blue();
};

class AgentTimeline {
public:
    AgentTimeline() = default;

    void set_title(std::string t)                          { title_ = std::move(t); }
    void set_border_color(Color c)                         { border_color_ = c; }
    void set_frame(int f)                                  { frame_ = f; }
    void set_stats(std::vector<AgentTimelineStat> s)       { stats_ = std::move(s); }
    void add(AgentTimelineEvent e)                         { events_.push_back(std::move(e)); }
    void clear()                                           { events_.clear(); }

    // Footer row shown at the bottom of the panel (typically when every event
    // has reached a terminal status). Pass empty strings to leave it off.
    void set_footer(std::string verb_glyph,
                    std::string verb_text,
                    Color       verb_color,
                    std::string total_summary) {
        footer_glyph_   = std::move(verb_glyph);
        footer_text_    = std::move(verb_text);
        footer_color_   = verb_color;
        footer_summary_ = std::move(total_summary);
        has_footer_     = !footer_glyph_.empty() || !footer_text_.empty();
    }

    operator Element() const { return build(); }

    // Declarative composition. The outer chrome is a single pipe chain;
    // dynamic content (stats, events, footer) flows through a flat
    // vector<Element> that v(...) flattens into top-level rows.
    [[nodiscard]] Element build() const {
        using namespace dsl;

        std::vector<Element> rows;
        rows.reserve(events_.size() * 4 + 4);

        const bool show_stats = events_.size() > 1 && !stats_.empty();
        if (show_stats) {
            rows.push_back(stats_row());
            rows.push_back(blank());
        }
        for (std::size_t i = 0; i < events_.size(); ++i) append_event(rows, i);
        if (has_footer_) {
            rows.push_back(blank());
            rows.push_back(footer_row());
        }

        return (v(rows)
                | border_<Round>
                | pad<0, 1, 0, 1>
                | bcolor(border_color_)
                | btext(title_, BorderTextPos::Top, BorderTextAlign::Start)
               ).build();
    }

private:
    std::vector<AgentTimelineEvent> events_;
    std::vector<AgentTimelineStat>  stats_;
    std::string title_;
    Color       border_color_ = Color::bright_black();
    int         frame_        = 0;
    bool        has_footer_   = false;
    std::string footer_glyph_;
    std::string footer_text_;
    std::string footer_summary_;
    Color       footer_color_ = Color::green();

    // ── Stats row: small-caps category badges separated by mid-dots.
    [[nodiscard]] Element stats_row() const {
        using namespace dsl;
        const Color muted = Color::bright_black();

        // Each badge is "LABEL N"; join with a mid-dot separator. Built as
        // a flat vector so map() can project then a separator can be
        // sprinkled between via h(); we hand-roll the sprinkle since map()
        // emits one-element-per-input.
        std::vector<Element> parts;
        parts.reserve(stats_.size() * 3);
        for (std::size_t i = 0; i < stats_.size(); ++i) {
            const auto& s = stats_[i];
            if (i > 0)
                parts.push_back(text("  \xc2\xb7  ", Style{}.with_fg(muted)));
            parts.push_back(text(small_caps(s.label),
                                 Style{}.with_fg(s.color).with_bold()));
            parts.push_back(text(" " + std::to_string(s.count),
                                 Style{}.with_fg(muted)));
        }
        return (h(parts) | grow(1.0f)).build();
    }

    // ── Footer row: `   ✓ DONE   3 actions   1.4s`.
    [[nodiscard]] Element footer_row() const {
        using namespace dsl;
        const Color muted = Color::bright_black();
        return h(
            text("   "),
            text(footer_glyph_ + " ", Style{}.with_fg(footer_color_).with_bold()),
            text(small_caps(footer_text_),
                 Style{}.with_fg(footer_color_).with_bold()),
            text("   "),
            text(footer_summary_, Style{}.with_fg(muted))
        ).build();
    }

    // ── Per-event block: header row + (optional) body rows + (optional)
    //    inter-event connector. Pushed into `rows` so v() picks them up
    //    in order.
    void append_event(std::vector<Element>& rows, std::size_t i) const {
        using namespace dsl;
        const auto& ev       = events_[i];
        const bool is_last   = (i + 1 == events_.size());
        const bool is_active = (ev.status == AgentEventStatus::Pending
                             || ev.status == AgentEventStatus::Running);
        const bool is_terminal = (ev.status == AgentEventStatus::Done
                               || ev.status == AgentEventStatus::Failed
                               || ev.status == AgentEventStatus::Rejected);

        // ── Header row.
        rows.push_back((h(
            text(tree_glyph(i, events_.size()), tree_style(ev.category_color, is_active)),
            text(" "),
            status_icon(ev.status, frame_),
            text("  "),
            text(ev.name, name_style(ev.status, ev.category_color, is_active)),
            text("  "),
            text(ev.detail, detail_style(is_active)),
            spacer(),
            when(is_terminal,
                 text(format_duration(ev.elapsed_seconds),
                      Style{}.with_fg(duration_color(ev.elapsed_seconds))))
        ) | grow(1.0f)).build());

        // ── Body rows under a `│` stripe. The body is whatever Element
        //    the caller supplied; we walk its children (or the single
        //    non-empty Text) so each line gets its own stripe — keeps
        //    each rendered row at exactly 1 line of measured height,
        //    which inline mode prefers.
        const Color cc        = event_connector_color(ev.status);
        const Style stripe_st = is_active ? Style{}.with_fg(cc)
                                          : Style{}.with_fg(cc).with_dim();
        Element body_rule = h(
            text("   "),
            text("\xe2\x94\x82  ", stripe_st)
        ).build();

        if (auto* bx = as_box(ev.body)) {
            for (const auto& child : bx->children)
                rows.push_back((h(body_rule, child) | grow(1.0f)).build());
        } else if (auto* t = as_text(ev.body); t && !t->content.empty()) {
            rows.push_back((h(body_rule, ev.body) | grow(1.0f)).build());
        }

        // ── Inter-event connector — a short `│` in the next event's
        //    color so the lane visually flows into the upcoming event.
        if (!is_last) {
            const Color next_cc = event_connector_color(events_[i + 1].status);
            rows.push_back(h(
                text("   "),
                text("\xe2\x94\x82", Style{}.with_fg(next_cc).with_dim())
            ).build());
        }
    }

    // ── Style helpers ──────────────────────────────────────────────────

    static Style tree_style(Color cat, bool is_active) {
        return is_active ? Style{}.with_fg(cat)
                         : Style{}.with_fg(cat).with_dim();
    }

    static Style name_style(AgentEventStatus s, Color cat, bool is_active) {
        if (s == AgentEventStatus::Failed)
            return Style{}.with_fg(Color::red()).with_bold();
        if (s == AgentEventStatus::Rejected)
            return Style{}.with_fg(Color::yellow()).with_bold();
        return is_active ? Style{}.with_fg(cat).with_bold()
                         : Style{}.with_fg(cat).with_dim();
    }

    static Style detail_style(bool is_active) {
        const Color muted = Color::bright_black();
        return is_active ? Style{}.with_fg(muted).with_italic()
                         : Style{}.with_fg(muted).with_dim().with_italic();
    }

    // ── Pure helpers (formatting / glyphs / colors) ────────────────────

    // Status icon. Same braille spinner as Spinner<SpinnerStyle::Dots> for
    // active events; static glyphs for terminal states.
    static dsl::RuntimeTextNode<std::string_view>
    status_icon(AgentEventStatus s, int frame) {
        static constexpr const char* spinner_frames[] = {
            "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
            "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7",
            "\xe2\xa0\x87", "\xe2\xa0\x8f",
        };
        switch (s) {
            case AgentEventStatus::Running:
                return dsl::text(std::string_view{spinner_frames[frame % 10]},
                                 Style{}.with_fg(Color::bright_cyan()).with_bold());
            case AgentEventStatus::Pending:
                return dsl::text(std::string_view{spinner_frames[frame % 10]},
                                 Style{}.with_fg(Color::bright_yellow()).with_bold());
            case AgentEventStatus::Done:
                return dsl::text(std::string_view{"\xe2\x9c\x93"},
                                 Style{}.with_fg(Color::bright_green()).with_bold());
            case AgentEventStatus::Failed:
                return dsl::text(std::string_view{"\xe2\x9c\x97"},
                                 Style{}.with_fg(Color::bright_red()).with_bold());
            case AgentEventStatus::Rejected:
                return dsl::text(std::string_view{"\xe2\x8a\x98"},
                                 Style{}.with_fg(Color::bright_yellow()).with_bold());
        }
        return dsl::text(std::string_view{"\xe2\x97\x8b"},
                         Style{}.with_fg(Color::bright_black()));
    }

    // Tree glyph: rounded + light box-drawing characters. ╭─ first, ├─ middle,
    // ╰─ last, ── singleton. Drawn in the per-event category color.
    static std::string_view tree_glyph(std::size_t idx, std::size_t total) {
        if (total == 1)        return "\xe2\x94\x80\xe2\x94\x80";  // ──
        if (idx == 0)          return "\xe2\x95\xad\xe2\x94\x80";  // ╭─
        if (idx + 1 == total)  return "\xe2\x95\xb0\xe2\x94\x80";  // ╰─
        return                        "\xe2\x94\x9c\xe2\x94\x80";  // ├─
    }

    // ms / s / m+s — short, glanceable, no surprising precision changes.
    static std::string format_duration(float secs) {
        char buf[24];
        if      (secs < 1.0f)
            std::snprintf(buf, sizeof(buf), "%.0fms", static_cast<double>(secs) * 1000.0);
        else if (secs < 60.0f)
            std::snprintf(buf, sizeof(buf), "%.1fs", static_cast<double>(secs));
        else {
            int mins   = static_cast<int>(secs) / 60;
            float rest = secs - static_cast<float>(mins * 60);
            std::snprintf(buf, sizeof(buf), "%dm%.0fs",
                          mins, static_cast<double>(rest));
        }
        return buf;
    }

    // Color-code wall-clock duration. Green = snappy (<250ms),
    // dim = normal (<2s), warn = slow (<15s), danger = stalling.
    static Color duration_color(float secs) {
        if (secs < 0.25f) return Color::green();
        if (secs < 2.0f)  return Color::bright_black();
        if (secs < 15.0f) return Color::yellow();
        return Color::red();
    }

    // Body-stripe and inter-event connector color, derived from event status.
    static Color event_connector_color(AgentEventStatus s) {
        switch (s) {
            case AgentEventStatus::Failed:   return Color::red();
            case AgentEventStatus::Rejected: return Color::yellow();
            case AgentEventStatus::Running:  return Color::blue();
            case AgentEventStatus::Pending:
            case AgentEventStatus::Done:     return Color::bright_black();
        }
        return Color::bright_black();
    }

    // Letter-spaced uppercase ("D O N E") for section labels.
    static std::string small_caps(std::string_view s) {
        std::string out;
        out.reserve(s.size() * 2);
        for (std::size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            out.push_back(static_cast<char>(
                (c >= 'a' && c <= 'z') ? (c - 32) : c));
            if (i + 1 < s.size()) out.push_back(' ');
        }
        return out;
    }
};

} // namespace maya
