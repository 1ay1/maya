#pragma once
// maya::widget::thinking — Thinking/reasoning indicator block
//
// Shows an animated spinner with "Thinking..." label that can expand
// to reveal the reasoning content. Designed for AI thinking blocks.
//
// Usage:
//   ThinkingBlock thinking;
//   thinking.set_active(true);
//   thinking.append("Analyzing the code structure...");
//   // In render loop:
//   thinking.advance(dt);
//   return thinking.build();

#include <string>
#include <string_view>

#include "../element/builder.hpp"
#include "../style/border.hpp"
#include "../style/style.hpp"
#include "spinner.hpp"

namespace maya {

struct ThinkingConfig {
    Style label_style      = Style{}.with_dim().with_italic();
    Style content_style    = Style{}.with_dim();
    Color border_color     = Color::rgb(60, 60, 80);
    std::string label      = "Thinking";
    bool show_content      = false;   // expand to show reasoning
    bool show_border       = true;
    int max_visible_lines  = 5;
};

class ThinkingBlock {
    std::string content_;
    bool active_ = false;
    bool expanded_ = false;
    Spinner<SpinnerStyle::Dots> spinner_;
    ThinkingConfig cfg_;

public:
    ThinkingBlock() = default;
    explicit ThinkingBlock(ThinkingConfig cfg) : cfg_(std::move(cfg)) {}

    void set_active(bool active) { active_ = active; }
    [[nodiscard]] bool is_active() const { return active_; }

    void set_expanded(bool expanded) { expanded_ = expanded; }
    void toggle_expanded() { expanded_ = !expanded_; }
    [[nodiscard]] bool is_expanded() const { return expanded_; }

    void set_content(std::string_view content) { content_ = std::string{content}; }
    void append(std::string_view text) { content_ += text; }
    void clear() { content_.clear(); active_ = false; expanded_ = false; }

    void advance(float dt) {
        if (active_) spinner_.advance(dt);
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        if (!active_ && content_.empty()) {
            return Element{TextElement{}};
        }

        // Header: spinner + "Thinking..."
        std::vector<Element> header_parts;
        if (active_) {
            header_parts.push_back(spinner_.build());
            header_parts.push_back(Element{TextElement{
                .content = " " + cfg_.label + "...",
                .style = cfg_.label_style}});
        } else {
            // Completed thinking — show collapsed indicator
            std::string icon = expanded_
                ? "\xe2\x96\xbc "   // "▼ "
                : "\xe2\x96\xb6 ";  // "▶ "
            header_parts.push_back(Element{TextElement{
                .content = icon + cfg_.label,
                .style = cfg_.label_style}});
        }
        auto header = detail::hstack()(std::move(header_parts));

        // If not expanded and not showing content, just the header
        if (!expanded_ && !cfg_.show_content && !active_) {
            return std::move(header);
        }

        // Show content (truncated if needed)
        if (content_.empty()) {
            if (cfg_.show_border) {
                return detail::vstack()
                    .border(BorderStyle::Round)
                    .border_color(cfg_.border_color)
                    .padding(0, 1, 0, 1)(std::move(header));
            }
            return std::move(header);
        }

        // Truncate content to max_visible_lines
        std::string visible = truncate_lines(content_, cfg_.max_visible_lines);

        auto body = detail::vstack()(
            std::move(header),
            Element{TextElement{.content = std::move(visible),
                                .style = cfg_.content_style}});

        if (cfg_.show_border) {
            return detail::vstack()
                .border(BorderStyle::Round)
                .border_color(cfg_.border_color)
                .padding(0, 1, 0, 1)(std::move(body));
        }

        return body;
    }

private:
    static std::string truncate_lines(const std::string& text, int max_lines) {
        if (max_lines <= 0) return text;

        int count = 0;
        size_t pos = 0;
        while (pos < text.size() && count < max_lines) {
            auto nl = text.find('\n', pos);
            if (nl == std::string::npos) break;
            pos = nl + 1;
            ++count;
        }

        if (count >= max_lines && pos < text.size()) {
            return text.substr(0, pos) + "...";
        }
        return text;
    }
};

} // namespace maya
