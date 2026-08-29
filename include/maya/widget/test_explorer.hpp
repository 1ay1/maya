#pragma once
// maya::widget::TestExplorer — test tree with results (background-free)
//
// The Testing panel: suites and their tests as a tree, each with a status glyph
// (passed ✔ / failed ✘ / skipped ○ / running ◐), the name, and a right-aligned
// duration. Suites roll up a pass/total count. Foreground-only.
//
// Usage:
//   TestExplorer t;
//   t.suite("RopeTest", 3, 4)
//     .test(TestStatus::Passed,  "concat_joins", 1, "0.4ms")
//     .test(TestStatus::Failed,  "at_out_of_range", 1, "1.2ms")
//     .test(TestStatus::Skipped, "huge_file", 1);
//   Element ui = t | dsl::width(48);

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

enum class TestStatus : uint8_t { Passed, Failed, Skipped, Running };

struct TestExplorerTheme {
    Color suite = Color::hex(0xCDD6F4);
    Color name  = Color::hex(0xBAC2DE);
    Color guide = Color::hex(0x45475A);
    Color dur    = Color::hex(0x585B70);
    Color passed  = Color::hex(0xA6E3A1);
    Color failed  = Color::hex(0xF38BA8);
    Color skipped = Color::hex(0x6C7086);
    Color running = Color::hex(0xE2B341);
};

class TestExplorer {
public:
    TestExplorer& suite(std::string name, int passed, int total) {
        rows_.push_back({true, TestStatus::Passed, std::move(name), 0, "",
                         passed, total}); return *this;
    }
    TestExplorer& test(TestStatus st, std::string name, int depth, std::string dur = {}) {
        rows_.push_back({false, st, std::move(name), depth, std::move(dur), 0, 0});
        return *this;
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> out;
        for (const auto& r : rows_) out.push_back(row(r));
        return dsl::v(std::move(out)).build();
    }

private:
    struct R { bool suite; TestStatus st; std::string name; int depth; std::string dur; int passed, total; };
    std::vector<R> rows_;
    TestExplorerTheme theme;

    Color sc(TestStatus s) const {
        switch (s) { case TestStatus::Passed: return theme.passed;
                     case TestStatus::Failed: return theme.failed;
                     case TestStatus::Skipped: return theme.skipped;
                     default: return theme.running; }
    }
    const char* sg(TestStatus s) const {
        switch (s) { case TestStatus::Passed: return "\xe2\x9c\x94";   // ✔
                     case TestStatus::Failed: return "\xe2\x9c\x98";   // ✘
                     case TestStatus::Skipped: return "\xe2\x97\x8b";  // ○
                     default: return "\xe2\x97\x90"; }                 // ◐
    }

    Element row(const R& r) const {
        std::string left; std::vector<StyledRun> lr;
        auto put=[&](std::string_view t, Style st){ if(t.empty())return;
            lr.push_back({left.size(),t.size(),st}); left+=t; };
        for (int d = 0; d < r.depth; ++d) put("\xe2\x94\x82 ", Style{}.with_fg(theme.guide)); // │
        if (r.suite) {
            put("\xef\x84\x87 ", Style{}.with_fg(theme.guide));                    //  chevron
            put("\xef\x83\x85 ", Style{}.with_fg(theme.suite));                    //  flask
            put(r.name, Style{}.with_fg(theme.suite).with_bold());
            std::string cnt = "  " + std::to_string(r.passed) + "/" + std::to_string(r.total);
            put(cnt, Style{}.with_fg(r.passed == r.total ? theme.passed : theme.failed));
            return Element{TextElement{ .content=std::move(left), .style=Style{},
                                        .wrap=TextWrap::NoWrap, .runs=std::move(lr) }};
        }
        put(std::string(sg(r.st)) + " ", Style{}.with_fg(sc(r.st)).with_bold());
        put(r.name, Style{}.with_fg(r.st == TestStatus::Skipped ? theme.skipped : theme.name));
        std::string dur = r.dur;
        return Element{ComponentElement{
            .render=[left=std::move(left), lr=std::move(lr), dur=std::move(dur),
                     dc=theme.dur](int w, int)->Element{
                std::string s=left; std::vector<StyledRun> runs=lr;
                if(!dur.empty()){ int gap=std::max(1,w-string_width(s)-string_width(dur));
                    runs.push_back({s.size(),(size_t)gap,Style{}}); s.append((size_t)gap,' ');
                    runs.push_back({s.size(),dur.size(),Style{}.with_fg(dc)}); s+=dur; }
                return Element{TextElement{ .content=std::move(s), .style=Style{},
                                            .wrap=TextWrap::NoWrap, .runs=std::move(runs) }};
            },
            .measure=[](int mw)->Size{ return Size{Columns(mw),Rows(1)}; },
        }};
    }
};

} // namespace maya
