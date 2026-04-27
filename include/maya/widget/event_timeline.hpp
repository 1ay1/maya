#pragma once
// maya::widget::event_timeline — Vertical event feed with arbitrary-widget bodies
//
// A "Linear / GitHub PR" style activity timeline: each event has a marker
// glyph on a vertical rail, a one-line title, an optional right-aligned
// elapsed-time tag, and an arbitrary widget body (BashTool, Investigation,
// ReadTool — anything).
//
// Distinct from the existing maya::Timeline (widget/timeline.hpp), which is
// a compact CI-style task list with string-only events. This widget is for
// rich activity feeds where each entry hosts a full widget tree.
//
//   ●  User connected
//   │
//   ●  Read auth middleware                                            120ms
//   │  ╭─ ✓ Read ────────────────────────────────────────────────────────╮
//   │  │ src/auth/middleware.ts                                          │
//   │  ╰─────────────────────────────────────────────────────────────────╯
//   │
//   ⠋  Investigating routes                                              2.1s
//   │  ╭─ ● Investigate ─────────────────────────────────────────────────╮
//   │  │ Where are routes defined?                                  ⠋    │
//   │  │ ├─ ✓ ls src/routes/                                             │
//   │  │ └─ ● Read src/routes/api.ts                                     │
//   │  ╰─────────────────────────────────────────────────────────────────╯
//   │
//   ○  Will run tests after read
//
// API:
//
//   EventTimeline tl;
//   tl.add({.title = "User connected", .status = EventStatus::Info});
//   tl.add({.title = "Read auth middleware", .elapsed = "120ms",
//           .body = read_tool.build(), .status = EventStatus::Done});
//   tl.add({.title = "Investigating routes", .elapsed = "2.1s",
//           .body = investigation.build(), .status = EventStatus::Active});
//   tl.advance(dt);   // animates spinner glyphs on Active events
//
// The vertical rail auto-stretches to each event body's height because the
// rail column is a ComponentElement and Maya's flex defaults to align=Stretch
// in horizontal containers — render() is invoked with the row's allocated
// height, not the rail's measured height. The elapsed tag is right-aligned
// inside the same title TextElement using a computed gap, so it sticks to
// the right edge regardless of body width.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"
#include "spinner.hpp"

namespace maya {

// ============================================================================
// EventStatus — semantic role of an event, drives marker glyph & colour
// ============================================================================

enum class EventStatus : uint8_t {
    Pending,    // ○ dim — queued / not yet started
    Active,     // ⠋ spinner — currently in progress
    Done,       // ✓ green — completed successfully
    Failed,     // ✗ red — completed with error
    Info,       // ● blue — neutral informational point
    Warning,    // ▲ yellow — non-fatal alert
    Custom,     // user-supplied glyph + colour
};

// ============================================================================
// EventTimeline — bordered vertical feed of events with widget bodies
// ============================================================================

class EventTimeline {
public:
    struct Event {
        std::string  title;                      ///< single-line summary
        std::string  elapsed;                    ///< pre-formatted (e.g. "1.2s"); shown right-aligned
        Element      body{};                     ///< optional rich content (any widget)
        EventStatus  status = EventStatus::Info;

        // Used only when status == Custom.
        std::string  custom_marker;              ///< UTF-8 glyph (1-2 cells wide)
        Color        custom_color = Color::white();

        // Style override for the title text. Default = terminal default.
        Style        title_style{};
    };

private:
    std::vector<Event>            events_;
    int                           inter_event_spacing_ = 1;     ///< rail-only rows between events
    bool                          show_rail_           = true;
    Style                         elapsed_style_       = Style{}.with_dim();
    Spinner<SpinnerStyle::Dots>   spinner_{Style{}};

public:
    EventTimeline() = default;

    // ── Mutation ──────────────────────────────────────────────────────────

    /// Append an event. Returns a reference so callers can keep mutating
    /// the just-added event (set body/status/elapsed) as data streams in.
    /// References stay valid until the next add()/clear() — use back() to
    /// re-fetch if you've appended siblings since.
    Event& add(Event e) {
        events_.push_back(std::move(e));
        return events_.back();
    }

    [[nodiscard]] Event& back() { return events_.back(); }
    [[nodiscard]] const Event& back() const { return events_.back(); }
    [[nodiscard]] Event& at(std::size_t i) { return events_[i]; }
    [[nodiscard]] const Event& at(std::size_t i) const { return events_[i]; }
    [[nodiscard]] std::size_t size() const noexcept { return events_.size(); }

