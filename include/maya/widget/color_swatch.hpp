#pragma once
// maya::widget::ColorSwatch — colour preview list (background-free)
//
// A list of colour swatches: a filled block in the colour, its name, and its
// hex. Handy for theme editors and inline #rrggbb decorators.
//
// Usage:  ColorSwatch s; s.color("mauve", 0xCBA6F7).color("green", 0xA6E3A1);

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

struct ColorSwatch {
    struct Row { std::string name; uint32_t hex; };
    std::vector<Row> rows;
    Color            name_col = Color::hex(0xBAC2DE);
    Color            hex_col  = Color::hex(0x585B70);

    ColorSwatch& color(std::string name, uint32_t hex) {
        rows.push_back({std::move(name), hex}); return *this;
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> out;
        for (const auto& r : rows) {
            std::string s; std::vector<StyledRun> runs;
            auto put = [&](std::string_view t, Style st){ if(t.empty())return;
                runs.push_back({s.size(), t.size(), st}); s += t; };
            put("\xe2\x96\x88\xe2\x96\x88 ", Style{}.with_fg(Color::rgb(
                (r.hex >> 16) & 0xFF, (r.hex >> 8) & 0xFF, r.hex & 0xFF))); // ██
            put(r.name, Style{}.with_fg(name_col));
            char buf[10]; std::snprintf(buf, sizeof buf, " #%06X", r.hex & 0xFFFFFF);
            put(buf, Style{}.with_fg(hex_col));
            out.push_back(Element{TextElement{ .content = std::move(s), .style = Style{},
                                               .wrap = TextWrap::NoWrap, .runs = std::move(runs) }});
        }
        return dsl::v(std::move(out)).build();
    }
};

} // namespace maya
