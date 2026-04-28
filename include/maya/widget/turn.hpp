#pragma once
// maya::widget::Turn — one conversation turn.
//
// Visual identity of a single speaker turn:
//
//   ─── [↺ Restore checkpoint] ──────────────  (optional, above rail)
//   ┃ ❯ You                                        12:34  ·  turn 1
//   ┃
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
//   }}.build();

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
    };

    explicit Turn(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;
        const Color muted = Color::bright_black();

        // ── Header row: glyph + bold label + spacer + dim meta.
        auto header = h(
            text(cfg_.glyph, Style{}.with_fg(cfg_.rail_color)),
            text(" "),
            text(cfg_.label, Style{}.with_fg(cfg_.rail_color).with_bold()),
            spacer(),
            text(cfg_.meta, Style{}.with_fg(muted)),
            text(" ")
        ) | grow(1.0f);

        // ── Body: render each slot, blank line between consecutive ones.
        std::vector<Element> body_rows;
        body_rows.reserve(cfg_.body.size() * 2);
        bool first = true;
        for (const auto& slot : cfg_.body) {
            Element rendered = render_slot(slot);
            if (is_blank(rendered)) continue;
            if (!first) body_rows.push_back(blank());
            body_rows.push_back(std::move(rendered));
            first = false;
        }

        // ── Optional error banner under body.
        auto error_row = h(
            text("\xe2\x9a\xa0  ", Style{}.with_fg(Color::red()).with_bold()),    // ⚠
            text(cfg_.error, Style{}.with_fg(Color::red()).with_dim().with_italic())
        );

        auto inner = v(
            header,
            blank(),
            body_rows,
            when(!cfg_.error.empty(), v(blank(), error_row))
        ) | grow(1.0f);

        Element rail = maya::detail::box()
            .direction(FlexDirection::Row)
            .border(BorderStyle::Bold, cfg_.rail_color)
            .border_sides({.top = false, .right = false,
                           .bottom = false, .left = true})
            .padding(0, 0, 0, 2)
            .grow(1.0f)
          (inner.build());

        // ── Optional checkpoint divider above the rail.
        if (!cfg_.checkpoint_above) return rail;
        return v(
            CheckpointDivider{{
                .label = cfg_.checkpoint_label,
                .color = cfg_.checkpoint_color,
            }}.build(),
            std::move(rail)
        ).build();
    }

private:
    Config cfg_;

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
