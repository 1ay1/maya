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
//   toasts.advance(dt);
//   auto ui = toasts.build();

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "../dsl.hpp"
#include "../style/border.hpp"
#include "../style/style.hpp"

namespace maya {

enum class ToastLevel : uint8_t { Info, Success, Warning, Error };

struct ToastManager {
    struct Toast {
        std::string message;
        ToastLevel level;
        float remaining;
    };

    struct Config {
        float duration    = 3.0f;
        float fade_time   = 0.5f;
        int   max_visible = 3;
    };

private:
    Config cfg_;
    std::vector<Toast> toasts_;

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
        toasts_.push_back({std::move(message), level, cfg_.duration});
    }

    void advance(float dt) {
        for (auto& t : toasts_) t.remaining -= dt;
        toasts_.erase(
            std::remove_if(toasts_.begin(), toasts_.end(),
                [](auto const& t) { return t.remaining <= 0.0f; }),
            toasts_.end());
    }

    [[nodiscard]] bool empty() const { return toasts_.empty(); }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> cards;

        // Show up to max_visible, newest at bottom
        int start = static_cast<int>(toasts_.size()) - cfg_.max_visible;
        if (start < 0) start = 0;

        for (int i = start; i < static_cast<int>(toasts_.size()); ++i) {
            auto const& toast = toasts_[static_cast<size_t>(i)];
            auto info = level_info(toast.level);

            bool fading = toast.remaining < cfg_.fade_time;

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

        return dsl::v(std::move(cards)).build();
    }
};

} // namespace maya
