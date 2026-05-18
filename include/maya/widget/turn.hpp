#pragma once
// maya::widget::Turn — one conversation turn.
//
// Visual identity of a single speaker turn:
//
//   ─── [↺ Restore checkpoint] ──────────────  (optional, above rail)
//   ┃ ❯ You                                        12:34  ·  turn 1
//   ┃ <body slot 0>
//   ┃
//   ┃ <body slot 1>            (Turn auto-spaces between slots)
//   ┃
//   ┃ ⚠  optional inline error
//
// Body slots are TYPED widget configs (or a pre-built Element escape
// hatch for cross-frame-cached widgets like StreamingMarkdown). Turn
// inserts a blank line between consecutive non-empty slots — callers
// don't push spacers themselves.
//
//   maya::Turn{{
//       .glyph      = "\xe2\x9c\xa6",                      // ✦
//       .label      = "Opus 4.7",
//       .rail_color = Color::magenta(),
//       .meta       = "12:34  \xc2\xb7  4.2s  \xc2\xb7  turn 3",
//       .body       = {
//           Turn::MarkdownText{ msg.text },                // markdown body
//           AgentTimeline::Config{ ... },                  // tool calls
//           Permission::Config{ ... },                     // pending permission
//       },
//       .error      = "stream cut off mid-tool",
//       .checkpoint_above = msg.has_checkpoint,
//       .hash_id    = CacheIdBuilder{}.add("turn"sv).add(msg.id).build(),
//   }}.build();
//
// Performance:
//   When `hash_id` is non-empty Turn wraps its output in a
//   maya::component() with that key. The renderer's hash-keyed
//   cells cache then short-circuits both layout (one node per turn)
//   and paint (one row-blit per cached turn) on every subsequent
//   frame — independent of the body's size. Settled turns in a long
//   conversation drop to O(1) per frame; only the actively-mutating
//   turn pays the build/parse cost. Leave hash_id empty on the live
//   turn (or rebuild it on each content edit) so the cache properly
//   misses and rebuilds.

#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "../dsl.hpp"
#include "../element/builder.hpp"
#include "../element/element.hpp"
#include "../style/border.hpp"
#include "../style/color.hpp"

#include "agent_timeline.hpp"
#include "checkpoint_divider.hpp"
#include "markdown.hpp"
#include "permission.hpp"

namespace maya {

class Turn {
public:
    // ── Body slot kinds ──────────────────────────────────────────────────
    struct PlainText {
        std::string content;
        Color       color = Color::bright_white();
    };

    struct MarkdownText {
        std::string content;
    };

    // Discriminated body slot. Each variant maps to a specific renderer:
    //   PlainText            → plain styled text
    //   MarkdownText         → maya::markdown(content)
    //   AgentTimeline::Config → maya::AgentTimeline (the Actions panel)
    //   Permission::Config    → maya::Permission     (inline permission card)
    //   Element              → escape hatch for widgets that need
    //                          cross-frame state (e.g. cached
    //                          StreamingMarkdown). Caller passes the
    //                          built Element; Turn just slots it.
    using BodySlot = std::variant<
        PlainText,
        MarkdownText,
        AgentTimeline::Config,
        Permission::Config,
        Element>;

    struct Config {
        std::string           glyph;
        std::string           label;
        Color                 rail_color = Color::cyan();
        std::string           meta;
        std::vector<BodySlot> body;
        std::string           error;        // empty = no error banner
        bool                  checkpoint_above = false;
        std::string           checkpoint_label = "Restore checkpoint";
        Color                 checkpoint_color = Color::yellow();

        // Set to true on the 2nd+ turn within a same-speaker run. The
        // header row (glyph + label + meta) is suppressed and the
        // Conversation widget skips the inter-turn divider, so the rail
        // and body of consecutive same-speaker turns flow as one block.
        // The agent-loop case ("agent did 3 reads in 3 API rounds")
        // visually reads as one logical action without collapsing the
        // underlying per-response Message structure that the Anthropic
        // protocol requires.
        bool                  continuation = false;

        // Content-stable cache key (Witness Chain). When non-empty,
        // Turn wraps its output in maya::component(...).hash_id(...),
        // so the renderer reuses the previously-painted cells on every
        // subsequent frame and skips re-running the body construction
        // (markdown parse, tool-card builds, permission lookups). The
        // id MUST change whenever the rendered output would change —
        // typically derive it as `CacheIdBuilder{}.add("turn"sv)
        // .add(msg_id).add(content_gen).build()` so an edit reliably
        // invalidates. An empty CacheId keeps the legacy per-frame
        // build path; use that for the live turn whose content
        // mutates as tokens stream in.
        CacheId               hash_id;
    };

    explicit Turn(Config c)
        : cfg_(std::make_shared<const Config>(std::move(c))) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;
        if (cfg_->hash_id.empty()) {
            // No caching opted in — preserve the eager build path
            // (no ComponentElement wrapper).
            return build_inner(*cfg_);
        }
        // Memoized path: defer construction into a component() whose
        // render lambda the renderer only invokes on cache miss. The
        // lambda captures a shared_ptr<const Config> (16 B) — small
        // enough to fit the std::function SBO on every mainstream
        // C++ runtime, so the per-frame allocation count is zero
        // even though the lambda is reconstructed each frame.
        auto cfg = cfg_;
        return maya::detail::component(
            [cfg = std::move(cfg)](int /*w*/, int /*h*/) -> Element {
                return build_inner(*cfg);
            })
            .grow(1.0f)
            .hash_id(cfg_->hash_id);
    }

private:
    std::shared_ptr<const Config> cfg_;

