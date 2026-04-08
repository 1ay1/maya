#pragma once
// maya::widget::scroll — Scrollable container with viewport clipping
//
// Wraps content that may be taller than the viewport. Renders only the
// visible rows and an optional braille-resolution scrollbar.
//
// Usage:
//   auto chat = scroll({.auto_bottom = true}, [&](int w, int h) {
//       return v(map(messages, render_msg)).build();
//   });

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include "../core/focus.hpp"
#include "../core/overload.hpp"
#include "../core/signal.hpp"
#include "../element/builder.hpp"
#include "../layout/yoga.hpp"
#include "../render/renderer.hpp"
#include "../style/style.hpp"
#include "../terminal/input.hpp"

namespace maya {

// ============================================================================
// ScrollConfig — compile-time or runtime configuration
// ============================================================================

struct ScrollConfig {
    bool auto_bottom = false;
    bool show_bar    = true;
};

// ============================================================================
// ScrollState — reactive scroll position tracking
// ============================================================================

class ScrollState {
    Signal<int> offset_{0};
    int content_height_ = 0;
    int viewport_height_ = 0;

public:
    [[nodiscard]] int offset()          const { return offset_(); }
    [[nodiscard]] int content_height()  const { return content_height_; }
    [[nodiscard]] int viewport_height() const { return viewport_height_; }

    [[nodiscard]] bool at_bottom() const {
        return offset_() + viewport_height_ >= content_height_;
    }

    [[nodiscard]] float progress() const {
        int max = content_height_ - viewport_height_;
        return max > 0 ? static_cast<float>(offset_()) / max : 1.0f;
    }

    [[nodiscard]] float thumb_ratio() const {
        return content_height_ > 0
            ? static_cast<float>(viewport_height_) / content_height_
            : 1.0f;
    }

    void scroll_by(int delta) {
        int max = std::max(0, content_height_ - viewport_height_);
        offset_.set(std::clamp(offset_() + delta, 0, max));
    }

    void scroll_to(int pos) {
        int max = std::max(0, content_height_ - viewport_height_);
        offset_.set(std::clamp(pos, 0, max));
    }

    void scroll_to_bottom() {
        int max = std::max(0, content_height_ - viewport_height_);
        offset_.set(max);
    }

