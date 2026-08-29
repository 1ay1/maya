#pragma once
// maya::widget::ProblemsPanel — grouped diagnostics list (background-free)
//
// The "Problems" tab: diagnostics grouped by file. Each file is a collapsible
// header with its icon, name, and error/warning count badges; expanded groups
// list each diagnostic with a severity glyph, message, optional code, and a
// right-aligned Ln:Col. Foreground-only, tree-guide connectors under files.
//
// Usage:
//   ProblemsPanel p;
//   p.file("rope.cpp", true)
//     .problem(Severity::Error,   "expected ';' after expression", 41, 18, "E0001")
//     .problem(Severity::Warning, "unused variable 'weight'",       12,  9)
//    .file("editor.rs", true)
//     .problem(Severity::Warning, "value assigned is never read",   88, 13);
//   Element ui = p | dsl::width(60);

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"
#include "diagnostic_lens.hpp"   // Severity

namespace maya {

struct ProblemsPanelTheme {
    Color file    = Color::hex(0xCDD6F4);
    Color guide   = Color::hex(0x45475A);
    Color message = Color::hex(0xBAC2DE);
    Color code     = Color::hex(0x585B70); // rule code (E0001)
    Color loc       = Color::hex(0x585B70); // Ln:Col
    Color error     = Color::hex(0xF38BA8);
    Color warning   = Color::hex(0xE2B341);
    Color info       = Color::hex(0x89B4FA);
    Color hint        = Color::hex(0x94E2D5);
    Color active      = Color::hex(0x232634); // active-row wash
};

class ProblemsPanel {
public:
    ProblemsPanel& file(std::string name, bool open = true) {
        groups_.push_back({std::move(name), open, {}});
        return *this;
    }
    ProblemsPanel& problem(Severity sev, std::string msg, int line, int col,
                           std::string code = {}) {
        groups_.back().items.push_back({sev, std::move(msg), std::move(code), line, col});
        return *this;
    }
    ProblemsPanel& active(int i) { active_ = i; return *this; } // flat row index

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> rows;
        int idx = 0;
        for (const auto& g : groups_) {
            rows.push_back(file_row(g, idx == active_));
            ++idx;
            if (!g.open) continue;
            for (size_t j = 0; j < g.items.size(); ++j) {
                const bool last = (j + 1 == g.items.size());
                rows.push_back(item_row(g.items[j], last, idx == active_));
                ++idx;
            }
        }
        return dsl::v(std::move(rows)).build();
    }

private:
    struct Item { Severity sev; std::string msg; std::string code; int line, col; };
    struct Group { std::string name; bool open; std::vector<Item> items; };

    std::vector<Group> groups_;
    int                active_ = -1;
    ProblemsPanelTheme theme;

    Color sev_color(Severity s) const {
        switch (s) {
            case Severity::Error:   return theme.error;
            case Severity::Warning: return theme.warning;
            case Severity::Info:    return theme.info;
            case Severity::Hint:    return theme.hint;
        }
        return theme.info;
    }
    const char* sev_glyph(Severity s) const {
        switch (s) {
            case Severity::Error:   return "\xef\x81\x97"; //  times-circle
            case Severity::Warning: return "\xef\x81\xb1"; //  warning
            case Severity::Info:    return "\xef\x81\x9a"; //  info-circle
            case Severity::Hint:    return "\xef\x83\xab"; //  lightbulb
        }
        return "\xef\x81\x9a";
    }

    Element file_row(const Group& g, bool on) const {
        int errs = 0, warns = 0;
        for (const auto& it : g.items) {
            if (it.sev == Severity::Error) ++errs;
            else if (it.sev == Severity::Warning) ++warns;
        }
        std::string s; std::vector<StyledRun> r;
        auto tint = [on, this](Style st){ return on ? st.with_bg(theme.active) : st; };
        auto put = [&](std::string_view t, Style st) {
            if (t.empty()) return; r.push_back({s.size(), t.size(), tint(st)}); s += t;
        };
        put(g.open ? "\xef\x84\x87 " : "\xef\x84\x85 ", Style{}.with_fg(theme.guide)); //  / 
        put("\xef\x85\x9b ", Style{}.with_fg(theme.info)); //  file
        put(g.name, Style{}.with_fg(theme.file).with_bold());
        if (errs)  put("  \xef\x81\x97 " + std::to_string(errs), Style{}.with_fg(theme.error));
        if (warns) put("  \xef\x81\xb1 " + std::to_string(warns), Style{}.with_fg(theme.warning));
        return Element{TextElement{ .content = std::move(s), .style = Style{},
                                    .wrap = TextWrap::NoWrap, .runs = std::move(r) }};
    }

    Element item_row(const Item& it, bool last, bool on) const {
        std::string left; std::vector<StyledRun> lr;
        auto tint = [on, this](Style st){ return on ? st.with_bg(theme.active) : st; };
        auto put = [&](std::string_view t, Style st) {
            if (t.empty()) return; lr.push_back({left.size(), t.size(), tint(st)}); left += t;
        };
        put(last ? " \xe2\x94\x94\xe2\x94\x80 " : " \xe2\x94\x9c\xe2\x94\x80 ",  // └─ / ├─
            Style{}.with_fg(theme.guide));
        put(std::string(sev_glyph(it.sev)) + " ", Style{}.with_fg(sev_color(it.sev)));
        put(it.msg, Style{}.with_fg(theme.message));
        if (!it.code.empty()) put("  " + it.code, Style{}.with_fg(theme.code));

        std::string loc = "Ln " + std::to_string(it.line) + ", Col " + std::to_string(it.col);

        return Element{ComponentElement{
            .render = [left = std::move(left), lr = std::move(lr), loc = std::move(loc),
                       on, lcol = theme.loc, shade = theme.active](int w, int) -> Element {
                const Style fill = on ? Style{}.with_bg(shade) : Style{};
                std::string s = left; std::vector<StyledRun> runs = lr;
                int lw = string_width(left), rw = string_width(loc);
                int gap = std::max(2, w - lw - rw);
                runs.push_back({s.size(), static_cast<size_t>(gap), fill});
                s.append(static_cast<size_t>(gap), ' ');
                Style ls = Style{}.with_fg(lcol);
                if (on) ls = ls.with_bg(shade);
                runs.push_back({s.size(), loc.size(), ls});
                s += loc;
                int total = string_width(s);
                if (total < w) {
                    runs.push_back({s.size(), static_cast<size_t>(w - total), fill});
                    s.append(static_cast<size_t>(w - total), ' ');
                }
                return Element{TextElement{ .content = std::move(s), .style = fill,
                                            .wrap = TextWrap::NoWrap, .runs = std::move(runs) }};
            },
        }};
    }
};

} // namespace maya
