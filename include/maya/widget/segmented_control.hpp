#pragma once
// maya::widget::SegmentedControl — segmented toggle (background-free)
//
// A row of mutually-exclusive segments (like an iOS segmented control / a view
// switcher): the active segment is bold + accent + shaded, others dim, joined
// by thin dividers inside a rounded frame.
//
// Usage:  SegmentedControl s; s.seg("Code").seg("Blame").seg("Preview").active(0);

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/builder.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct SegmentedControlTheme {
    Color active   = Color::hex(0xF5F5F7);
    Color idle     = Color::hex(0x6C7086);
    Color shade    = Color::hex(0x313244);
    Color divider  = Color::hex(0x313244);
    Color border    = Color::hex(0x313244);
};

struct SegmentedControl {
    std::vector<std::string> segs;
    int                      active_ = 0;
    SegmentedControlTheme    theme;

    SegmentedControl& seg(std::string s) { segs.push_back(std::move(s)); return *this; }
    SegmentedControl& active(int i) { active_ = i; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::string s; std::vector<StyledRun> r;
        auto put = [&](std::string_view t, Style st){ if(t.empty())return;
            r.push_back({s.size(), t.size(), st}); s += t; };
        for (size_t i = 0; i < segs.size(); ++i) {
            const bool on = (static_cast<int>(i) == active_);
            if (i) put("\xe2\x94\x82", Style{}.with_fg(theme.divider)); // │
            Style st = Style{}.with_fg(on ? theme.active : theme.idle);
            if (on) st = st.with_bold().with_bg(theme.shade);
            put(" " + segs[i] + " ", st);
        }
        Element row{TextElement{ .content = std::move(s), .style = Style{},
                                 .wrap = TextWrap::NoWrap, .runs = std::move(r) }};
        return maya::detail::box().border(BorderStyle::Round).border_color(theme.border)(row);
    }
};

} // namespace maya