    void update_metrics(int viewport_h, int content_h) {
        viewport_height_ = viewport_h;
        content_height_ = content_h;
        // Clamp offset if content shrunk
        int max = std::max(0, content_h - viewport_h);
        if (offset_() > max) offset_.set(max);
    }
};

// ============================================================================
// Scrollbar rendering — braille sub-character precision
// ============================================================================

namespace detail {

inline Element render_scrollbar(int height, float progress, float thumb_ratio,
                                uint16_t track_style, uint16_t thumb_style) {
    // Use a ComponentElement to render at the allocated size
    return Element{ComponentElement{
        .render = [=](int /*w*/, int h) -> Element {
            // Build a text column of braille characters
            if (h <= 0) return Element{TextElement{""}};

            int total_dots = h * 4;  // 4 vertical dots per braille cell
            int thumb_dots = std::max(2, static_cast<int>(thumb_ratio * total_dots));
            int thumb_start = static_cast<int>(progress * (total_dots - thumb_dots));

            std::string bar;
            bar.reserve(static_cast<size_t>(h * 4)); // UTF-8 braille chars

            for (int row = 0; row < h; ++row) {
                char32_t ch = U'\u2800'; // braille base
                bool has_thumb = false;
                for (int dot = 0; dot < 4; ++dot) {
                    int pos = row * 4 + dot;
                    if (pos >= thumb_start && pos < thumb_start + thumb_dots) {
                        // Left column dots: positions 0,1,2,6
                        static constexpr uint8_t left_bits[] = {0x01, 0x02, 0x04, 0x40};
                        ch |= left_bits[dot];
                        // Right column dots: positions 3,4,5,7
                        static constexpr uint8_t right_bits[] = {0x08, 0x10, 0x20, 0x80};
                        ch |= right_bits[dot];
                        has_thumb = true;
                    }
                }

                // Encode UTF-8
                if (ch < 0x80) {
                    bar += static_cast<char>(ch);
                } else if (ch < 0x800) {
                    bar += static_cast<char>(0xC0 | (ch >> 6));
                    bar += static_cast<char>(0x80 | (ch & 0x3F));
                } else {
                    bar += static_cast<char>(0xE0 | (ch >> 12));
                    bar += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                    bar += static_cast<char>(0x80 | (ch & 0x3F));
                }
                if (row < h - 1) bar += '\n';
                (void)has_thumb;
            }

            return Element{TextElement{
                .content = std::move(bar),
                .style = Style{}.with_fg(Color::rgb(80, 80, 100)),
            }};
        },
        .layout = {.width = Dimension::fixed(1)},
    }};
}

} // namespace detail

// ============================================================================
// scroll() — Factory for scrollable containers
// ============================================================================

class Scrollable {
    ScrollConfig cfg_;
    ScrollState* state_;
    std::function<Element(int w, int h)> content_fn_;

public:
    Scrollable(ScrollConfig cfg, ScrollState& state,
               std::function<Element(int w, int h)> content_fn)
        : cfg_(cfg), state_(&state), content_fn_(std::move(content_fn)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        // In inline/auto_height mode, the terminal's own scrollback provides
        // scrolling.  ComponentElement + grow=1 would collapse to 0 height
        // (no definite parent), so we bypass the scroll machinery entirely
        // and return content at its natural height.
        if (is_auto_height()) {
            int w = available_width();
            int content_w = cfg_.show_bar ? w - 1 : w;
            if (content_w < 1) content_w = 1;
            return content_fn_(content_w, 0);
        }

        auto* st = state_;
        auto cfg = cfg_;
        auto fn = content_fn_;

        return Element{ComponentElement{
            .render = [st, cfg, fn](int w, int h) -> Element {
                int content_w = cfg.show_bar ? w - 1 : w;
                if (content_w < 1) content_w = 1;

                Element content = fn(content_w, h);

                // Measure content height via layout only — no canvas or
                // painting.  build_layout_tree + compute is much cheaper
                // than a full render_tree.
                std::vector<layout::LayoutNode> lnodes;
                render_detail::build_layout_tree(content, lnodes, theme::dark);
                if (!lnodes.empty())
                    layout::compute(lnodes, 0, content_w);
                int measured_h = lnodes.empty() ? 1
                    : lnodes[0].computed.size.height.raw();

                // No scrolling needed — content fits the viewport.
                if (measured_h <= h) return content;

                st->update_metrics(h, measured_h);

                if (cfg.auto_bottom && st->at_bottom()) {
                    st->scroll_to_bottom();
                }

                int offset = st->offset();

                auto shifted = detail::box()
                    .direction(FlexDirection::Column)
                    .height(Dimension::fixed(measured_h))
                    .margin(-offset, 0, 0, 0)(
                        std::move(content)
                    );

                auto inner = detail::box()
                    .direction(FlexDirection::Column)
                    .width(Dimension::fixed(content_w))
                    .height(Dimension::fixed(h))
                    .overflow(Overflow::Hidden)(
                        std::move(shifted)
                    );

                if (cfg.show_bar) {
                    return detail::hstack()(
                        std::move(inner),
                        detail::render_scrollbar(h, st->progress(),
                            st->thumb_ratio(), 0, 0)
                    );
                }
                return inner;
            },
            .layout = {.grow = 1.0f},
        }};
    }

    // Handle scroll events
    bool handle(const Event& ev) {
        return std::visit(overload{
            [this](const KeyEvent& k) -> bool {
                if (auto* sp = std::get_if<SpecialKey>(&k.key)) {
                    switch (*sp) {
                        case SpecialKey::PageUp:   state_->scroll_by(-state_->viewport_height()); return true;
                        case SpecialKey::PageDown: state_->scroll_by(state_->viewport_height());  return true;
                        case SpecialKey::Up:       if (k.mods.ctrl) { state_->scroll_by(-1); return true; } break;
                        case SpecialKey::Down:     if (k.mods.ctrl) { state_->scroll_by(1); return true; }  break;
                        default: break;
                    }
                }
                return false;
            },
            [this](const MouseEvent& m) -> bool {
                if (m.button == MouseButton::ScrollUp)   { state_->scroll_by(-3); return true; }
                if (m.button == MouseButton::ScrollDown)  { state_->scroll_by(3);  return true; }
                return false;
            },
            [](const auto&) -> bool { return false; },
        }, ev);
    }
};

/// Create a scrollable container.
///   auto chat = scroll({.auto_bottom = true}, state, [&](int w, int h) {
///       return v(map(messages, render_msg)).build();
///   });
[[nodiscard]] inline Scrollable scroll(
    ScrollConfig cfg, ScrollState& state,
    std::function<Element(int w, int h)> content_fn)
{
    return Scrollable(cfg, state, std::move(content_fn));
}

} // namespace maya
