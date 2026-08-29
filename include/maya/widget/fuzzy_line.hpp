#pragma once
// maya::widget::FuzzyLine — highlighted fuzzy-match result row
//
// Renders a candidate string with the characters that matched a query lit up
// in an accent colour (bold), the rest dimmed — exactly the look of a fuzzy
// file finder / command palette result. Includes a tiny subsequence matcher
// so you can go straight from (candidate, query) to a styled row, and an
// optional dim right-aligned hint (e.g. a directory or keybinding).
//
// Foreground-only; the terminal keeps its background.
//
// Usage:
//   auto hits = FuzzyLine::match("src/widget/code_view.hpp", "cvh");
//   if (hits.score >= 0)
//       Element row = FuzzyLine{"src/widget/code_view.hpp", hits.positions}
//                       .hint("widget").icon("\xef\x85\x9b ");   //  file glyph

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct FuzzyLineTheme {
    Color base  = Color::hex(0xBAC2DE); // unmatched text
    Color match = Color::hex(0xF9E2AF); // matched characters
    Color icon  = Color::hex(0x89B4FA); // leading glyph
    Color hint  = Color::hex(0x585B70); // trailing hint
};

struct FuzzyLine {
    struct Result {
        int              score = -1;   // -1 == no match; higher is better
        std::vector<int> positions;    // matched byte offsets into candidate
        explicit operator bool() const { return score >= 0; }
    };

    // Case-insensitive subsequence match. Rewards contiguous runs and matches
    // at word boundaries (after '/', '_', '-', '.', ' ') — a decent default.
    static Result match(std::string_view cand, std::string_view query) {
        Result r;
        if (query.empty()) { r.score = 0; return r; }
        size_t qi = 0;
        int score = 0, streak = 0;
        for (size_t i = 0; i < cand.size() && qi < query.size(); ++i) {
            if (lower(cand[i]) == lower(query[qi])) {
                r.positions.push_back(static_cast<int>(i));
                bool boundary = (i == 0) || is_sep(cand[i - 1]);
                score += 1 + streak * 2 + (boundary ? 4 : 0);
                ++streak; ++qi;
            } else {
                streak = 0;
            }
        }
        if (qi < query.size()) { r.positions.clear(); r.score = -1; return r; }
        r.score = score - static_cast<int>(cand.size()) / 8; // prefer short
        return r;
    }

    std::string      text;
    std::vector<int> matches;
    std::string      icon_;
    std::string      hint_;
    FuzzyLineTheme   theme;

    FuzzyLine(std::string t, std::vector<int> m)
        : text(std::move(t)), matches(std::move(m)) {}

    FuzzyLine& icon(std::string s) { icon_ = std::move(s); return *this; }
    FuzzyLine& hint(std::string s) { hint_ = std::move(s); return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::string      s;
        std::vector<StyledRun> runs;
        auto put = [&](std::string_view t, Style st) {
            if (t.empty()) return;
            runs.push_back({s.size(), t.size(), st});
            s += t;
        };

        if (!icon_.empty()) put(icon_, Style{}.with_fg(theme.icon));

        const Style base  = Style{}.with_fg(theme.base);
        const Style hit   = Style{}.with_fg(theme.match).with_bold();
        size_t mi = 0;
        for (size_t i = 0; i < text.size(); ++i) {
            bool m = (mi < matches.size() && static_cast<int>(i) == matches[mi]);
            if (m) ++mi;
            put(std::string_view{text}.substr(i, 1), m ? hit : base);
        }

        Element left = Element{TextElement{
            .content = std::move(s),
            .style   = base,
            .wrap    = TextWrap::NoWrap,
            .runs    = std::move(runs),
        }};

        if (hint_.empty()) return left;

        // Push the hint to the right edge.
        return dsl::h(
            left,
            dsl::spacer(),
            Element{TextElement{
                .content = hint_,
                .style   = Style{}.with_fg(theme.hint),
                .wrap    = TextWrap::NoWrap,
            }}
        ).build();
    }

private:
    static char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; }
    static bool is_sep(char c) {
        return c == '/' || c == '_' || c == '-' || c == '.' || c == ' ' || c == '\\';
    }
};

} // namespace maya
