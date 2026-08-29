#pragma once
// maya::widget::TerminalPane — integrated terminal / output view (bg-free)
//
// A read-only render of a shell session: a prompt + echoed command, coloured
// output lines (stdout dim, stderr red, info blue, success green), and a live
// input line with a block caret. Foreground-only. Pair it with a scroll
// primitive for history.
//
// Usage:
//   TerminalPane t;
//   t.prompt("~/rope", "cmake --build build")
//    .out("[100%] Built target maya")
//    .ok("Build succeeded in 4.2s")
//    .input("~/rope", "ctest --test-dir build");
//   Element ui = t;

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct TerminalPaneTheme {
    Color prompt  = Color::hex(0xA6E3A1); // ❯
    Color cwd     = Color::hex(0x89B4FA); // path
    Color command = Color::hex(0xE6EDF3); // typed command
    Color out      = Color::hex(0x9399B2); // stdout
    Color err       = Color::hex(0xF38BA8); // stderr
    Color info      = Color::hex(0x89DCEB); // info
    Color ok        = Color::hex(0xA6E3A1); // success
    Color caret     = Color::hex(0xF5F5F7); // block caret
};

class TerminalPane {
public:
    TerminalPane& prompt(std::string cwd, std::string cmd) {
        lines_.push_back({Kind::Prompt, std::move(cwd), std::move(cmd)}); return *this;
    }
    TerminalPane& out(std::string s)  { lines_.push_back({Kind::Out, {}, std::move(s)}); return *this; }
    TerminalPane& err(std::string s)  { lines_.push_back({Kind::Err, {}, std::move(s)}); return *this; }
    TerminalPane& info(std::string s) { lines_.push_back({Kind::Info, {}, std::move(s)}); return *this; }
    TerminalPane& ok(std::string s)   { lines_.push_back({Kind::Ok, {}, std::move(s)}); return *this; }
    // Live input line (caret shown at the end).
    TerminalPane& input(std::string cwd, std::string typed) {
        cwd_ = std::move(cwd); input_ = std::move(typed); has_input_ = true; return *this;
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> rows;
        rows.reserve(lines_.size() + 1);
        for (const auto& ln : lines_) rows.push_back(line_row(ln));
        if (has_input_) rows.push_back(input_row());
        return dsl::v(std::move(rows)).build();
    }

private:
    enum class Kind : uint8_t { Prompt, Out, Err, Info, Ok };
    struct Line { Kind kind; std::string cwd; std::string text; };

    std::vector<Line> lines_;
    std::string       cwd_, input_;
    bool              has_input_ = false;
    TerminalPaneTheme theme;

    void put(std::string& s, std::vector<StyledRun>& r, std::string_view t, Style st) const {
        if (t.empty()) return; r.push_back({s.size(), t.size(), st}); s += t;
    }

    Element line_row(const Line& ln) const {
        std::string s; std::vector<StyledRun> r;
        if (ln.kind == Kind::Prompt) {
            put(s, r, ln.cwd + " ", Style{}.with_fg(theme.cwd));
            put(s, r, "\xe2\x9d\xaf ", Style{}.with_fg(theme.prompt).with_bold()); // ❯
            put(s, r, ln.text, Style{}.with_fg(theme.command));
        } else {
            Color c = ln.kind == Kind::Err  ? theme.err
                    : ln.kind == Kind::Info ? theme.info
                    : ln.kind == Kind::Ok   ? theme.ok : theme.out;
            std::string_view mark = ln.kind == Kind::Err  ? "\xe2\x9c\x98 "  // ✘
                                  : ln.kind == Kind::Ok   ? "\xe2\x9c\x94 "  // ✔
                                  : ln.kind == Kind::Info ? "\xe2\x84\xb9 "  // ℹ
                                  : "  ";
            put(s, r, mark, Style{}.with_fg(c));
            put(s, r, ln.text, Style{}.with_fg(c));
        }
        return Element{TextElement{ .content = std::move(s), .style = Style{},
                                    .wrap = TextWrap::NoWrap, .runs = std::move(r) }};
    }

    Element input_row() const {
        std::string s; std::vector<StyledRun> r;
        put(s, r, cwd_ + " ", Style{}.with_fg(theme.cwd));
        put(s, r, "\xe2\x9d\xaf ", Style{}.with_fg(theme.prompt).with_bold()); // ❯
        put(s, r, input_, Style{}.with_fg(theme.command));
        put(s, r, " ", Style{}.with_inverse()); // block caret (terminal colours)
        return Element{TextElement{ .content = std::move(s), .style = Style{},
                                    .wrap = TextWrap::NoWrap, .runs = std::move(r) }};
    }
};

} // namespace maya
