#pragma once
// maya::widget::ColorPickerGrid — palette swatch grid (background-free)
//
// A grid of colour swatches for a theme/colour picker: filled blocks laid out
// in rows, with the selected swatch bracketed by an accent frame and its hex
// shown below.
//
// Usage:
//   ColorPickerGrid g; g.color(0xF38BA8).color(0xFAB387)...; g.columns(8).select(2);
//   Element ui = g;

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct ColorPickerGrid {
    std::vector<uint32_t> colors;
    int                   cols = 8;
    int                   selected_ = 0;
    Color                 frame = Color::hex(0x89B4FA);
    Color                 hex_col = Color::hex(0x9399B2);

    ColorPickerGrid& color(uint32_t hex) { colors.push_back(hex); return *this; }
    ColorPickerGrid& columns(int c) { cols = c; return *this; }
    ColorPickerGrid& select(int i) { selected_ = i; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> rows;
        const int n = static_cast<int>(colors.size());
        for (int r = 0; r < n; r += cols) {
            std::string s; std::vector<StyledRun> runs;
            auto put=[&](std::string_view t, Style st){ if(t.empty())return;
                runs.push_back({s.size(),t.size(),st}); s+=t; };
            for (int c = 0; c < cols && r + c < n; ++c) {
                int i = r + c;
                uint32_t h = colors[static_cast<size_t>(i)];
                Color col = Color::rgb((h>>16)&0xFF, (h>>8)&0xFF, h&0xFF);
                bool sel = (i == selected_);
                put(sel ? "\xe2\x96\x95" : " ", Style{}.with_fg(frame));  // ▕ left frame
                put("\xe2\x96\x88\xe2\x96\x88", Style{}.with_fg(col));      // ██
                put(sel ? "\xe2\x96\x8f" : " ", Style{}.with_fg(frame));   // ▏ right frame
            }
            rows.push_back(Element{TextElement{ .content=std::move(s), .style=Style{},
                                                .wrap=TextWrap::NoWrap, .runs=std::move(runs) }});
        }
        // selected hex readout
        if (selected_ >= 0 && selected_ < n) {
            char buf[10]; std::snprintf(buf, sizeof buf, "#%06X", colors[static_cast<size_t>(selected_)] & 0xFFFFFF);
            rows.push_back(Element{TextElement{ .content = std::string("  ") + buf,
                                                .style = Style{}.with_fg(hex_col),
                                                .wrap = TextWrap::NoWrap }});
        }
        return dsl::v(std::move(rows)).build();
    }
};

} // namespace maya
