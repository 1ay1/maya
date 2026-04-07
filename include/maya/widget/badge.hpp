#pragma once
// maya::widget::badge — Inline styled label tags
//
// Usage:
//   auto tag = Badge("Tool", {.style = Style{}.with_fg(Color::rgb(180,140,255))});
//   auto err = error_badge();
//   auto ok  = success_badge("Done");

#include <string>
#include <string_view>

#include "../element/builder.hpp"
#include "../style/style.hpp"

namespace maya {

struct BadgeConfig {
    Style style           = Style{}.with_bold();
    std::string left_cap  = "[";
    std::string right_cap = "]";
    Style bracket_style   = Style{}.with_dim();
};

class Badge {
    std::string label_;
    BadgeConfig cfg_;

public:
    Badge() = default;
    explicit Badge(std::string_view label, BadgeConfig cfg = {})
        : label_(label), cfg_(cfg) {}

    void set_label(std::string_view l) { label_ = std::string{l}; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        return detail::hstack()(
            Element{TextElement{.content = cfg_.left_cap, .style = cfg_.bracket_style}},
            Element{TextElement{.content = label_, .style = cfg_.style}},
            Element{TextElement{.content = cfg_.right_cap, .style = cfg_.bracket_style}}
        );
    }
};

// ── Preset factories ───────────────────────────────────────────────────────

[[nodiscard]] inline Badge success_badge(std::string_view label = "Success") {
    return Badge(label, {.style = Style{}.with_bold().with_fg(Color::rgb(80, 220, 120))});
}

[[nodiscard]] inline Badge error_badge(std::string_view label = "Error") {
    return Badge(label, {.style = Style{}.with_bold().with_fg(Color::rgb(255, 80, 80))});
}

[[nodiscard]] inline Badge warning_badge(std::string_view label = "Warning") {
    return Badge(label, {.style = Style{}.with_bold().with_fg(Color::rgb(255, 200, 60))});
}

[[nodiscard]] inline Badge info_badge(std::string_view label = "Info") {
    return Badge(label, {.style = Style{}.with_bold().with_fg(Color::rgb(100, 180, 255))});
}

[[nodiscard]] inline Badge tool_badge(std::string_view label = "Tool") {
    return Badge(label, {.style = Style{}.with_bold().with_fg(Color::rgb(180, 140, 255))});
}

} // namespace maya
