#pragma once
// maya::widget::IndentScopeGuides — active indent-scope highlight (background-free)
//
// Renders a block of code with indent guides (│) at each level, highlighting
// the guide of the *active scope* (the block the cursor is in) across its line
// range — the "highlight active indent guide" feature.
//
// Usage:
//   IndentScopeGuides g;
//   g.line("void f() {",        0)
//    .line("    if (x) {",      1)
//    .line("        do_a();",   2)
//    .line("        do_b();",   2)
//    .line("    }",             1)
//    .line("}",                 0)
//    .active(/*depth=*/2, /*from=*/2, /*to=*/3);
//   Element ui = g;

#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct IndentScopeTheme {
    Color code   = Color::hex(0xC9D1D9);
    Color guide  = Color::hex(0x2A2F3A);
    Color active = Color::hex(0x89B4FA);
    int   width  = 4; // columns per indent level
};

class IndentScopeGuides {
public:
    IndentScopeGuides& line(std::string code, int depth) {
        rows_.push_back({std::move(code), depth}); return *this;
    }
    IndentScopeGuides& active(int depth, int from, int to) {
        adepth_ = depth; afrom_ = from; ato_ = to; return *this;
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> out;
        for (size_t i = 0; i < rows_.size(); ++i) out.push_back(row(rows_[i], static_cast<int>(i)));
        return dsl::v(std::move(out)).build();
    }

private:
    struct L { std::string code; int depth; };
    std::vector<L> rows_;
    int            adepth_ = -1, afrom_ = 0, ato_ = 0;
    IndentScopeTheme theme;

    Element row(const L& l, int idx) const {
        std::string s; std::vector<StyledRun> r;
        auto put=[&](std::string_view t, Style st){ if(t.empty())return;
            r.push_back({s.size(),t.size(),st}); s+=t; };
        const bool in_active = (idx >= afrom_ && idx <= ato_);
        // strip the code's own leading spaces; we draw guides instead
        std::string_view code{l.code};
        size_t lead = code.find_first_not_of(' ');
        if (lead == std::string_view::npos) lead = code.size();
        code.remove_prefix(lead);

        for (int d = 0; d < l.depth; ++d) {
            bool hot = (in_active && d == adepth_ - 1 && adepth_ > 0);
            put("\xe2\x94\x82", Style{}.with_fg(hot ? theme.active : theme.guide)); // │
            for (int k = 1; k < theme.width; ++k) put(" ", Style{});
        }
        put(code, Style{}.with_fg(theme.code));
        return Element{TextElement{ .content=std::move(s), .style=Style{},
                                    .wrap=TextWrap::NoWrap, .runs=std::move(r) }};
    }
};

} // namespace maya