    void clear() { events_.clear(); }

    // ── Configuration ─────────────────────────────────────────────────────

    void set_show_rail(bool b)            { show_rail_           = b; }
    void set_inter_event_spacing(int n)   { inter_event_spacing_ = n; }
    void set_elapsed_style(Style s)       { elapsed_style_       = s; }

    /// Tick the shared spinner — drives every Active event's marker.
    /// One spinner across the whole timeline keeps active events in lockstep.
    void advance(float dt) { spinner_.advance(dt); }

    // ── Rendering ─────────────────────────────────────────────────────────

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        if (events_.empty()) {
            return Element{TextElement{}};  // empty
        }

        std::vector<Element> rows;
        rows.reserve(events_.size() * (1 + static_cast<std::size_t>(inter_event_spacing_)));

        for (std::size_t i = 0; i < events_.size(); ++i) {
            const bool last = (i + 1 == events_.size());
            rows.push_back(build_event_row(events_[i], /*extend_rail=*/!last));

            if (!last) {
                for (int s = 0; s < inter_event_spacing_; ++s) {
                    rows.push_back(build_spacer_row());
                }
            }
        }

        return dsl::v(std::move(rows)).build();
    }

private:
    // Box-drawing rail glyph and width-1 marker fallback.
    static constexpr std::string_view kRailGlyph = "\xe2\x94\x82";  // │

    struct Marker {
        std::string glyph;
        Color       color;
    };

    [[nodiscard]] Marker marker_for(const Event& e) const {
        switch (e.status) {
            case EventStatus::Pending: return {"\xe2\x97\x8b", Color::bright_black()}; // ○
            case EventStatus::Active:  return {std::string{spinner_.current_frame()},
                                               Color::yellow()};                       // ⠋
            case EventStatus::Done:    return {"\xe2\x9c\x93", Color::green()};        // ✓
            case EventStatus::Failed:  return {"\xe2\x9c\x97", Color::red()};          // ✗
            case EventStatus::Info:    return {"\xe2\x97\x8f", Color::blue()};         // ●
            case EventStatus::Warning: return {"\xe2\x96\xb2", Color::yellow()};       // ▲
            case EventStatus::Custom:  return {e.custom_marker.empty()
                                                ? std::string{"\xe2\x97\x8f"}
                                                : e.custom_marker,
                                               e.custom_color};
        }
        return {"\xe2\x97\x8f", Color::white()};
    }

    // Rail column: marker on top row, dim │ on every subsequent row when
    // extend_below is true (otherwise blanks). Sized via measure() to one
    // marker-wide column; align_self = Stretch makes the row layout expand
    // the rail's allocated height to match its taller siblings (the body
    // widget) — without it, FlexStyle::align_self defaults to Start, which
    // overrides the parent's align_items=Stretch and pins the rail at its
    // measured 1-row height even when the body next to it is many rows.
    [[nodiscard]] static Element build_rail_column(std::string marker,
                                                   Color       color,
                                                   bool        extend_below) {
        const int marker_w = string_width(marker);
        FlexStyle layout{};
        layout.align_self = Align::Stretch;
        return Element{ComponentElement{
            .render = [marker = std::move(marker), color, extend_below]
                      (int /*w*/, int h) -> Element {
                std::vector<Element> lines;
                lines.reserve(static_cast<std::size_t>(h));
                lines.push_back(Element{TextElement{
                    .content = marker,
                    .style   = Style{}.with_fg(color),
                    .wrap    = TextWrap::NoWrap,
                }});
                for (int r = 1; r < h; ++r) {
                    lines.push_back(Element{TextElement{
                        .content = extend_below ? std::string{kRailGlyph} : std::string{" "},
                        .style   = Style{}.with_dim(),
                        .wrap    = TextWrap::NoWrap,
                    }});
                }
                return dsl::v(std::move(lines)).build();
            },
            .measure = [marker_w](int /*max_w*/) -> Size {
                return Size{Columns{marker_w}, Rows{1}};
            },
            .layout = layout,
        }};
    }

