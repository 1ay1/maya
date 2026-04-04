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

    [[nodiscard]] Element render(int frame = 0) const {
        using namespace maya::dsl;
        auto& p = palette();

        // Count lines
        int line_count = 1;
        for (char c : props_.content) if (c == '\n') ++line_count;

        // Header
        std::vector<Element> header;
        header.push_back(text(expanded_ ? "▾" : "▸", Style{}.with_fg(p.dim)));
        header.push_back(text(" ", Style{}));

        if (props_.is_streaming) {
            header.push_back(text(spin(frame), Style{}.with_fg(p.secondary)));
            header.push_back(text(" ", Style{}));
        }

        header.push_back(text("Thinking",
                               Style{}.with_italic().with_fg(p.secondary)));

        if (!expanded_) {
            // Show preview of first line
            auto first_nl = props_.content.find('\n');
            auto preview = first_nl != std::string::npos
                ? props_.content.substr(0, std::min(first_nl, size_t(60)))
                : props_.content.substr(0, 60);
            if (!preview.empty()) {
                header.push_back(text("  ", Style{}));
                header.push_back(text(preview + (props_.content.size() > 60 ? "..." : ""),
                                       Style{}.with_fg(p.dim).with_italic()));
            }
        }

        std::vector<Element> rows;
        rows.push_back(hstack()(std::move(header)));

        if (expanded_) {
            // Show content with line limit
            std::string_view sv = props_.content;
            int shown = 0;
            while (!sv.empty() && shown < props_.max_lines) {
                auto nl = sv.find('\n');
                auto line = (nl == std::string_view::npos) ? sv : sv.substr(0, nl);
                rows.push_back(text("  " + std::string(line),
                                     Style{}.with_fg(p.muted).with_italic()));
                if (nl == std::string_view::npos) break;
                sv = sv.substr(nl + 1);
                ++shown;
            }

            if (line_count > props_.max_lines) {
                rows.push_back(text("  ... " + std::to_string(line_count - props_.max_lines) + " more lines",
                                     Style{}.with_fg(p.dim)));
            }

            if (props_.is_streaming) {
                rows.push_back(text("  █", Style{}.with_fg(p.secondary)));
            }
        }

        auto sides = BorderSides{.top = false, .right = false, .bottom = false, .left = true};
        return vstack()
            .border(BorderStyle::Round)
            .border_color(p.secondary)
            .border_sides(sides)
            .padding(0, 1, 0, 0)(std::move(rows));
    }
};

} // namespace maya::components
