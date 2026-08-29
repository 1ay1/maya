#pragma once
// maya::widget::WordCount — document statistics readout (background-free)
//
// A one-line document stats strip: words, characters, lines, and an estimated
// reading time — the writer's status readout. Also exposes a static counter so
// you can feed it raw text.
//
// Usage:
//   WordCount w = WordCount::of(text);   or   WordCount{}.words(1200).chars(6800).lines(94);
//   Element ui = w;

#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct WordCountTheme {
    Color num   = Color::hex(0xE6EDF3);
    Color label = Color::hex(0x7F849C);
    Color sep    = Color::hex(0x45475A);
};

struct WordCount {
    int words_ = 0, chars_ = 0, lines_ = 0;
    int wpm_ = 200;
    WordCountTheme theme;

    WordCount& words(int n) { words_ = n; return *this; }
    WordCount& chars(int n) { chars_ = n; return *this; }
    WordCount& lines(int n) { lines_ = n; return *this; }

    static WordCount of(std::string_view text) {
        WordCount w; bool in = false;
        for (char c : text) {
            ++w.chars_;
            if (c == '\n') ++w.lines_;
            bool sp = (c==' '||c=='\t'||c=='\n'||c=='\r');
            if (!sp && !in) { ++w.words_; in = true; }
            else if (sp) in = false;
        }
        if (!text.empty() && text.back() != '\n') ++w.lines_;
        return w;
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::string s; std::vector<StyledRun> r;
        auto put=[&](std::string_view t, Style st){ if(t.empty())return;
            r.push_back({s.size(),t.size(),st}); s+=t; };
        auto stat=[&](int n, std::string_view label, bool sep){
            if (sep) put("   \xc2\xb7   ", Style{}.with_fg(theme.sep)); // ·
            put(std::to_string(n), Style{}.with_fg(theme.num).with_bold());
            put(" " + std::string(label), Style{}.with_fg(theme.label));
        };
        stat(words_, "words", false);
        stat(chars_, "chars", true);
        stat(lines_, "lines", true);
        int mins = (wpm_ > 0) ? (words_ + wpm_ - 1) / wpm_ : 0;
        put("   \xc2\xb7   ", Style{}.with_fg(theme.sep));
        put("~" + std::to_string(mins), Style{}.with_fg(theme.num).with_bold());
        put(" min read", Style{}.with_fg(theme.label));
        return Element{TextElement{ .content=std::move(s), .style=Style{},
                                    .wrap=TextWrap::NoWrap, .runs=std::move(r) }};
    }
};

} // namespace maya
