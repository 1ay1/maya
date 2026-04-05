#pragma once
// maya::components::MessageBubble — Chat message container (user/assistant)
//
//   MessageBubble({.role = Role::User, .content = "Hello!"})
//   MessageBubble({.role = Role::Assistant, .content = response_md,
//                  .is_streaming = true, .frame = tick})
//   MessageBubble({.role = Role::Assistant, .children = { Markdown({...}), CodeBlock({...}) }})
//
// User messages: right-aligned accent border, bold label
// Assistant messages: left-aligned muted border, with optional streaming indicator

#include "core.hpp"

namespace maya::components {

enum class Role { User, Assistant, System };

struct MessageBubbleProps {
    Role        role         = Role::User;
    std::string content      = "";
    Children    children     = {};
    bool        is_streaming = false;
    int         frame        = 0;
    std::string timestamp    = "";   // optional timestamp display
};

Element MessageBubble(MessageBubbleProps props);

} // namespace maya::components
