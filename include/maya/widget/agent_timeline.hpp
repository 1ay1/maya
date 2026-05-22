#pragma once
// maya::widget::AgentTimeline — bordered Actions panel for agent tool turns.
//
// CI-pipeline-style log of tool events inside a single bordered card. Each
// event has a tree glyph, status icon, name + detail line + duration, an
// optional indented body under a `│` connector, and (when settled) a footer
// summary. The widget OWNS the chrome — borders, tree glyphs, status icons,
// duration formatting, body stripe, footer. Callers supply pure data:
// per-tool body element, category color, rich event status.
//
//   maya::AgentTimeline{{
//       .title        = " ACTIONS  ·  3/5  ·  Bash ",
//       .border_color = Color::cyan(),
//       .frame        = spinner_frame,
//       .stats        = {{"INSPECT", 3, Color::blue()},
//                        {"MUTATE",  2, Color::magenta()}},
//       .events       = {{.name="Bash", .detail="npm test  ·  exit 0",
//                         .elapsed_seconds=1.2f,
//                         .category_color=Color::green(),
//                         .status=AgentEventStatus::Done,
//                         .body=preview_block_element}},
//       .footer       = {{.glyph="\xe2\x9c\x93", .text="done",
//                         .color=Color::green(), .summary="3 actions   1.4s"}},
//   }}.build();

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../style/border.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

#include "tool_body_preview.hpp"

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
    std::string             name;                 // Display name e.g. "Bash"
    std::string             detail;               // One-line summary
    float                   elapsed_seconds = 0.0f;
    Color                   category_color  = Color::blue();
    AgentEventStatus        status          = AgentEventStatus::Pending;
    ToolBodyPreview::Config body;                 // empty by default; rendered under `│` stripe

    // Optional content-stable cache key (Witness Chain) for the
    // rendered event sub-tree (header row + striped body rows).
    //
    // When non-empty AND the event is in a TERMINAL state
    // (Done / Failed / Rejected), AgentTimeline::append_event wraps
    // the per-event rows in a ComponentElement whose hash_id is
    // this CacheId. The renderer's hash-keyed component_cache then
    // stores the rendered cells against this key and serves every
    // subsequent frame as a cell-blit — layout + paint of the tool
    // body is skipped entirely.
    //
    // Why "terminal" only: a Running / Pending tool's card carries
    // a live spinner frame in its status icon, and a Done card with
    // a frame-dependent state somewhere inside would lock a stale
    // spinner glyph into the cells. Terminal events render no
    // frame-driven state (status icon collapses to a static ✓ / ✗ /
    // ⊘), so their cell snapshot is byte-stable until the host
    // mutates the event itself (which it signals by changing the
    // hash_id, typically by mixing the tool's wire id into the
    // CacheIdBuilder).
    //
    // Default empty → every frame walks the event sub-tree, matching
    // the pre-cache behaviour for callers that haven't opted in.
    CacheId                 hash_id;
};

struct AgentTimelineStat {
    std::string label;                        // raw — widget small-caps's it
    int         count = 0;
    Color       color = Color::blue();
};

struct AgentTimelineFooter {
    std::string glyph;
    std::string text;
    Color       color = Color::green();
    std::string summary;
};

class AgentTimeline {
public:
    struct Config {
        std::string                          title;
        // Optional right-aligned segment on the same (top) border edge.
        // Used to pin a duration / status pill to the right side of the
        // panel header while the main title stays left-anchored — a
        // single `title` string alone is start-aligned and can't express
        // that split.
        std::string                          title_end;
        Color                                border_color = Color::bright_black();
        int                                  frame        = 0;
        std::vector<AgentTimelineStat>       stats;
        std::vector<AgentTimelineEvent>      events;
        std::optional<AgentTimelineFooter>   footer;
    };

