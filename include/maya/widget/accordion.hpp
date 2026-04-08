#pragma once
// maya::widget::accordion — Expand/collapse sections
//
// Ideal for thinking blocks, tool output, and collapsible detail panels.
// Toggles open/closed state via handle() or programmatic set_open().
//
// Usage:
//   Accordion thinking("Thinking...", content_element);
//   thinking.set_open(false);  // start collapsed
//
//   // In event handler:
//   if (auto* ke = as_key(ev)) thinking.handle(*ke);
//
//   // In render:
//   return thinking.build();

#include <functional>
#include <string>
#include <string_view>

#include "../core/focus.hpp"
#include "../core/overload.hpp"
#include "../element/builder.hpp"
#include "../style/style.hpp"
#include "../terminal/input.hpp"

namespace maya {

struct AccordionConfig {
    Style header_style     = Style{}.with_bold();
    Color border_color     = Color::rgb(60, 60, 80);
    Style collapsed_style  = Style{}.with_dim();
    std::string open_icon  = "\xe2\x96\xbc ";   // "▼ "
    std::string closed_icon = "\xe2\x96\xb6 ";  // "▶ "
    bool show_border       = false;
    bool start_open        = true;
};

class Accordion {
    std::string title_;
    Element content_;
    bool open_;
    FocusNode focus_;
    AccordionConfig cfg_;

public:
    Accordion() : open_(true) {}

    explicit Accordion(std::string_view title, Element content,
                       AccordionConfig cfg = {})
        : title_(title)
        , content_(std::move(content))
        , open_(cfg.start_open)
        , cfg_(std::move(cfg)) {}

    void set_title(std::string_view t) { title_ = std::string{t}; }
    void set_content(Element e) { content_ = std::move(e); }
    void set_open(bool open) { open_ = open; }
    void toggle() { open_ = !open_; }
    [[nodiscard]] bool is_open() const { return open_; }
    [[nodiscard]] FocusNode& focus_node() { return focus_; }

    /// Handle key event. Enter/Space toggles. Returns true if consumed.
    [[nodiscard]] bool handle(const KeyEvent& ev) {
        if (!focus_.focused()) return false;

        return std::visit(overload{
            [&](CharKey ck) -> bool {
                if (ck.codepoint == ' ') { toggle(); return true; }
                return false;
            },
            [&](SpecialKey sk) -> bool {
                if (sk == SpecialKey::Enter) { toggle(); return true; }
                return false;
            },
        }, ev.key);
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        // Header line: icon + title
        const auto& icon = open_ ? cfg_.open_icon : cfg_.closed_icon;
        auto header = detail::hstack()(
            Element{TextElement{.content = icon, .style = cfg_.header_style}},
            Element{TextElement{.content = title_, .style = cfg_.header_style}});

        if (!open_) {
            return std::move(header);
        }

        // Expanded: header + content
        if (cfg_.show_border) {
            return detail::vstack()
                .border(BorderStyle::Round)
                .border_color(cfg_.border_color)(
                    std::move(header),
                    content_);
        }

        return detail::vstack()(std::move(header), content_);
    }
};

} // namespace maya
