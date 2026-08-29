#pragma once
// maya::widget::RulerGuide — column ruler over code (background-free)
//
// Renders a block of code lines with a faint vertical guide at a fixed column
// (the 80/100-col ruler editors draw). Where a line is shorter than the ruler
// column, a dim │ marks the column; where code occupies it, the code shows.
//
// Usage:  RulerGuide{80}.line("int x = 1;").line("auto really_long = ...");
//         Element ui = r;

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct RulerGuideTheme {
    Color code  = Color::hex(0xC9D1D9);
    Color guide = Color::hex(0x313244);
    Color over  = Color::hex(0xF38BA8); // code past the ruler column
};

struct RulerGuide {
    int                      column = 80;
    std::vector<std::string> lines;
    RulerGuideTheme          theme;

    explicit RulerGuide(int col = 80) : column(col) {}
    RulerGuide& line(std::string l) { lines.push_back(std::move(l)); return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> out;
        for (const auto& l : lines) out.push_back(row(l));
        return dsl::v(std::move(out)).build();
    }

private:
    Element row(const std::string& l) const {
        const int len = string_width(l);
        std::string s = l; std::vector<StyledRun> r;
        if (len > column) {
            // split code at the ruler column; the overflow is tinted
            r.push_back({0, (size_t)column_byte(l), Style{}.with_fg(theme.code)});
            r.push_back({(size_t)column_byte(l), l.size() - column_byte(l), Style{}.with_fg(theme.over)});
        } else {
            r.push_back({0, l.size(), Style{}.with_fg(theme.code)});
            // pad up to the column and drop a faint guide there
            int pad = column - len;
            if (pad > 0) { s.append((size_t)(pad), ' ');
                           r.push_back({l.size(), (size_t)pad, Style{}}); }
            r.push_back({s.size(), 3, Style{}.with_fg(theme.guide)}); s += "\xe2\x94\x82"; // │
        }
        return Element{TextElement{ .content=std::move(s), .style=Style{},
                                    .wrap=TextWrap::NoWrap, .runs=std::move(r) }};
    }
    // byte offset of the ruler column (ASCII assumption for the split point)
    int column_byte(const std::string& l) const {
        return column < static_cast<int>(l.size()) ? column : static_cast<int>(l.size());
    }
};

} // namespace maya
