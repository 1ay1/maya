#pragma once
// maya::widget::CommentBlock — doc-comment renderer (background-free)
//
// Renders a documentation comment block with tag highlighting: the comment body
// is muted green/italic, doc tags (@param, @return, @brief, @tparam, …) are
// accented, the parameter name after @param is emphasised, and `inline code`
// spans are tinted. Good for hover cards and doc panels.
//
// Usage:
//   CommentBlock c;
//   c.line("/// @brief Random access into the rope.")
//    .line("/// @param i  flat index (0-based)")
//    .line("/// @return the character at `i`");
//   Element ui = c;

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct CommentBlockTheme {
    Color body  = Color::hex(0x7F9C6E); // muted comment green
    Color tag   = Color::hex(0xCBA6F7); // @param, @return
    Color param = Color::hex(0xFAB387); // the param name
    Color code  = Color::hex(0x94E2D5); // `inline code`
};

struct CommentBlock {
    std::vector<std::string> lines;
    CommentBlockTheme        theme;

    CommentBlock& line(std::string l) { lines.push_back(std::move(l)); return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> out;
        for (const auto& l : lines) out.push_back(row(l));
        return dsl::v(std::move(out)).build();
    }

private:
    Element row(const std::string& l) const {
        std::string s; std::vector<StyledRun> r;
        auto put=[&](std::string_view t, Style st){ if(t.empty())return;
            r.push_back({s.size(),t.size(),st}); s+=t; };
        size_t i = 0;
        bool after_param = false;
        while (i < l.size()) {
            if (l[i] == '@') {                     // doc tag
                size_t j = i + 1;
                while (j < l.size() && (std::isalpha((unsigned char)l[j]))) ++j;
                std::string tag = l.substr(i, j - i);
                put(tag, Style{}.with_fg(theme.tag).with_bold());
                after_param = (tag == "@param" || tag == "@tparam");
                i = j;
            } else if (l[i] == '`') {              // inline code
                size_t j = l.find('`', i + 1);
                if (j == std::string::npos) j = l.size() - 1;
                put(l.substr(i, j - i + 1), Style{}.with_fg(theme.code));
                i = j + 1;
            } else if (after_param && !std::isspace((unsigned char)l[i])) {
                size_t j = i;
                while (j < l.size() && !std::isspace((unsigned char)l[j])) ++j;
                put(l.substr(i, j - i), Style{}.with_fg(theme.param).with_bold());
                after_param = false;
                i = j;
            } else {                               // body text
                size_t j = i;
                while (j < l.size() && l[j] != '@' && l[j] != '`') {
                    if (after_param && !std::isspace((unsigned char)l[j])) break;
                    if (!std::isspace((unsigned char)l[j])) after_param = false;
                    ++j;
                }
                if (j == i) ++j;
                put(l.substr(i, j - i), Style{}.with_fg(theme.body).with_italic());
                i = j;
            }
        }
        return Element{TextElement{ .content=std::move(s), .style=Style{},
                                    .wrap=TextWrap::NoWrap, .runs=std::move(r) }};
    }
};

} // namespace maya