    explicit AgentTimeline(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        std::vector<Element> rows;
        rows.reserve(cfg_.events.size() * 4 + 4);

        // Stats row visibility is gated only on the host having
        // provided stats — NOT on the event count. The previous
        // `events.size() > 1` gate produced a +2-row height bump the
        // instant a second tool joined a panel mid-stream, which on
        // panels straddling the scrollback↔viewport seam stranded the
        // pre-bump top edge in native scrollback (visible as a
        // half-floating "A C T I O N S" header above the live panel).
        // Holding the row from frame 1 onward keeps the panel's
        // intrinsic row count monotone across its lifecycle: rows can
        // only be appended (more events, footer transition), never
        // inserted above already-emitted content. One extra row of
        // chrome on single-tool turns is the cost; visual stability
        // across multi-tool turns is the payoff.
        const bool show_stats = !cfg_.stats.empty();
        if (show_stats) {
            rows.push_back(stats_row());
            rows.push_back(blank());
        }
        for (std::size_t i = 0; i < cfg_.events.size(); ++i) append_event(rows, i);
        if (cfg_.footer) {
            rows.push_back(blank());
            rows.push_back(footer_row(*cfg_.footer));
        }

        // align_self(Stretch) forces the panel to claim the parent's
        // full cross-axis width regardless of how wide its rows
        // naturally measure. Without it, the bordered card hugs its
        // widest row (typically the stats row or the longest event
        // detail) and the right border lands mid-viewport — the
        // panel reads as "not responsive" as the terminal widens.
        // The inner rows already carry grow(1.0f) so they fill the
        // available width once the panel itself is stretched.
        //
        // Uses the runtime BoxBuilder (vstack()) instead of the
        // compile-time `v(...)` node because the DSL pipe layer
        // doesn't surface an align_self setter — the property has
        // to be set on the builder before children are accepted.
        // overflow:Hidden clips per-row content at the card's right
        // border so long file-write / file-read content can never
        // visibly bleed past the bordered rectangle. The row TextElement
        // already declares wrap=TruncateEnd which should fit the text
        // into the layout-allocated width, but with auto-width row
        // containers there's a small flex-shrink window where children
        // are measured before the parent's cross-stretch resolves the
        // row's definite width — long content from a write tool's
        // 100+ row body can land past the right border before the
        // shrink pass converges, especially when the AgentTimeline
        // gets stretched to a width that's narrower than the body
        // rows' natural widths. The canvas-level clip is the
        // structural guard against that escape hatch.
        auto box = vstack()
            .align_self(Align::Stretch)
            .border(BorderStyle::Round)
            .padding(0, 1, 0, 1)
            .border_color(cfg_.border_color)
            .border_text(cfg_.title, BorderTextPos::Top, BorderTextAlign::Start)
            .overflow(Overflow::Hidden);
        if (!cfg_.title_end.empty()) {
            box = std::move(box).border_text_end(
                cfg_.title_end, BorderTextPos::Top, BorderTextAlign::End);
        }
        return std::move(box)(rows);
    }

private:
    Config cfg_;

    // ── Stats row: small-caps category badges separated by mid-dots.
    //    Dots stay bright_black (truly ornamental — separators between
    //    badges). Counts use ANSI 7 (mid-gray, legible) so "INSPECT 2 ·
    //    MUTATE 1" doesn't collapse below readability on low-contrast
    //    terminal themes.
    [[nodiscard]] Element stats_row() const {
        using namespace dsl;
        const Color sep_color   = Color::bright_black();   // ornamental dots
        const Color count_color = Color::white();          // legible mid-gray

        std::vector<Element> parts;
        parts.reserve(cfg_.stats.size() * 3);
        for (std::size_t i = 0; i < cfg_.stats.size(); ++i) {
            const auto& s = cfg_.stats[i];
            if (i > 0)
                parts.push_back(text("  \xc2\xb7  ", Style{}.with_fg(sep_color)));
            parts.push_back(text(small_caps(s.label),
                                 Style{}.with_fg(s.color).with_bold()));
            parts.push_back(text(" " + std::to_string(s.count),
                                 Style{}.with_fg(count_color)));
        }
        return (h(parts) | grow(1.0f)).build();
    }

