#pragma once
// maya::widget::ChangeBar — diff change ribbon column (background-free)
//
// The thin column left of the gutter showing per-line VCS change state: added
// (green ▏), modified (amber ▏), deleted (a red ▔ marker between lines). One
// glyph per line, fixed-width to align with a CodeView.
//
// Usage:  ChangeBar b; b.line(Change::None).line(Change::Added).line(Change::Modified)
//                       .line(Change::Deleted);
//         Element ui = b | dsl::width(1);

#include <cstdint>
#include <string>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

enum class Change : uint8_t { None, Added, Modified, Deleted };

struct ChangeBarTheme {
    Color added    = Color::hex(0x3FB950);
    Color modified = Color::hex(0xD29922);
    Color deleted  = Color::hex(0xF85149);
};

class ChangeBar {
public:
    ChangeBar& line(Change c) { marks_.push_back(c); return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> rows;
        rows.reserve(marks_.size());
        for (Change c : marks_) rows.push_back(row(c));
        return dsl::v(std::move(rows)).build();
    }

private:
    std::vector<Change> marks_;
    ChangeBarTheme      theme;

    Element row(Change c) const {
        const char* g = " "; Color col = theme.added;
        switch (c) {
            case Change::Added:    g = "\xe2\x96\x8f"; col = theme.added;    break; // ▏
            case Change::Modified: g = "\xe2\x96\x8f"; col = theme.modified; break; // ▏
            case Change::Deleted:  g = "\xe2\x96\x94"; col = theme.deleted;  break; // ▔
            default: break;
        }
        std::vector<StyledRun> r{ {0, std::string(g).size(),
            c == Change::None ? Style{} : Style{}.with_fg(col).with_bold()} };
        return Element{TextElement{ .content = g, .style = Style{},
                                    .wrap = TextWrap::NoWrap, .runs = std::move(r) }};
    }
};

} // namespace maya
