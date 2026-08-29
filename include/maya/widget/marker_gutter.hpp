#pragma once
// maya::widget::MarkerGutter — breakpoint / bookmark / execution column (bg-free)
//
// The narrow column left of the line numbers that shows debugger and editor
// markers per line: breakpoints (● red), disabled breakpoints (○), conditional
// breakpoints (◆), bookmarks () and the current execution line (▶ yellow).
// Fixed width so it aligns 1:1 with a CodeView.
//
// Usage:
//   MarkerGutter g;
//   g.line(Marker::None).line(Marker::Breakpoint).line(Marker::Execution)
//    .line(Marker::Bookmark);
//   Element ui = g | dsl::width(2);

#include <cstdint>
#include <string>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

enum class Marker : uint8_t {
    None, Breakpoint, BreakpointDisabled, ConditionalBreakpoint, Bookmark, Execution,
};

struct MarkerGutterTheme {
    Color breakpoint = Color::hex(0xF85149);
    Color disabled   = Color::hex(0x6C7086);
    Color conditional = Color::hex(0xE2B341);
    Color bookmark    = Color::hex(0x89B4FA);
    Color execution   = Color::hex(0xF9E2AF);
};

class MarkerGutter {
public:
    MarkerGutter& line(Marker m) { marks_.push_back(m); return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> rows;
        rows.reserve(marks_.size());
        for (Marker m : marks_) rows.push_back(row(m));
        return dsl::v(std::move(rows)).build();
    }

private:
    std::vector<Marker> marks_;
    MarkerGutterTheme   theme;

    Element row(Marker m) const {
        const char* g = " ";
        Color c = theme.disabled;
        switch (m) {
            case Marker::Breakpoint:            g = "\xe2\x97\x8f"; c = theme.breakpoint; break; // ●
            case Marker::BreakpointDisabled:    g = "\xe2\x97\x8b"; c = theme.disabled;   break; // ○
            case Marker::ConditionalBreakpoint: g = "\xe2\x97\x86"; c = theme.conditional;break; // ◆
            case Marker::Bookmark:              g = "\xef\x80\xac"; c = theme.bookmark;   break; // 
            case Marker::Execution:             g = "\xe2\x96\xb6"; c = theme.execution;  break; // ▶
            default: break;
        }
        std::string s = std::string(" ") + g;
        std::vector<StyledRun> r{ {0, 1, Style{}}, {1, std::string(g).size(),
                                   Style{}.with_fg(c).with_bold()} };
        return Element{TextElement{ .content = std::move(s), .style = Style{},
                                    .wrap = TextWrap::NoWrap, .runs = std::move(r) }};
    }
};

} // namespace maya
