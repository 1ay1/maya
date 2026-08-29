#pragma once
// maya::widget::StagingView — git staged/unstaged files (background-free)
//
// The Source Control list: two groups (Staged Changes / Changes) with counts,
// each file a row with a status letter (M/A/D/U/R), the path, and a stage /
// unstage affordance. Active row shaded.
//
// Usage:
//   StagingView s;
//   s.staged("M", "src/rope.cpp").staged("A", "include/rope.hpp")
//    .change("M", "README.md").change("U", "notes.txt")
//    .active(0);
//   Element ui = s | dsl::width(40);

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct StagingTheme {
    Color header = Color::hex(0x585B70);
    Color path   = Color::hex(0xBAC2DE);
    Color modified = Color::hex(0xE2B341);
    Color added    = Color::hex(0xA6E3A1);
    Color deleted  = Color::hex(0xF38BA8);
    Color untracked= Color::hex(0x94E2D5);
    Color active    = Color::hex(0x232634);
    Color action    = Color::hex(0x6C7086);
};

class StagingView {
public:
    StagingView& staged(std::string st, std::string path) {
        staged_.push_back({std::move(st), std::move(path)}); return *this;
    }
    StagingView& change(std::string st, std::string path) {
        changes_.push_back({std::move(st), std::move(path)}); return *this;
    }
    StagingView& active(int i) { active_ = i; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> rows; int idx = 0;
        rows.push_back(header("STAGED CHANGES", static_cast<int>(staged_.size())));
        for (auto& f : staged_) rows.push_back(file_row(f, true, idx++ == active_));
        rows.push_back(header("CHANGES", static_cast<int>(changes_.size())));
        for (auto& f : changes_) rows.push_back(file_row(f, false, idx++ == active_));
        return dsl::v(std::move(rows)).build();
    }

private:
    struct F { std::string st, path; };
    std::vector<F> staged_, changes_;
    int            active_ = -1;
    StagingTheme   theme;

    Color st_color(const std::string& st) const {
        if (st == "M") return theme.modified;
        if (st == "A") return theme.added;
        if (st == "D") return theme.deleted;
        if (st == "U") return theme.untracked;
        return theme.path;
    }

    Element header(std::string t, int n) const {
        std::string s = t + "  " + std::to_string(n);
        std::vector<StyledRun> r{ {0, s.size(), Style{}.with_fg(theme.header).with_bold()} };
        return Element{TextElement{ .content = std::move(s), .style = Style{},
                                    .wrap = TextWrap::NoWrap, .runs = std::move(r) }};
    }
    Element file_row(const F& f, bool staged, bool on) const {
        const Color shade = on ? theme.active : Color{};
        auto tint = [on, shade](Style st){ return on ? st.with_bg(shade) : st; };
        std::string left; std::vector<StyledRun> lr;
        auto put = [&](std::string_view t, Style st){ if(t.empty())return;
            lr.push_back({left.size(), t.size(), tint(st)}); left += t; };
        put(staged ? "\xe2\x9c\x93 " : "  ", tint(Style{}.with_fg(theme.added))); // ✓
        put(f.st + " ", Style{}.with_fg(st_color(f.st)).with_bold());
        put(f.path, Style{}.with_fg(theme.path));
        std::string act = staged ? "\xe2\x88\x92" : "\xef\x81\xa7"; // − unstage /  stage(+)
        return Element{ComponentElement{
            .render = [left = std::move(left), lr = std::move(lr), act = std::move(act),
                       on, shade, ac = theme.action](int w, int) -> Element {
                const Style fill = on ? Style{}.with_bg(shade) : Style{};
                std::string s = left; std::vector<StyledRun> runs = lr;
                int gap = std::max(1, w - string_width(s) - string_width(act));
                runs.push_back({s.size(), static_cast<size_t>(gap), fill});
                s.append(static_cast<size_t>(gap), ' ');
                Style as = Style{}.with_fg(ac); if (on) as = as.with_bg(shade);
                runs.push_back({s.size(), act.size(), as}); s += act;
                int total = string_width(s);
                if (total < w) { runs.push_back({s.size(), static_cast<size_t>(w-total), fill});
                                 s.append(static_cast<size_t>(w-total), ' '); }
                return Element{TextElement{ .content = std::move(s), .style = fill,
                                            .wrap = TextWrap::NoWrap, .runs = std::move(runs) }};
            },
            .measure = [](int mw) -> Size { return Size{Columns(mw), Rows(1)}; },
        }};
    }
};

} // namespace maya
