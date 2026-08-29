#pragma once
// maya::widget::EditorStatusLine — status bar (background-free)
//
// A full-width editor status line built from coloured segments. It keeps the
// terminal's own background and separates segments with thin nerd-font
// dividers ( / , U+E0B1 / U+E0B3) drawn in a dim foreground — so it reads
// as a crisp status strip without ever painting a cell background. Left
// segments carry mode / git / file; right segments carry language / position
// / encoding, and the middle stretches to fill the width.
//
// Usage:
//   EditorStatusLine sl;
//   sl.left(EditorStatusLine::mode("NORMAL"))
//     .left(EditorStatusLine::branch("main"))
//     .left(EditorStatusLine::file("src/main.cpp", true))
//     .right(EditorStatusLine::lang("C++")).right(EditorStatusLine::pos(42, 7, 128));
//   Element ui = sl;

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

struct EditorStatusLine {
    struct Seg {
        std::string text;
        Color       fg;
        bool        bold = false;
    };

    struct Config {
        Color divider = Color::hex(0x585B70); // dim separator glyph
    };

    Config           config{};
    std::vector<Seg> left_;
    std::vector<Seg> right_;

    EditorStatusLine() = default;
    explicit EditorStatusLine(Config c) : config(c) {}

    EditorStatusLine& left(Seg s)  { left_.push_back(std::move(s));  return *this; }
    EditorStatusLine& right(Seg s) { right_.push_back(std::move(s)); return *this; }

    // ── Ready-made segment presets ──────────────────────────────────────────
    static Seg mode(std::string_view m) {
        Color accent = Color::hex(0x89B4FA);
        if (m == "INSERT") accent = Color::hex(0xA6E3A1);
        else if (m == "VISUAL") accent = Color::hex(0xF9E2AF);
        else if (m == "REPLACE" || m == "COMMAND") accent = Color::hex(0xF38BA8);
        return Seg{"\xef\x84\xa0 " + std::string(m), accent, true}; //  circle
    }
    static Seg branch(std::string_view name) {
        return Seg{"\xee\x82\xa0 " + std::string(name), Color::hex(0xA6E3A1)}; //  git
    }
    static Seg file(std::string_view path, bool dirty = false) {
        return Seg{std::string(path) + (dirty ? " \xe2\x97\x8f" : ""), Color::hex(0xCDD6F4)};
    }
    static Seg lang(std::string_view l) {
        return Seg{std::string(l), Color::hex(0xCBA6F7)};
    }
    static Seg pos(int line, int col, int total) {
        char buf[64];
        int pct = total > 0 ? (line * 100) / total : 0;
        std::snprintf(buf, sizeof buf, "\xef\x82\x9b %d:%d  %d%%", line, col, pct); // 
        return Seg{buf, Color::hex(0x89B4FA), true};
    }
    static Seg info(std::string_view text) {
        return Seg{std::string(text), Color::hex(0xBAC2DE)};
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        // U+E0B1  (right thin) / U+E0B3  (left thin)
        const std::string sep_r = " \xee\x82\xb1 ";
        const std::string sep_l = " \xee\x82\xb3 ";
        const Style div = Style{}.with_fg(config.divider);

        auto put = [](std::string& s, std::vector<StyledRun>& runs,
                      std::string_view t, Style st) {
            if (t.empty()) return;
            runs.push_back({s.size(), t.size(), st});
            s += t;
        };
        auto seg_style = [](const Seg& g) {
            Style st = Style{}.with_fg(g.fg);
            return g.bold ? st.with_bold() : st;
        };

        std::string ls; std::vector<StyledRun> lr;
        for (size_t i = 0; i < left_.size(); ++i) {
            if (i) put(ls, lr, sep_r, div);
            put(ls, lr, left_[i].text, seg_style(left_[i]));
        }

        std::string rs; std::vector<StyledRun> rr;
        for (size_t i = 0; i < right_.size(); ++i) {
            if (i) put(rs, rr, sep_l, div);
            put(rs, rr, right_[i].text, seg_style(right_[i]));
        }

        return Element{ComponentElement{
            .render = [ls = std::move(ls), lr = std::move(lr),
                       rs = std::move(rs), rr = std::move(rr)](int w, int) -> Element {
                const int lw = string_width(ls);
                const int rw = string_width(rs);
                const int gap = std::max(1, w - lw - rw - 2);

                std::string content = " ";
                std::vector<StyledRun> runs;
                runs.push_back({0, 1, Style{}});
                const size_t loff = content.size();
                content += ls;
                for (auto r : lr) runs.push_back({loff + r.byte_offset, r.byte_length, r.style});

                runs.push_back({content.size(), static_cast<size_t>(gap), Style{}});
                content.append(static_cast<size_t>(gap), ' ');

                const size_t roff = content.size();
                content += rs;
                for (auto r : rr) runs.push_back({roff + r.byte_offset, r.byte_length, r.style});

                content += " ";
                runs.push_back({content.size() - 1, 1, Style{}});

                return Element{TextElement{
                    .content = std::move(content),
                    .style   = Style{},
                    .wrap    = TextWrap::NoWrap,
                    .runs    = std::move(runs),
                }};
            },
        }};
    }
};

} // namespace maya
