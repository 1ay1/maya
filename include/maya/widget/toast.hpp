#pragma once
// maya::widget::toast — Notification toast stack
//
// Zed's bordered notification cards + Claude Code's severity indicators.
//
//   ╭─ ✓ ──────────────────────────────────╮
//   │ Response complete                    │
//   ╰──────────────────────────────────────╯
//   ╭─ ⚠ ──────────────────────────────────╮
//   │ Context window at 85%                │
//   ╰──────────────────────────────────────╯
//
// Usage:
//   ToastManager toasts;
//   toasts.push("Response complete", ToastLevel::Success);
//   auto ui = toasts.build();   // expiry is clock-driven; no advance() needed
//
// Responsive: cards are clamped to `max_width` (default 60) and pinned to
// the right edge — the toast convention everywhere — instead of stretching
// into a full-width banner on a wide terminal. Below max_width the clamp is
// transparent and the message text wraps inside the card.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "../core/motion.hpp"   // anim::default_clock / keep_animating_after
#include "../dsl.hpp"
#include "../style/border.hpp"
#include "../style/style.hpp"

namespace maya {

enum class ToastLevel : uint8_t { Info, Success, Warning, Error };

struct ToastManager {
    struct Toast {
        std::string message;
        ToastLevel level;
        std::int64_t expires_at_ms;   // shared-clock deadline
    };

    struct Config {
        float duration    = 3.0f;
        float fade_time   = 0.5f;
        int   max_visible = 3;
        int   max_width   = 60;   // clamp card width; 0 = full slot
        bool  right_align = true; // pin clamped cards to the right edge
    };

private:
    Config cfg_;
    // mutable: build() prunes expired toasts lazily against the shared
    // clock — the pruning is idempotent, order-preserving housekeeping.
    mutable std::vector<Toast> toasts_;

    struct LevelInfo {
        const char* icon;
        Color color;
    };

    static LevelInfo level_info(ToastLevel level) {
        switch (level) {
        case ToastLevel::Info:    return {"\xe2\x84\xb9", Color::blue()};    // ℹ
        case ToastLevel::Success: return {"\xe2\x9c\x93", Color::green()};   // ✓
        case ToastLevel::Warning: return {"\xe2\x9a\xa0", Color::yellow()};  // ⚠
        case ToastLevel::Error:   return {"\xe2\x9c\x97", Color::red()};     // ✗
        }
        return {"\xe2\x84\xb9", Color::blue()};
    }

public:
    ToastManager() : cfg_{} {}
    explicit ToastManager(Config cfg) : cfg_(std::move(cfg)) {}

    void push(std::string message, ToastLevel level = ToastLevel::Info) {
        toasts_.push_back({std::move(message), level,
                           anim::default_clock().now_ms()
                               + static_cast<std::int64_t>(
                                     cfg_.duration * 1000.0f)});
    }

    /// No-op for source compat — toasts now expire against the shared
    /// animation clock inside build(); nothing to advance.
    [[deprecated("toasts are clock-driven; remove the advance() call")]]
    void advance(float) noexcept {}

    [[nodiscard]] bool empty() const { return toasts_.empty(); }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        // Clock-driven lifecycle: drop expired toasts, then schedule ONE
        // wake at the nearest upcoming transition (fade start or expiry) so
        // the stack redraws exactly when something visibly changes.
        const std::int64_t now = anim::default_clock().now_ms();
        toasts_.erase(
            std::remove_if(toasts_.begin(), toasts_.end(),
                [&](auto const& t) { return t.expires_at_ms <= now; }),
            toasts_.end());

        const std::int64_t fade_ms =
            static_cast<std::int64_t>(cfg_.fade_time * 1000.0f);
        std::int64_t next_change = -1;
        for (auto const& t : toasts_) {
            const std::int64_t until_expiry = t.expires_at_ms - now;
            const std::int64_t until_fade   = until_expiry - fade_ms;
            const std::int64_t u =
                until_fade > 0 ? until_fade : until_expiry;
            if (u > 0 && (next_change < 0 || u < next_change))
                next_change = u;
        }
        if (next_change > 0) anim::keep_animating_after(next_change);

        std::vector<Element> cards;

        // Show up to max_visible, newest at bottom
        int start = static_cast<int>(toasts_.size()) - cfg_.max_visible;
        if (start < 0) start = 0;

        for (int i = start; i < static_cast<int>(toasts_.size()); ++i) {
            auto const& toast = toasts_[static_cast<size_t>(i)];
            auto info = level_info(toast.level);

            bool fading = (toast.expires_at_ms - now) < fade_ms;

            // Border color tinted by severity
            Color border_color = Color::bright_black();
            if (toast.level == ToastLevel::Error)
                border_color = Color::red();
            else if (toast.level == ToastLevel::Warning)
                border_color = Color::yellow();
            else if (toast.level == ToastLevel::Success)
                border_color = Color::green();

            // Border label with icon
            std::string border_label = " ";
            border_label += info.icon;
            border_label += " ";

            // Message content
            Style msg_style = Style{};
            if (fading) msg_style = msg_style.with_dim();

            auto card = (dsl::v(Element{TextElement{
                        .content = toast.message,
                        .style = msg_style,
                    }})
                | dsl::border(BorderStyle::Round)
                | dsl::bcolor(fading ? Color::bright_black() : border_color)
                | dsl::btext(border_label, BorderTextPos::Top, BorderTextAlign::Start)
                | dsl::padding(0, 1, 0, 1)).build();

            cards.push_back(std::move(card));
        }

        Element stack = dsl::v(std::move(cards)).build();
        if (cfg_.max_width <= 0) return stack;
        return detail::clamp(std::move(stack), cfg_.max_width,
                             cfg_.right_align ? HAlign::Right
                                              : HAlign::Center);
    }
};

} // namespace maya
