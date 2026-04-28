#pragma once
// maya::widget::agent_timeline — bordered Actions panel for agent tool turns.
//
// Drop-in for the moha "assistant_timeline" rendering. One bordered card whose
// body is a CI-pipeline-style log of tool events: each event has a tree glyph,
// status icon, name + detail line + duration, optional indented body under a
// `│` connector, and (when settled) a footer summary.
//
//   AgentTimeline tl;
//   tl.set_title(" ACTIONS  ·  3/5  ·  Bash ");
//   tl.set_border_color(Color::cyan());
//   tl.set_frame(spinner_frame);
//   tl.set_stats({{"INSPECT", 3, Color::blue()}, {"MUTATE", 2, Color::magenta()}});
//   tl.add({.name="Bash", .detail="npm test  ·  exit 0",
//           .elapsed_seconds=1.2f, .category_color=Color::green(),
//           .status=AgentEventStatus::Done,
//           .body=preview_block_element});
//   tl.set_footer("\xe2\x9c\x93", "done", Color::green(), "3 actions   1.4s");
//   auto ui = tl.build();
//
// The widget OWNS the chrome (border, title, stripe, tree glyphs, status icons,
// duration formatting, footer) so callers don't reimplement them. Callers
// supply the per-tool body element (rendered under the `│` stripe), the
// category color (domain-specific), and the rich event status — everything
// else is computed inside.

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
// tool-call discriminator (Pending/Running/Done/Failed/Rejected). "Approved"
// in the source domain folds into Running for visual purposes — both are
// "in flight."
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

    void set_title(std::string t)              { title_ = std::move(t); }
    void set_border_color(Color c)             { border_color_ = c; }
    void set_frame(int f)                      { frame_ = f; }
    void set_stats(std::vector<AgentTimelineStat> s) { stats_ = std::move(s); }

    void add(AgentTimelineEvent e)             { events_.push_back(std::move(e)); }
    void clear()                               { events_.clear(); }

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

    [[nodiscard]] Element build() const {
        using namespace dsl;

        const Color muted = Color::bright_black();
        std::vector<Element> rows;

        // ── Stats header — small-caps category badges, only when >1 event.
        if (events_.size() > 1 && !stats_.empty()) {
            std::vector<Element> parts;
            bool first = true;
            for (const auto& s : stats_) {
                if (!first) parts.push_back(text("  \xc2\xb7  ", Style{}.with_fg(muted)));
                first = false;
                parts.push_back(text(small_caps(s.label),
                                     Style{}.with_fg(s.color).with_bold()));
                parts.push_back(text(" " + std::to_string(s.count),
                                     Style{}.with_fg(muted)));
            }
            rows.push_back((h(std::move(parts)) | grow(1.0f)).build());
            rows.push_back(text(""));
        }

        // ── Per-event rows.
        for (std::size_t i = 0; i < events_.size(); ++i) {
            const auto& ev = events_[i];
            const bool is_last  = (i + 1 == events_.size());
            const bool is_active = (ev.status == AgentEventStatus::Pending
                                 || ev.status == AgentEventStatus::Running);
            const bool is_terminal = (ev.status == AgentEventStatus::Done
                                   || ev.status == AgentEventStatus::Failed
                                   || ev.status == AgentEventStatus::Rejected);

            // Tree glyph in category color.
            Style tree_style = is_active
                ? Style{}.with_fg(ev.category_color)
                : Style{}.with_fg(ev.category_color).with_dim();

            // Name color: failed/rejected stay danger/warn; others use category
            // color, bold for active and dim for settled.
            Style name_style;
            if      (ev.status == AgentEventStatus::Failed)
                name_style = Style{}.with_fg(Color::red()).with_bold();
            else if (ev.status == AgentEventStatus::Rejected)
                name_style = Style{}.with_fg(Color::yellow()).with_bold();
            else if (is_active)
                name_style = Style{}.with_fg(ev.category_color).with_bold();
            else
                name_style = Style{}.with_fg(ev.category_color).with_dim();

            // Italic for the detail column — visually separates "data the
            // tool is acting on" from chrome.
            Style detail_style = is_active
                ? Style{}.with_fg(muted).with_italic()
                : Style{}.with_fg(muted).with_dim().with_italic();

            // Build the left-side run as one h(). Spacer + elapsed at the
            // outer level so spacer-grow propagates correctly.
            std::vector<Element> left_parts;
            left_parts.push_back(text(tree_glyph(i, events_.size()), tree_style));
            left_parts.push_back(text(" ", {}));
            left_parts.push_back(status_icon(ev.status, frame_));
            left_parts.push_back(text("  ", {}));
            left_parts.push_back(text(ev.name, name_style));
            left_parts.push_back(text("  ", {}));
            left_parts.push_back(text(ev.detail, detail_style));
            Element left = h(std::move(left_parts)).build();

            if (is_terminal) {
                Element elapsed_el = text(format_duration(ev.elapsed_seconds),
                    Style{}.with_fg(duration_color(ev.elapsed_seconds)));
                rows.push_back((h(left, spacer(), elapsed_el) | grow(1.0f)).build());
            } else {
                rows.push_back((h(left, spacer()) | grow(1.0f)).build());
            }

            // ── Body content under a light `│` stripe.
            // Body row count is variable per tool. We flatten box children
            // into individual rows so each one gets its own stripe — keeps
            // each rendered row at exactly 1 measured line of height, which
            // is what maya's inline overwrite mode prefers.
            Color cc = event_connector_color(ev.status);
            Style stripe_style = is_active
                ? Style{}.with_fg(cc)
                : Style{}.with_fg(cc).with_dim();
            Element body_rule = h(
                text("   ", {}),                                  // tree+space alignment (3 cols)
                text("\xe2\x94\x82  ", stripe_style)              // │ (light)
            ).build();

            // Const access through downcast — body is an immutable view into ev.
            if (auto* bx = as_box(ev.body)) {
                for (const auto& child : bx->children)
                    rows.push_back((h(body_rule, child) | grow(1.0f)).build());
            } else if (auto* t = as_text(ev.body)) {
                if (!t->content.empty())
                    rows.push_back((h(body_rule, ev.body) | grow(1.0f)).build());
            }

            // ── Inter-event connector — color from the NEXT event's status
            // so the lane visually flows into the upcoming event.
            if (!is_last) {
                Color next_cc = event_connector_color(events_[i + 1].status);
                rows.push_back(h(
                    text("   ", {}),
                    text("\xe2\x94\x82", Style{}.with_fg(next_cc).with_dim())  // │
                ).build());
            }
        }

        // ── Footer summary.
        if (has_footer_) {
            rows.push_back(text(""));
            rows.push_back(h(
                text("   ", {}),
                text(footer_glyph_ + " ", Style{}.with_fg(footer_color_).with_bold()),
                text(small_caps(footer_text_), Style{}.with_fg(footer_color_).with_bold()),
                text("   ", {}),
                text(footer_summary_, Style{}.with_fg(muted))
            ).build());
        }

        // ── Outer chrome.
        return (v(std::move(rows))
                | border(BorderStyle::Round)
                | bcolor(border_color_)
                | btext(title_, BorderTextPos::Top, BorderTextAlign::Start)
                | padding(0, 1, 0, 1)
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

    // ── Internal helpers ────────────────────────────────────────────────

    // Status icon. Same braille spinner as Spinner<SpinnerStyle::Dots> for
    // active events; static glyphs for terminal states. Bright variants
    // chosen so the icon reads as "alive" / "settled" on every theme.
    static Element status_icon(AgentEventStatus s, int frame) {
        static constexpr const char* spinner_frames[] = {
            "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
            "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7",
            "\xe2\xa0\x87", "\xe2\xa0\x8f",
        };
        switch (s) {
            case AgentEventStatus::Running:
                return dsl::text(spinner_frames[frame % 10],
                                 Style{}.with_fg(Color::bright_cyan()).with_bold());
            case AgentEventStatus::Pending:
                return dsl::text(spinner_frames[frame % 10],
                                 Style{}.with_fg(Color::bright_yellow()).with_bold());
            case AgentEventStatus::Done:
                return dsl::text("\xe2\x9c\x93",
                                 Style{}.with_fg(Color::bright_green()).with_bold());   // ✓
            case AgentEventStatus::Failed:
                return dsl::text("\xe2\x9c\x97",
                                 Style{}.with_fg(Color::bright_red()).with_bold());     // ✗
            case AgentEventStatus::Rejected:
                return dsl::text("\xe2\x8a\x98",
                                 Style{}.with_fg(Color::bright_yellow()).with_bold()); // ⊘
        }
        return dsl::text("\xe2\x97\x8b", Style{}.with_fg(Color::bright_black()));      // ○ (unreachable)
    }

    // Tree glyph: rounded + light box-drawing characters. ╭─ first, ├─ middle,
    // ╰─ last, ── singleton. Drawn in the per-event category color so the
    // leading edge of each row reads as a colored timeline at a glance.
    static std::string tree_glyph(std::size_t idx, std::size_t total) {
        if (total == 1)              return "\xe2\x94\x80\xe2\x94\x80";  // ──
        if (idx == 0)                return "\xe2\x95\xad\xe2\x94\x80";  // ╭─
        if (idx + 1 == total)        return "\xe2\x95\xb0\xe2\x94\x80";  // ╰─
        return                              "\xe2\x94\x9c\xe2\x94\x80";  // ├─
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

    // Color-code a tool's wall-clock duration so the eye finds slow steps
    // without parsing numbers. Green = snappy (<250ms), dim = normal (<2s),
    // warn = slow (<15s), danger = stalling.
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
            case AgentEventStatus::Pending:  return Color::bright_black();
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
