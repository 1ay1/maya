#pragma once
// maya::widget::statusbar — Single-line status bar with sections
//
// Usage:
//   StatusBar bar;
//   bar.set_left({" ready ", "main"});
//   bar.set_right({"3 files", "UTF-8"});

#include <string>
#include <string_view>
#include <vector>

#include "../element/builder.hpp"
#include "../style/style.hpp"

namespace maya {

struct StatusBarConfig {
    Style left_style   = Style{};
    Style right_style  = Style{}.with_dim();
    std::string separator = " │ ";
    Style separator_style = Style{}.with_dim();
};

class StatusBar {
    std::vector<std::string> left_;
    std::vector<std::string> right_;
    StatusBarConfig cfg_;

public:
    StatusBar() = default;
    explicit StatusBar(StatusBarConfig cfg) : cfg_(std::move(cfg)) {}

    void set_left(std::vector<std::string> items) { left_ = std::move(items); }
    void set_right(std::vector<std::string> items) { right_ = std::move(items); }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> parts;

        // Left section
        for (size_t i = 0; i < left_.size(); ++i) {
            if (i > 0) {
                parts.push_back(Element{TextElement{
                    .content = cfg_.separator, .style = cfg_.separator_style}});
            }
            parts.push_back(Element{TextElement{
                .content = left_[i], .style = cfg_.left_style}});
        }

        // Spacer
        parts.push_back(detail::box().grow(1));

        // Right section
        for (size_t i = 0; i < right_.size(); ++i) {
            if (i > 0) {
                parts.push_back(Element{TextElement{
                    .content = cfg_.separator, .style = cfg_.separator_style}});
            }
            parts.push_back(Element{TextElement{
                .content = right_[i], .style = cfg_.right_style}});
        }

        return detail::hstack()(std::move(parts));
    }
};

} // namespace maya
