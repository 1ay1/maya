#pragma once
// maya::widget::HunkControls — diff hunk action toolbar (background-free)
//
// The little control strip attached to a diff hunk: the hunk range, prev/next
// navigation, and Stage / Unstage / Revert actions. Foreground-only.
//
// Usage:
//   HunkControls h{"@@ -10,6 +10,7 @@"}.staged(false);
//   Element ui = h;

#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct HunkControlsTheme {
    Color range  = Color::hex(0x89DCEB);
    Color nav    = Color::hex(0x6C7086);
    Color stage  = Color::hex(0xA6E3A1);
    Color revert  = Color::hex(0xF38BA8);
    Color sep      = Color::hex(0x45475A);
};

struct HunkControls {
    std::string range_;
    bool        staged_ = false;
    HunkControlsTheme theme;

    explicit HunkControls(std::string range = {}) : range_(std::move(range)) {}
    HunkControls& staged(bool v) { staged_ = v; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::string s; std::vector<StyledRun> r;
        auto put=[&](std::string_view t, Style st){ if(t.empty())return;
            r.push_back({s.size(),t.size(),st}); s+=t; };
        put(range_, Style{}.with_fg(theme.range));
        put("   ", Style{});
        put("\xef\x81\x86 ", Style{}.with_fg(theme.nav)); //  prev
        put("\xef\x81\x87", Style{}.with_fg(theme.nav));  //  next
        put("   \xe2\x94\x82   ", Style{}.with_fg(theme.sep)); // │
        if (staged_) {
            put("\xef\x81\x98 Unstage Hunk", Style{}.with_fg(theme.stage).with_bold()); //  minus
        } else {
            put("\xef\x81\xa7 Stage Hunk", Style{}.with_fg(theme.stage).with_bold());   //  plus
        }
        put("   ", Style{});
        put("\xef\x83\xa2 Revert", Style{}.with_fg(theme.revert));  //  undo
        return Element{TextElement{ .content=std::move(s), .style=Style{},
                                    .wrap=TextWrap::NoWrap, .runs=std::move(r) }};
    }
};

} // namespace maya
