#pragma once
// maya::widget::GitLensInline — end-of-line blame annotation (background-free)
//
// A code line with a trailing GitLens-style blame annotation: the code, then a
// gap, then a dim "author, when · summary". Foreground-only.
//
// Usage:  GitLensInline{"    return text[i - weight];"}
//             .blame("Ada Lovelace", "3 days ago", "fix rope leaf access");

#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct GitLensTheme {
    Color code  = Color::hex(0xC9D1D9);
    Color blame = Color::hex(0x585B70);
};

struct GitLensInline {
    std::string code_, author_, when_, summary_;
    GitLensTheme theme;

    explicit GitLensInline(std::string code = {}) : code_(std::move(code)) {}
    GitLensInline& blame(std::string author, std::string when, std::string summary) {
        author_ = std::move(author); when_ = std::move(when); summary_ = std::move(summary);
        return *this;
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::string ann;
        if (!author_.empty()) {
            ann = author_ + ", " + when_;
            if (!summary_.empty()) ann += " \xc2\xb7 " + summary_; // ·
        }
        std::string code = code_;
        return Element{ComponentElement{
            .render = [code = std::move(code), ann = std::move(ann),
                       cc = theme.code, bc = theme.blame](int w, int) -> Element {
                std::string s = code; std::vector<StyledRun> r;
                r.push_back({0, code.size(), Style{}.with_fg(cc)});
                if (!ann.empty()) {
                    int used = string_width(s);
                    int gap = std::max(4, w - used - string_width(ann));
                    if (used + gap + string_width(ann) > w) gap = 4; // just inline if tight
                    r.push_back({s.size(), (size_t)gap, Style{}}); s.append((size_t)gap, ' ');
                    r.push_back({s.size(), ann.size(), Style{}.with_fg(bc).with_italic()}); s += ann;
                }
                return Element{TextElement{ .content = std::move(s), .style = Style{},
                                            .wrap = TextWrap::NoWrap, .runs = std::move(r) }};
            },
            .measure = [](int mw) -> Size { return Size{Columns(mw), Rows(1)}; },
        }};
    }
};

} // namespace maya
