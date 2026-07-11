#pragma once
// maya::widget::breadcrumb — Path/context breadcrumb trail
//
// Passive display widget showing a navigation path with styled segments.
// Last segment is highlighted as the current location.
//
// Responsive via maya's pick() (ViewThatFits): the full trail renders while
// its measured width fits; a narrow slot collapses the middle to an ellipsis
// (first › … › last); the tightest slot shows just the current segment.
//
// Usage:
//   Breadcrumb bc({"src", "widget", "breadcrumb.hpp"});
//   auto ui = v(bc, content);

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../element/builder.hpp"
#include "../style/style.hpp"

namespace maya {

class Breadcrumb {
    std::vector<std::string> segments_;

public:
    explicit Breadcrumb(std::vector<std::string> segments)
        : segments_(std::move(segments)) {}

    Breadcrumb(std::initializer_list<std::string_view> segments) {
        segments_.reserve(segments.size());
        for (auto sv : segments) segments_.emplace_back(sv);
    }

    // -- Mutation --
    void set_segments(std::vector<std::string> segs) {
        segments_ = std::move(segs);
    }

    void push(std::string segment) {
        segments_.push_back(std::move(segment));
    }

    void pop() {
        if (!segments_.empty()) segments_.pop_back();
    }

    [[nodiscard]] int size() const { return static_cast<int>(segments_.size()); }

    // -- Node concept: build into Element --
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        if (segments_.empty()) {
            return Element{TextElement{}};
        }

        auto muted_style  = Style{}.with_dim();
        auto active_style = Style{}.with_bold();
        auto sep_style    = muted_style;

        // One trail at a chosen density. Each variant is a single NoWrap
        // TextElement so pick() measures the real styled row.
        auto trail = [&](bool elide_middle, bool last_only) -> Element {
            std::string content;
            std::vector<StyledRun> runs;
            auto add = [&](std::string_view s, const Style& st) {
                runs.push_back(StyledRun{content.size(), s.size(), st});
                content += s;
            };
            const int last = static_cast<int>(segments_.size()) - 1;
            if (last_only) {
                add(segments_[static_cast<size_t>(last)], active_style);
            } else if (elide_middle && last >= 2) {
                add(segments_.front(), muted_style);
                add(" \xe2\x80\xba ", sep_style);           // ›
                add("\xe2\x80\xa6", sep_style);              // …
                add(" \xe2\x80\xba ", sep_style);
                add(segments_[static_cast<size_t>(last)], active_style);
            } else {
                for (int i = 0; i <= last; ++i) {
                    if (i > 0) add(" \xe2\x80\xba ", sep_style);
                    add(segments_[static_cast<size_t>(i)],
                        i == last ? active_style : muted_style);
                }
            }
            return Element{TextElement{
                .content = std::move(content),
                .style = muted_style,
                .wrap = TextWrap::NoWrap,
                .runs = std::move(runs),
            }};
        };

        // Richest that fits: full trail → first › … › last → last only.
        std::vector<Element> alts;
        alts.push_back(trail(false, false));
        if (segments_.size() >= 3) alts.push_back(trail(true, false));
        if (segments_.size() >= 2) alts.push_back(trail(false, true));
        if (alts.size() == 1) return std::move(alts.front());
        return detail::pick(std::move(alts));
    }
};

} // namespace maya
