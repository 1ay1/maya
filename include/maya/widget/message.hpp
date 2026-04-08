#pragma once
// maya::widget::message — User and assistant message containers
//
// Zed's bordered design + Claude Code's flow:
//   User: rounded border box (Zed agent panel style)
//   Assistant: clean, padded content (no border, natural flow)
//
// Usage:
//   auto user_msg = UserMessage::build("show me the project structure");
//   auto asst_msg = AssistantMessage::build(markdown_element);

#include <string>
#include <string_view>
#include <utility>

#include "../element/builder.hpp"
#include "../style/border.hpp"
#include "../style/style.hpp"

namespace maya {

// ============================================================================
// UserMessage — Zed-style bordered user input
// ============================================================================

struct UserMessage {
    [[nodiscard]] static Element build(std::string_view content) {
        return build(Element{TextElement{
            .content = std::string{content},
            .style = Style{}.with_fg(Color::rgb(200, 204, 212)),
        }});
    }

    [[nodiscard]] static Element build(Element content) {
        return detail::box()
            .border(BorderStyle::Round)
            .border_color(Color::rgb(50, 54, 62))
            .padding(0, 1, 0, 1)(
                std::move(content)
            );
    }
};

// ============================================================================
// AssistantMessage — clean flow container
// ============================================================================

struct AssistantMessage {
    [[nodiscard]] static Element build(Element content) {
        return detail::box().padding(0, 0, 0, 2)(
            std::move(content)
        );
    }
};

} // namespace maya
