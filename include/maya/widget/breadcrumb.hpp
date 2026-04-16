#pragma once
// maya::widget::breadcrumb — Path/context breadcrumb trail
//
// Passive display widget showing a navigation path with styled segments.
// Last segment is highlighted as the current location.
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

        auto dim_style = Style{}.with_dim();
        auto muted_style = Style{}.with_dim();
        auto active_style = Style{}.with_bold();
        auto sep_style = dim_style;

        std::string content;
        std::vector<StyledRun> runs;

        int last = static_cast<int>(segments_.size()) - 1;

        for (int i = 0; i <= last; ++i) {
            if (i > 0) {
                std::string sep = " \xe2\x80\xba "; // " › "
                runs.push_back(StyledRun{content.size(), sep.size(), sep_style});
                content += sep;
            }
            const auto& seg = segments_[static_cast<size_t>(i)];
            runs.push_back(StyledRun{
                content.size(), seg.size(),
                (i == last) ? active_style : muted_style,
            });
            content += seg;
        }

        return Element{TextElement{
            .content = std::move(content),
            .style = muted_style,
            .runs = std::move(runs),
        }};
    }
};

} // namespace maya
