#pragma once
// maya::widget::MergeConflict — three-way merge conflict block (background-free)
//
// Renders a conflict hunk the way editors do: an "ours" section, a divider, a
// "theirs" section, each with a coloured banner (accept-current / accept-
// incoming / accept-both hints) and the code lines tinted by side.
//
// Usage:
//   MergeConflict c;
//   c.ours("HEAD", {"  return left->at(i);"})
//    .theirs("feature", {"  return left ? left->at(i) : text[i];"});
//   Element ui = c;

#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct MergeConflictTheme {
    Color ours    = Color::hex(0xA6E3A1); // current change
    Color theirs  = Color::hex(0x89B4FA); // incoming change
    Color banner  = Color::hex(0x585B70);
    Color code     = Color::hex(0xBAC2DE);
};

struct MergeConflict {
    std::string ours_label = "HEAD", theirs_label = "incoming";
    std::vector<std::string> ours_lines, theirs_lines;
    MergeConflictTheme theme;

    MergeConflict& ours(std::string label, std::vector<std::string> lines) {
        ours_label = std::move(label); ours_lines = std::move(lines); return *this;
    }
    MergeConflict& theirs(std::string label, std::vector<std::string> lines) {
        theirs_label = std::move(label); theirs_lines = std::move(lines); return *this;
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> rows;
        rows.push_back(banner("\xe2\x96\xb2\xe2\x96\xb2\xe2\x96\xb2 <<<<<<< " + ours_label +
                              "  (Accept Current)", theme.ours));
        for (auto& l : ours_lines) rows.push_back(code(l, theme.ours));
        rows.push_back(banner("======= (Accept Both)", theme.banner));
        for (auto& l : theirs_lines) rows.push_back(code(l, theme.theirs));
        rows.push_back(banner("\xe2\x96\xbc\xe2\x96\xbc\xe2\x96\xbc >>>>>>> " + theirs_label +
                              "  (Accept Incoming)", theme.theirs));
        return dsl::v(std::move(rows)).build();
    }

private:
    Element banner(std::string t, Color c) const {
        return Element{TextElement{ .content = std::move(t),
                                    .style = Style{}.with_fg(c).with_bold(),
                                    .wrap = TextWrap::NoWrap }};
    }
    Element code(const std::string& l, Color c) const {
        std::string s = "\xe2\x96\x8e " + l; // ▎ side rail
        std::vector<StyledRun> r{ {0, 4, Style{}.with_fg(c)},
                                  {4, s.size() - 4, Style{}.with_fg(theme.code)} };
        return Element{TextElement{ .content = std::move(s), .style = Style{},
                                    .wrap = TextWrap::NoWrap, .runs = std::move(r) }};
    }
};

} // namespace maya
