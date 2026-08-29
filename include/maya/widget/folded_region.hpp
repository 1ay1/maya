#pragma once
// maya::widget::FoldedRegion — collapsed code region (background-free)
//
// The folded-region placeholder editors show for a collapsed block: the fold
// header line, a "⋯" glyph, and a dim "N lines" count in a subtle pill, with an
// expand chevron.
//
// Usage:  FoldedRegion{"struct Rope {"}.hidden(14);

#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct FoldedRegionTheme {
    Color chevron = Color::hex(0x89B4FA);
    Color header  = Color::hex(0xC9D1D9);
    Color ellipsis = Color::hex(0x585B70);
    Color count     = Color::hex(0x6C7086);
    Color edge       = Color::hex(0x45475A);
};

struct FoldedRegion {
    std::string header_;
    int         hidden_ = 0;
    FoldedRegionTheme theme;

    explicit FoldedRegion(std::string header = {}) : header_(std::move(header)) {}
    FoldedRegion& hidden(int n) { hidden_ = n; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::string s; std::vector<StyledRun> r;
        auto put=[&](std::string_view t, Style st){ if(t.empty())return;
            r.push_back({s.size(),t.size(),st}); s+=t; };
        put("\xef\x84\x85 ", Style{}.with_fg(theme.chevron)); //  chevron-right (collapsed)
        put(header_, Style{}.with_fg(theme.header));
        put("  ", Style{});
        put("\xe2\x9d\xb4 ", Style{}.with_fg(theme.edge));     // ❴
        put("\xe2\x8b\xaf ", Style{}.with_fg(theme.ellipsis)); // ⋯
        put(std::to_string(hidden_) + " lines", Style{}.with_fg(theme.count));
        put(" \xe2\x9d\xb5", Style{}.with_fg(theme.edge));     // ❵
        return Element{TextElement{ .content=std::move(s), .style=Style{},
                                    .wrap=TextWrap::NoWrap, .runs=std::move(r) }};
    }
};

} // namespace maya