    // Title row: title text on the left, elapsed tag right-aligned to the
    // allocated width. Implemented as a single TextElement with a computed
    // gap of spaces and a StyledRun for the elapsed segment so wrap is
    // predictable and the tag never line-breaks below the title.
    [[nodiscard]] Element build_title_row(std::string title,
                                          std::string elapsed,
                                          Style       title_style) const {
        Style elapsed_style = elapsed_style_;
        return Element{ComponentElement{
            .render = [title = std::move(title),
                       elapsed = std::move(elapsed),
                       title_style, elapsed_style](int w, int /*h*/) -> Element {
                if (elapsed.empty()) {
                    // No elapsed → just the title, single line.
                    return Element{TextElement{
                        .content = title,
                        .style   = title_style,
                        .wrap    = TextWrap::TruncateEnd,
                    }};
                }

                const int title_w   = string_width(title);
                const int elapsed_w = string_width(elapsed);
                int gap = w - title_w - elapsed_w;

                // If the line doesn't fit, truncate the title and keep the
                // elapsed tag — the tag is the high-information bit while a
                // step is in progress.
                if (gap < 1) {
                    int keep = w - elapsed_w - 2;  // 1 for ellipsis, 1 gap
                    if (keep < 1) {
                        return Element{TextElement{
                            .content = elapsed,
                            .style   = elapsed_style,
                            .wrap    = TextWrap::NoWrap,
                        }};
                    }
                    std::string trimmed = detail::truncate_end(title, keep);
                    int trimmed_w = string_width(trimmed);
                    gap = w - trimmed_w - elapsed_w;
                    if (gap < 1) gap = 1;
                    return assemble(trimmed, elapsed, gap, title_style, elapsed_style);
                }
                return assemble(title, elapsed, gap, title_style, elapsed_style);
            },
        }};
    }

    [[nodiscard]] static Element assemble(std::string_view title,
                                          std::string_view elapsed,
                                          int              gap,
                                          Style            title_style,
                                          Style            elapsed_style) {
        std::string content;
        content.reserve(title.size() + static_cast<std::size_t>(gap) + elapsed.size());
        content.append(title);
        const std::size_t gap_off     = content.size();
        content.append(static_cast<std::size_t>(gap), ' ');
        const std::size_t elapsed_off = content.size();
        content.append(elapsed);

        std::vector<StyledRun> runs;
        runs.push_back(StyledRun{0, title.size(), title_style});
        runs.push_back(StyledRun{gap_off, static_cast<std::size_t>(gap), Style{}});
        runs.push_back(StyledRun{elapsed_off, elapsed.size(), elapsed_style});
        return Element{TextElement{
            .content = std::move(content),
            .style   = {},
            .wrap    = TextWrap::NoWrap,
            .runs    = std::move(runs),
        }};
    }

    [[nodiscard]] Element build_event_row(const Event& e, bool extend_rail) const {
        Marker m = marker_for(e);

        // Body block: title row (with right-aligned elapsed) stacked above
        // the optional user widget. Skip an empty default-constructed body
        // so events without a body don't get a stray 1-row blank gap.
        std::vector<Element> body_parts;
        if (!e.title.empty() || !e.elapsed.empty()) {
            body_parts.push_back(build_title_row(e.title, e.elapsed, e.title_style));
        }
        if (auto const* te = as_text(e.body); te == nullptr || !te->content.empty()) {
            body_parts.push_back(e.body);
        }
        if (body_parts.empty()) {
            body_parts.push_back(Element{TextElement{}});  // 1-row spacer
        }

        Element body_block = (body_parts.size() == 1)
            ? std::move(body_parts[0])
            : dsl::v(std::move(body_parts)).build();

        // Horizontal layout: [rail] [gap] [body]
        std::vector<Element> cols;
        if (show_rail_) {
            cols.push_back(build_rail_column(m.glyph, m.color, extend_rail));
            cols.push_back(Element{TextElement{.content = "  ", .wrap = TextWrap::NoWrap}});
        }
        cols.push_back(std::move(body_block));

        return dsl::h(std::move(cols)).build();
    }

    // Inter-event spacer: a single row with just the rail glyph — visually
    // connects consecutive event markers.
    [[nodiscard]] Element build_spacer_row() const {
        std::vector<Element> cols;
        if (show_rail_) {
            cols.push_back(Element{TextElement{
                .content = std::string{kRailGlyph},
                .style   = Style{}.with_dim(),
                .wrap    = TextWrap::NoWrap,
            }});
            cols.push_back(Element{TextElement{.content = "  ", .wrap = TextWrap::NoWrap}});
        }
        return dsl::h(std::move(cols)).build();
    }
};

}  // namespace maya
