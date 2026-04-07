#pragma once
// maya::widget::toast — Temporary notification messages
//
// Usage:
//   ToastManager toasts;
//   toasts.push("File saved", ToastLevel::Success);
//   toasts.advance(dt);  // call each frame
//   auto ui = v(main_content, toasts);

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "../element/builder.hpp"
#include "../style/style.hpp"

namespace maya {

enum class ToastLevel : uint8_t { Info, Success, Warning, Error };

struct ToastConfig {
    float duration    = 3.0f;
    float fade_time   = 0.5f;
    int max_visible   = 3;
};

class ToastManager {
    struct Item {
        std::string message;
        ToastLevel level;
        float remaining;
    };

    std::vector<Item> toasts_;
    ToastConfig cfg_;

public:
    ToastManager() = default;
    explicit ToastManager(ToastConfig cfg) : cfg_(cfg) {}

    void push(std::string_view message, ToastLevel level = ToastLevel::Info) {
        push(message, level, cfg_.duration);
    }

    void push(std::string_view message, ToastLevel level, float duration) {
        toasts_.push_back({std::string{message}, level, duration});
    }

    void advance(float dt) {
        for (auto& t : toasts_) t.remaining -= dt;
        std::erase_if(toasts_, [](const Item& t) { return t.remaining <= 0.0f; });
    }

    void clear() { toasts_.clear(); }
    [[nodiscard]] bool empty() const { return toasts_.empty(); }
    [[nodiscard]] int count() const { return static_cast<int>(toasts_.size()); }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        if (toasts_.empty()) return Element{TextElement{}};

        int show = std::min(cfg_.max_visible, static_cast<int>(toasts_.size()));
        int start = static_cast<int>(toasts_.size()) - show;

        std::vector<Element> rows;
        rows.reserve(static_cast<size_t>(show));

        for (int i = start; i < static_cast<int>(toasts_.size()); ++i) {
            const auto& t = toasts_[static_cast<size_t>(i)];
            bool fading = t.remaining < cfg_.fade_time;

            const char* icon = "ℹ";
            Style s;
            switch (t.level) {
                case ToastLevel::Info:
                    icon = "ℹ"; s = Style{}.with_fg(Color::rgb(100, 180, 255)); break;
                case ToastLevel::Success:
                    icon = "✓"; s = Style{}.with_fg(Color::rgb(80, 220, 120)); break;
                case ToastLevel::Warning:
                    icon = "⚠"; s = Style{}.with_fg(Color::rgb(255, 200, 60)); break;
                case ToastLevel::Error:
                    icon = "✗"; s = Style{}.with_fg(Color::rgb(255, 80, 80)); break;
            }
            if (fading) s = s.with_dim();

            rows.push_back(detail::hstack()(
                Element{TextElement{.content = std::string{icon} + " ", .style = s}},
                Element{TextElement{.content = t.message, .style = fading ? Style{}.with_dim() : Style{}}}
            ));
        }

        return detail::vstack()(std::move(rows));
    }
};

} // namespace maya
