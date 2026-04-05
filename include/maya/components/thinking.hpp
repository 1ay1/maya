#pragma once
// maya::components::ThinkingBlock — Collapsible thinking/reasoning display
//
//   ThinkingBlock({.content = "Let me analyze the code structure...",
//                  .expanded = false})
//
//   ThinkingBlock({.content = thinking_text,
//                  .expanded = true,
//                  .is_streaming = true,
//                  .frame = tick})

#include "core.hpp"

namespace maya::components {

struct ThinkingBlockProps {
    std::string content      = "";
    bool        expanded     = false;
    bool        is_streaming = false;
    int         frame        = 0;
    int         max_lines    = 10;     // max visible when expanded
};

class ThinkingBlock {
    bool expanded_;
    ThinkingBlockProps props_;

public:
    explicit ThinkingBlock(ThinkingBlockProps props = {})
        : expanded_(props.expanded), props_(std::move(props)) {}

    void toggle() { expanded_ = !expanded_; }
    void set_expanded(bool v) { expanded_ = v; }
    [[nodiscard]] bool expanded() const { return expanded_; }

    void set_content(std::string c) { props_.content = std::move(c); }
    void set_streaming(bool v) { props_.is_streaming = v; }

    [[nodiscard]] Element render(int frame = 0) const;
};

} // namespace maya::components
