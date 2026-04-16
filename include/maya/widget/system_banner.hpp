#pragma once
// maya::widget::system_banner — System-level notification banner
//
// Displays system alerts: context window warnings, rate limits,
// connection issues, announcements. Color-coded by severity.
//
//   SystemBanner banner("Context window is 85% full", BannerLevel::Warning);
//   auto ui = banner.build();

#include <cstdint>
#include <string>

#include "../dsl.hpp"
#include "../style/style.hpp"

namespace maya {

enum class BannerLevel : uint8_t {
    Info,
    Success,
    Warning,
    Error,
};

class SystemBanner {
    std::string message_;
    BannerLevel level_ = BannerLevel::Info;
    bool dismissable_  = false;
    bool dismissed_    = false;

    [[nodiscard]] Color level_color() const {
        switch (level_) {
            case BannerLevel::Info:    return Color::blue();
            case BannerLevel::Success: return Color::green();
            case BannerLevel::Warning: return Color::yellow();
            case BannerLevel::Error:   return Color::red();
        }
        return Color::blue();
    }

    [[nodiscard]] const char* level_icon() const {
        switch (level_) {
            case BannerLevel::Info:    return "\xe2\x93\x98"; // ⓘ
            case BannerLevel::Success: return "\xe2\x9c\x93"; // ✓
            case BannerLevel::Warning: return "\xe2\x9a\xa0"; // ⚠
            case BannerLevel::Error:   return "\xe2\x9c\x97"; // ✗
        }
        return "\xe2\x93\x98";
    }

public:
    SystemBanner() = default;
    SystemBanner(std::string message, BannerLevel level = BannerLevel::Info)
        : message_(std::move(message)), level_(level) {}

    void set_message(std::string m) { message_ = std::move(m); }
    void set_level(BannerLevel l) { level_ = l; }
    void set_dismissable(bool b) { dismissable_ = b; }
    void dismiss() { dismissed_ = true; }
    [[nodiscard]] bool is_dismissed() const { return dismissed_; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        if (dismissed_ || message_.empty()) return text("");

        Color lc = level_color();
        auto line_style = Style{}.with_fg(lc).with_dim();

        // Horizontal rule lines
        std::string rule;
        for (int i = 0; i < 3; ++i) rule += "\xe2\x94\x80"; // ─

        std::vector<Element> parts;
        parts.push_back(text(rule + " ", line_style));
        parts.push_back(text(level_icon(), Style{}.with_fg(lc)));
        parts.push_back(text(" " + message_, Style{}.with_fg(lc)));

        if (dismissable_) {
            parts.push_back(text("  [esc]", Style{}.with_dim()));
        }

        parts.push_back(text(" " + rule, line_style));

        return h(std::move(parts)).build();
    }
};

} // namespace maya