    [[nodiscard]] static Element build_inner(const Config& cfg) {
        using namespace dsl;
        const Color muted = Color::bright_black();

        // ── Header row: glyph + bold label + spacer + dim meta.
        //    Suppressed entirely on continuation turns — the previous
        //    turn in the run already showed it.
        //
        //    Wrapped in a component(...) so the row is built with an
        //    EXPLICIT width pulled from the layout pass. The plain
        //    h(...) | grow(1.0f) form measures the row at hypothetical
        //    sizes first — the spacer's flex_grow only distributes
        //    when its container's main axis is `main_definite`, which
        //    transitively requires the row's own style.width to be
        //    fixed. Inside an auto-width chain (Conversation → Turn
        //    rail → inner vstack), that never reliably resolves on
        //    the first pass, so the spacer collapses to zero and the
        //    meta strip lands flush against the label.
        //
        //    Inside the component lambda we KNOW the width (the layout
        //    engine hands us its resolved content_w) so we set
        //    width(Dimension::fixed(w)) on the row — the row's own
        //    compute_node then reports main_definite=true and the
        //    spacer claims its share of free space deterministically.
        Color rail_c = cfg.rail_color;
        std::string glyph_s = cfg.glyph;
        std::string label_s = cfg.label;
        std::string meta_s  = cfg.meta;
        Element header = maya::detail::component(
            [rail_c, glyph_s = std::move(glyph_s),
             label_s = std::move(label_s), meta_s = std::move(meta_s), muted]
            (int /*w*/, int /*h*/) -> Element {
                using namespace dsl;
                // The renderer's slow path forces the returned sub-tree's
                // root width to the parent's content_w before laying it
                // out, so we DON'T bake a width on the hstack here.
                // Setting width(fixed(w)) would lock the row to whatever
                // the measure pass passed (typically kUnconstrained),
                // pushing the meta strip past the canvas right edge.
                return maya::detail::hstack()
                  (text(glyph_s, Style{}.with_fg(rail_c)).build(),
                   text(" ").build(),
                   text(label_s, Style{}.with_fg(rail_c).with_bold()).build(),
                   spacer().build(),
                   text(meta_s, Style{}.with_fg(muted)).build(),
                   text(" ").build());
            })
            .grow(1.0f);

        // ── Body: render each slot with a one-row gap between
        //    consecutive non-blank slots. The gap matters most at the
        //    markdown→tool-panel boundary: without it the prose runs
        //    flush into the bordered actions card, fusing two distinct
        //    sections into one wall of text. Skipped slots (is_blank)
        //    don't trigger the gap so empty bodies stay zero-row.
        std::vector<Element> body_rows;
        body_rows.reserve(cfg.body.size() * 2);
        bool any_emitted = false;
        for (const auto& slot : cfg.body) {
            Element rendered = render_slot(slot);
            if (is_blank(rendered)) continue;
            if (any_emitted) body_rows.push_back(blank().build());
            body_rows.push_back(std::move(rendered));
            any_emitted = true;
        }

        // ── Optional error banner under body.
        auto error_row = h(
            text("\xe2\x9a\xa0  ", Style{}.with_fg(Color::red()).with_bold()),    // ⚠
            text(cfg.error, Style{}.with_fg(Color::red()).with_dim().with_italic())
        );

        // Optional error row (zero-row when absent). when()'s default
        // else-branch is blank() — a 1-row empty TextElement — which
        // would leave a phantom row at the bottom of every error-less
        // Turn. nothing() collapses to zero rows.
        auto error_block = [&]() -> Element {
            if (cfg.error.empty()) return nothing();
            return v(blank(), error_row).build();
        };

        // Two DSL branches build distinct compile-time tree types, so we
        // can't ternary directly — fold each to Element separately.
        // Blank row under the header separates it from the body.
        Element inner = cfg.continuation
            ? v(
                body_rows,
                error_block()
              ).build()
            : v(
                header,
                blank(),
                body_rows,
                error_block()
              ).build();

        Element rail = maya::detail::box()
            .direction(FlexDirection::Row)
            .border(BorderStyle::Bold, cfg.rail_color)
            .border_sides({.top = false, .right = false,
                           .bottom = false, .left = true})
            .padding(0, 0, 0, 2)
          (std::move(inner));

        // ── Optional checkpoint divider above the rail.
        if (!cfg.checkpoint_above) return rail;
        return v(
            CheckpointDivider{{
                .label = cfg.checkpoint_label,
                .color = cfg.checkpoint_color,
            }}.build(),
            std::move(rail)
        ).build();
    }

    [[nodiscard]] static Element render_slot(const BodySlot& slot) {
        using namespace dsl;
        return std::visit([](const auto& s) -> Element {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, PlainText>) {
                if (s.content.empty()) return blank();
                return text(s.content, Style{}.with_fg(s.color)).build();
            } else if constexpr (std::is_same_v<T, MarkdownText>) {
                if (s.content.empty()) return blank();
                return maya::markdown(s.content);
            } else if constexpr (std::is_same_v<T, AgentTimeline::Config>) {
                return AgentTimeline{s}.build();
            } else if constexpr (std::is_same_v<T, Permission::Config>) {
                Permission p{s};
                return p.build();
            } else if constexpr (std::is_same_v<T, Element>) {
                return s;
            } else {
                return blank();
            }
        }, slot);
    }

    // A slot rendered to a TextElement with empty content is "blank" —
    // skip it so we don't insert an extra blank line in front of nothing.
    [[nodiscard]] static bool is_blank(const Element& e) {
        if (auto* tx = maya::as_text(e); tx && tx->content.empty()) return true;
        return false;
    }
};

} // namespace maya
