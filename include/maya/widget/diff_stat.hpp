#pragma once
// maya::widget::DiffStat — changed-files summary (background-free)
//
// The `git diff --stat` view: each changed file with its +added / -removed
// counts and a proportional ▰▱ bar, plus a status letter. Foreground-only;
// the file name column truncates while the stat column stays aligned right.
//
// Usage:
//   DiffStat d;
//   d.file("src/rope.cpp", 42, 8).file("include/rope.hpp", 3, 0)
//    .file("tests/rope_test.cpp", 0, 15);
//   Element ui = d | dsl::width(52);

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct DiffStatTheme {
    Color name  = Color::hex(0xBAC2DE);
    Color added = Color::hex(0xA6E3A1);
    Color removed = Color::hex(0xF38BA8);
    Color muted   = Color::hex(0x45475A);
    Color count    = Color::hex(0x9399B2);
};

class DiffStat {
public:
    DiffStat& file(std::string name, int added, int removed) {
        files_.push_back({std::move(name), added, removed}); return *this;
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        int maxtot = 1;
        for (const auto& f : files_) maxtot = std::max(maxtot, f.added + f.removed);
        std::vector<Element> rows;
        for (const auto& f : files_) rows.push_back(row(f, maxtot));
        return dsl::v(std::move(rows)).build();
    }

private:
    struct F { std::string name; int added, removed; };
    std::vector<F> files_;
    DiffStatTheme  theme;
    int            bar_ = 12;

    Element row(const F& f, int maxtot) const {
        const int bar_ = this->bar_;
        const auto name = f.name; const int add = f.added, rem = f.removed;
        const auto th = theme;
        return Element{ComponentElement{
            .render = [=](int w, int) -> Element {
                // stat column: "+42 -8 ▰▰▰▱▱"
                const int tot = add + rem;
                int fa = tot ? (add * bar_ + tot / 2) / std::max(1, tot) : 0;
                fa = std::clamp(fa, 0, bar_);

                std::string cnt = "+" + std::to_string(add) + " -" + std::to_string(rem) + " ";
                std::string bar_add, bar_rem;
                for (int i = 0; i < fa; ++i) bar_add += "\xe2\x96\xb0"; // ▰
                for (int i = fa; i < bar_; ++i) bar_rem += "\xe2\x96\xb1"; // ▱

                std::string right; std::vector<StyledRun> rr;
                auto rput = [&](std::string_view t, Style st){ if(t.empty())return;
                    rr.push_back({right.size(), t.size(), st}); right += t; };
                rput("+" + std::to_string(add) + " ", Style{}.with_fg(th.added));
                rput("-" + std::to_string(rem) + " ", Style{}.with_fg(th.removed));
                rput(bar_add, Style{}.with_fg(rem == 0 ? th.added : th.removed));
                rput(bar_rem, Style{}.with_fg(th.muted));
                (void)cnt;

                std::string s; std::vector<StyledRun> runs;
                const int right_w = string_width(right);
                int avail = std::max(1, w - right_w - 1);
                std::string nm = string_width(name) > avail
                    ? maya::detail::truncate_middle(name, avail) : name;
                runs.push_back({0, nm.size(), Style{}.with_fg(th.name)}); s = nm;
                int gap = std::max(1, w - string_width(s) - right_w);
                runs.push_back({s.size(), static_cast<size_t>(gap), Style{}});
                s.append(static_cast<size_t>(gap), ' ');
                size_t off = s.size(); s += right;
                for (auto run : rr) runs.push_back({off + run.byte_offset, run.byte_length, run.style});
                return Element{TextElement{ .content = std::move(s), .style = Style{},
                                            .wrap = TextWrap::NoWrap, .runs = std::move(runs) }};
            },
            .measure = [](int mw) -> Size { return Size{Columns(mw), Rows(1)}; },
        }};
    }
};

} // namespace maya
