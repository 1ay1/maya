#pragma once
// maya::widget::BlameHeatmap — line-age recency column (background-free)
//
// A thin column colouring each line by the age of its last change: hot (recent
// — warm accent) fading to cold (old — muted blue/grey). The git "heat" gutter.
// Fixed-width, aligns with a CodeView.
//
// Usage:
//   BlameHeatmap h; h.line(0).line(2).line(30).line(400);   // age in days
//   Element ui = h | dsl::width(1);

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

class BlameHeatmap {
public:
    BlameHeatmap& line(int age_days) { ages_.push_back(age_days); return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> rows;
        rows.reserve(ages_.size());
        for (int a : ages_) rows.push_back(row(a));
        return dsl::v(std::move(rows)).build();
    }

private:
    std::vector<int> ages_;

    // 5-stop ramp: recent → old.
    static Color heat(int days) {
        static const uint32_t ramp[] = {
            0xF9A03F, // < 1 week: warm orange
            0xE2B341, // < 1 month: amber
            0x94C973, // < 6 months: green
            0x5E9CC4, // < 2 years: blue
            0x45566A, // older: muted slate
        };
        int i = days < 7 ? 0 : days < 31 ? 1 : days < 183 ? 2 : days < 730 ? 3 : 4;
        uint32_t h = ramp[i];
        return Color::rgb((h>>16)&0xFF, (h>>8)&0xFF, h&0xFF);
    }

    Element row(int age) const {
        std::vector<StyledRun> r{ {0, 3, Style{}.with_fg(heat(age))} };
        return Element{TextElement{ .content = "\xe2\x96\x8e", // ▎
                                    .style = Style{}, .wrap = TextWrap::NoWrap,
                                    .runs = std::move(r) }};
    }
};

} // namespace maya