    // ── Footer row: `   ✓ DONE   3 actions   1.4s`. The DONE glyph +
    //    label carry the status color; the summary uses ANSI 7 (legible
    //    mid-gray) so the count + duration stays readable instead of
    //    collapsing into background on dark themes.
    [[nodiscard]] static Element footer_row(const AgentTimelineFooter& f) {
        using namespace dsl;
        return h(
            text("   "),
            text(f.glyph + " ", Style{}.with_fg(f.color).with_bold()),
            text(small_caps(f.text), Style{}.with_fg(f.color).with_bold()),
            text("   "),
            text(f.summary, Style{}.with_fg(Color::white()))
        ).build();
    }

    // ── Per-event block: header row + (optional) body rows + (optional)
    //    inter-event connector. Pushed into `rows` so v() picks them up
    //    in order.
    //
    // Caching: when ev.hash_id is set AND the event is terminal
    // (Done / Failed / Rejected), the (header + body) sub-tree is
    // wrapped in a ComponentElement so the renderer's hash-keyed
    // cache blits cells across frames instead of re-walking
    // ToolBodyPreview's tree. The inter-event connector stays
    // outside the cached wrapper because its color depends on the
    // NEXT event's status — caching it under THIS event's id would
    // freeze the connector to whatever next-status existed at first
    // population, missing later transitions like "the running tool
    // below me just turned green."
    void append_event(std::vector<Element>& rows, std::size_t i) const {
        using namespace dsl;
        const auto& ev       = cfg_.events[i];
        const bool is_last   = (i + 1 == cfg_.events.size());
        const bool is_terminal = (ev.status == AgentEventStatus::Done
                               || ev.status == AgentEventStatus::Failed
                               || ev.status == AgentEventStatus::Rejected);

        if (is_terminal && !ev.hash_id.empty()) {
            ComponentElement comp;
            comp.hash_id = ev.hash_id;
            // Capture the event by shared_ptr<const> so the lambda owns
            // a stable view of its inputs without per-frame deep-copy
            // of body text. Prior by-value capture cost
            // sizeof(AgentTimelineEvent) (strings + ToolBodyPreview body
            // text) on every AgentTimeline::build() call — even when
            // the cache fast-path won't invoke the lambda. For a
            // settled write/edit body with hundreds of lines that's
            // hundreds of KB of std::string copies per frame per event,
            // which made the cached path measurably SLOWER than the
            // uncached path on small bodies (see test_render_scaling
            // test_agent_timeline_per_event_hash_id_bounds_cost).
            // shared_ptr capture is one ref-bump per frame, fixed cost.
            auto ev_sp = std::make_shared<const AgentTimelineEvent>(ev);
            std::string glyph{tree_glyph(i, cfg_.events.size())};
            comp.render = [ev_sp = std::move(ev_sp),
                           glyph_copy = std::move(glyph)]
                          (int /*w*/, int /*h*/) -> Element {
                std::vector<Element> sub;
                sub.reserve(4);
                build_event_body(sub, *ev_sp, /*is_terminal=*/true,
                                 /*frame=*/0, glyph_copy);
                return v(std::move(sub)).build();
            };
            rows.push_back(Element{std::move(comp)});
        } else {
            build_event_body(rows, ev, is_terminal, cfg_.frame,
                             std::string{tree_glyph(i, cfg_.events.size())});
        }

        if (!is_last) {
            const Color next_cc = event_connector_color(cfg_.events[i + 1].status);
            rows.push_back(h(
                text("   "),
                text("\xe2\x94\x82", Style{}.with_fg(next_cc).with_dim())
            ).build());
        }
    }

