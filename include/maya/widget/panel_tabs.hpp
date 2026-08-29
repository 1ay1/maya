#pragma once
// maya::widget::PanelTabs — bottom-panel tab header (background-free)
//
// The header row of the bottom panel: PROBLEMS / OUTPUT / TERMINAL / DEBUG,
// each with an optional count badge. The active tab is bright + bold with an
// accent dot; inactive tabs are dim. Foreground-only.
//
// Usage:
//   PanelTabs p;
//   p.tab("PROBLEMS", 4).tab("OUTPUT").tab("DEBUG CONSOLE").tab("TERMINAL")
//    .active(0);
//   Element ui = p;

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct PanelTabsTheme {
    Color active   = Color::hex(0xE6EDF3);
    Color idle     = Color::hex(0x6C7086);
    Color accent   = Color::hex(0x89B4FA);
    Color badge     = Color::hex(0xF38BA8);
};

struct PanelTabs {
    struct Tab { std::string label; int count; };
    std::vector<Tab> tabs;
    int              active_ = 0;
    PanelTabsTheme   theme;

    PanelTabs& tab(std::string label, int count = -1) {
        tabs.push_back({std::move(label), count}); return *this;
    }
    PanelTabs& active(int i) { active_ = i; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::string s; std::vector<StyledRun> r;
        auto put = [&](std::string_view t, Style st) {
            if (t.empty()) return; r.push_back({s.size(), t.size(), st}); s += t;
        };
        put(" ", Style{});
        for (size_t i = 0; i < tabs.size(); ++i) {
            const bool on = (static_cast<int>(i) == active_);
            if (i) put("    ", Style{});
            put(on ? "\xe2\x97\x86 " : "  ", Style{}.with_fg(theme.accent)); // ◆
            Style ls = Style{}.with_fg(on ? theme.active : theme.idle);
            if (on) ls = ls.with_bold();
            put(tabs[i].label, ls);
            if (tabs[i].count >= 0)
                put(" " + std::to_string(tabs[i].count),
                    Style{}.with_fg(on ? theme.badge : theme.idle).with_bold());
        }
        return Element{TextElement{ .content = std::move(s), .style = Style{},
                                    .wrap = TextWrap::NoWrap, .runs = std::move(r) }};
    }
};

} // namespace maya
