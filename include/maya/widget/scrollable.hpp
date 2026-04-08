#pragma once
// maya::widget::scrollable — Scrollable container with viewport clipping
//
// Wraps content taller than the viewport and allows scrolling.
// Renders a scroll indicator on the right edge.
//
// Usage:
//   Scrollable scroll({.height = 10});
//   scroll.set_content(my_tall_element);
//   // In event handler: scroll.scroll_down(), scroll.scroll_up()
//   auto ui = scroll.build();

#include "../element/builder.hpp"
#include "../element/element.hpp"
#include "../style/style.hpp"
#include "../style/color.hpp"
#include "../terminal/input.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace maya {

struct ScrollConfig {
    int height = 10;             ///< Visible viewport height in rows
    int scroll_amount = 1;       ///< Rows per scroll step
    bool show_indicator = true;  ///< Show scroll position indicator
    Color indicator_color  = Color::rgb(80, 80, 120);
    Color indicator_active = Color::rgb(140, 140, 200);
};

class Scrollable {
    ScrollConfig cfg_;
    int offset_ = 0;
    int content_height_ = 0;
    Element content_{TextElement{}};

public:
    explicit Scrollable(ScrollConfig cfg = {}) : cfg_(cfg) {}

    void set_content(Element elem) { content_ = std::move(elem); }
    void set_content_height(int h) { content_height_ = h; }

    void scroll_up(int n = 0) {
        int amount = n > 0 ? n : cfg_.scroll_amount;
        offset_ = std::max(0, offset_ - amount);
    }

    void scroll_down(int n = 0) {
        int amount = n > 0 ? n : cfg_.scroll_amount;
        int max_offset = std::max(0, content_height_ - cfg_.height);
        offset_ = std::min(max_offset, offset_ + amount);
    }

    void scroll_to_top() { offset_ = 0; }
    void scroll_to_bottom() {
        offset_ = std::max(0, content_height_ - cfg_.height);
    }

    [[nodiscard]] int offset()  const noexcept { return offset_; }
    [[nodiscard]] int height()  const noexcept { return cfg_.height; }
    [[nodiscard]] int content_height() const noexcept { return content_height_; }

    /// Handle scroll-related key events (Page Up/Down, arrow keys when focused).
    /// Returns true if the event was consumed.
    bool handle(const KeyEvent& ev) {
        if (auto* sk = std::get_if<SpecialKey>(&ev.key)) {
            switch (*sk) {
                case SpecialKey::Up:       scroll_up();             return true;
                case SpecialKey::Down:     scroll_down();           return true;
                case SpecialKey::PageUp:   scroll_up(cfg_.height);  return true;
                case SpecialKey::PageDown: scroll_down(cfg_.height); return true;
                case SpecialKey::Home:     scroll_to_top();         return true;
                case SpecialKey::End:      scroll_to_bottom();      return true;
                default: break;
            }
        }
        return false;
    }

    /// Handle mouse scroll events.
    /// Returns true if the event was consumed.
    bool handle_mouse(const MouseEvent& me) {
        if (me.kind == MouseEventKind::Press) {
            if (me.button == MouseButton::ScrollUp)   { scroll_up();   return true; }
            if (me.button == MouseButton::ScrollDown) { scroll_down(); return true; }
        }
        return false;
    }

    /// Build the scrollable view.
    /// Uses overflow:hidden on a fixed-height box with margin-top offset to clip content.
    [[nodiscard]] Element build() const {
        // Build scroll indicator column
        std::string indicator;
        if (cfg_.show_indicator && content_height_ > cfg_.height) {
            int track_h = cfg_.height;
            // Thumb size: proportional to visible fraction
            int thumb_h = std::max(1, track_h * cfg_.height / content_height_);
            // Thumb position
            int max_off = std::max(1, content_height_ - cfg_.height);
            int thumb_pos = (track_h - thumb_h) * offset_ / max_off;

            for (int i = 0; i < track_h; ++i) {
                bool in_thumb = (i >= thumb_pos && i < thumb_pos + thumb_h);
                indicator += in_thumb
                    ? "\xe2\x94\x83"   // ┃ (thick vertical — active thumb)
                    : "\xe2\x94\x82";  // │ (thin vertical — track)
                if (i < track_h - 1) indicator += '\n';
            }
        }

        // Viewport: fixed height, clips overflow
        auto viewport = detail::vstack()
            .height(Dimension::fixed(cfg_.height))
            .overflow(Overflow::Hidden);

        // Inner content shifted upward by offset using negative top margin
        auto inner = detail::vstack()
            .margin(-offset_, 0, 0, 0);

        auto content_box = inner(content_);

        if (cfg_.show_indicator && content_height_ > cfg_.height) {
            return detail::hstack()(
                viewport(std::move(content_box)),
                Element{TextElement{
                    .content = indicator,
                    .style   = Style{}.with_fg(cfg_.indicator_color),
                }}
            );
        }

        return viewport(std::move(content_box));
    }

    /// Implicit conversion to Element.
    operator Element() const { return build(); }
};

} // namespace maya
