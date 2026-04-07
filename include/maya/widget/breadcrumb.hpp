#pragma once
// maya::widget::breadcrumb — Navigational context path display
//
// Usage:
//   Breadcrumb bc({"project", "src", "main.cpp"});
//   auto ui = h(bc, text(" - editing"));

#include <string>
#include <string_view>
#include <vector>

#include "../element/builder.hpp"
#include "../style/style.hpp"

namespace maya {

struct BreadcrumbConfig {
    std::string separator  = " › ";
    Style separator_style  = Style{}.with_dim();
    Style segment_style    = Style{}.with_fg(Color::rgb(180, 180, 200));
    Style active_style     = Style{}.with_bold().with_fg(Color::rgb(100, 200, 255));
    bool highlight_last    = true;
};

class Breadcrumb {
    std::vector<std::string> segments_;
    BreadcrumbConfig cfg_;

public:
    explicit Breadcrumb(std::vector<std::string> segments, BreadcrumbConfig cfg = {})
        : segments_(std::move(segments)), cfg_(std::move(cfg)) {}

    Breadcrumb(std::initializer_list<std::string_view> segments, BreadcrumbConfig cfg = {})
        : cfg_(std::move(cfg))
    {
        segments_.reserve(segments.size());
        for (auto sv : segments) segments_.emplace_back(sv);
    }

    void set_segments(std::vector<std::string> segs) { segments_ = std::move(segs); }
    void push(std::string_view seg) { segments_.emplace_back(seg); }
    void pop() { if (!segments_.empty()) segments_.pop_back(); }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        if (segments_.empty()) return Element{TextElement{}};

        std::vector<Element> parts;
        parts.reserve(segments_.size() * 2);

        for (size_t i = 0; i < segments_.size(); ++i) {
            if (i > 0) {
                parts.push_back(Element{TextElement{
                    .content = cfg_.separator,
                    .style = cfg_.separator_style,
                }});
            }

            bool is_last = (i == segments_.size() - 1);
            Style s = (is_last && cfg_.highlight_last) ? cfg_.active_style : cfg_.segment_style;

            parts.push_back(Element{TextElement{
                .content = segments_[i],
                .style = s,
            }});
        }

        return detail::hstack()(std::move(parts));
    }
};

} // namespace maya
