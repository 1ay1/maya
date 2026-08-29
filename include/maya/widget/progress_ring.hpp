#pragma once
// maya::widget::ProgressRing — circular progress / spinner (background-free)
//
// A one-glyph rotating ring (for indeterminate work) or a quarter-filled ring
// stepped by a 0..1 value, with an optional label + percentage. Complements
// StatusProgress's linear bar.
//
// Usage:
//   ProgressRing{}.frame(n).label("Loading");            // spin
//   ProgressRing{}.value(0.6f).label("Downloading");     // determinate

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct ProgressRingTheme {
    Color ring  = Color::hex(0x89B4FA);
    Color label = Color::hex(0xBAC2DE);
    Color pct    = Color::hex(0x9399B2);
};

struct ProgressRing {
    float value_ = -1.0f;   // <0 => indeterminate
    int   frame_ = 0;
    std::string label_;
    ProgressRingTheme theme;

    ProgressRing& value(float v) { value_ = v; return *this; }
    ProgressRing& frame(int f)   { frame_ = f; return *this; }
    ProgressRing& label(std::string s) { label_ = std::move(s); return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::string s; std::vector<StyledRun> r;
        auto put=[&](std::string_view t, Style st){ if(t.empty())return;
            r.push_back({s.size(),t.size(),st}); s+=t; };
        if (value_ < 0.0f) {
            static const char* sp[] = {"\xe2\x97\x9c","\xe2\x97\x9d","\xe2\x97\x9e","\xe2\x97\x9f"}; // ◜◝◞◟
            put(sp[((frame_ % 4) + 4) % 4], Style{}.with_fg(theme.ring).with_bold());
        } else {
            // ○ ◔ ◑ ◕ ● quarter-filled ring by value
            static const char* q[] = {"\xe2\x97\x8b","\xe2\x97\x94","\xe2\x97\x91","\xe2\x97\x95","\xe2\x97\x8f"};
            int idx = std::clamp(static_cast<int>(value_ * 4 + 0.5f), 0, 4);
            put(q[idx], Style{}.with_fg(theme.ring).with_bold());
        }
        if (!label_.empty()) put("  " + label_, Style{}.with_fg(theme.label));
        if (value_ >= 0.0f)  put("  " + std::to_string(static_cast<int>(value_ * 100)) + "%",
                                 Style{}.with_fg(theme.pct));
        return Element{TextElement{ .content=std::move(s), .style=Style{},
                                    .wrap=TextWrap::NoWrap, .runs=std::move(r) }};
    }
};

} // namespace maya