    // Build the header row + body rows for a single event into `out`.
    // Pure function of its inputs — caller passes the live `frame` for
    // active spinner glyphs and the precomputed `glyph` so the cached
    // path can capture a position at lambda construction time.
    static void build_event_body(std::vector<Element>& out,
                                 const AgentTimelineEvent& ev,
                                 bool is_terminal,
                                 int frame,
                                 const std::string& glyph) {
        using namespace dsl;
        const bool is_active = (ev.status == AgentEventStatus::Pending
                             || ev.status == AgentEventStatus::Running);

        // `detail` is clipped (TruncateEnd) — when a tool's args don't fit
        // (long `powershell -c "…"` commands, paths past the right edge),
        // default Wrap would let the text bleed into N visual rows while
        // the parent h() still measures the row as 1 line, so the next
        // event's body / connector renders on top of the wrapped overflow.
        // The body preview below is the place for full content; the
        // header stays a 1-line summary with an ellipsis at the cut.
        //
        out.push_back((h(
            text(glyph, tree_style(ev.category_color, is_active)),
            text(" "),
            status_icon(ev.status, frame),
            text("  "),
            text(ev.name, name_style(ev.status, ev.category_color, is_active)),
            text("  "),
            text(ev.detail, detail_style(ev.category_color, is_active)) | clip,
            spacer(),
            when(is_terminal,
                 // Leading 2-col pad lives INSIDE the elapsed text so the
                 // trailing segment is unconditionally wider than just the
                 // duration glyphs. When the row is at its width limit,
                 // this forces `detail | clip` to shrink (yoga's flex
                 // arithmetic: hypothetical > available → truncate the
                 // shrink-eligible cell) instead of letting detail butt
                 // up against the elapsed with zero spacer. Result: the
                 // elapsed stays right-aligned and always has ≥2 cols of
                 // breathing room before it.
                 //
                 // `| nowrap`: text nodes default to TextWrap::Wrap, so
                 // if yoga decides to shrink THIS cell (it's shrinkable
                 // by default) below its natural width, the elapsed
                 // glyphs would break to a second visual row and render
                 // on top of the connector below. NoWrap keeps the row
                 // a single line; the card's Overflow::Hidden clips any
                 // horizontal bleed at the border instead of wrapping.
                 text("  " + format_duration(ev.elapsed_seconds),
                      Style{}.with_fg(duration_color(ev.elapsed_seconds)))
                 | nowrap)
        ) | grow(1.0f)).build());

        // Build the body via ToolBodyPreview, then stripe each child row
        // with the `│` connector. The body widget owns its own line layout
        // (vector<row> in a vstack); we walk its children to give each row
        // its own stripe, keeping every rendered row at exactly 1 line of
        // measured height (inline mode prefers that).
        const Color cc        = event_connector_color(ev.status);
        const Style stripe_st = is_active ? Style{}.with_fg(cc)
                                          : Style{}.with_fg(cc).with_dim();
        Element body_rule = h(
            text("   "),
            text("\xe2\x94\x82  ", stripe_st)
        ).build();

        Element body_el = ToolBodyPreview{ev.body}.build();
        if (auto* bx = as_box(body_el)) {
            for (const auto& child : bx->children)
                out.push_back((h(body_rule, child) | grow(1.0f)).build());
        } else if (auto* t = as_text(body_el); t && !t->content.empty()) {
            out.push_back((h(body_rule, body_el) | grow(1.0f)).build());
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

    static Style detail_style(Color cat, bool /*is_active*/) {
        // Tool detail (file path / args / command) renders in the
        // tool's category color + italic. The whole header row
        // ("✓ Name detail elapsed") now flows in the category hue —
        // strong visual identity per tool.
        return Style{}.with_fg(cat).with_italic();
    }

    // ── Pure helpers (formatting / glyphs / colors) ────────────────────

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

    static std::string_view tree_glyph(std::size_t idx, std::size_t total) {
        if (total == 1)        return "\xe2\x94\x80\xe2\x94\x80";  // ──
        if (idx == 0)          return "\xe2\x95\xad\xe2\x94\x80";  // ╭─
        if (idx + 1 == total)  return "\xe2\x95\xb0\xe2\x94\x80";  // ╰─
        return                        "\xe2\x94\x9c\xe2\x94\x80";  // ├─
    }

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

    static Color duration_color(float secs) {
        if (secs < 0.25f) return Color::green();
        if (secs < 2.0f)  return Color::bright_black();
        if (secs < 15.0f) return Color::yellow();
        return Color::red();
    }

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
