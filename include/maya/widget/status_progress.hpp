#pragma once
// maya::widget::StatusProgress — inline progress segment (background-free)
//
// A one-line labelled progress indicator for a status bar or a task row: a
// label, a ━/─ bar, and a percentage (or a spinner frame for indeterminate
// work). Foreground-only.
//
// Usage:
//   StatusProgress p; p.label("Indexing").value(0.42f);
//   StatusProgress q; q.label("Cloning").indeterminate(frame);

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

struct StatusProgressTheme {
    Color label = Color::hex(0xBAC2DE);
    Color fill  = Color::hex(0x89B4FA);
    Color track = Color::hex(0x45475A);
    Color pct    = Color::hex(0x9399B2);
};

struct StatusProgress {
    std::string label_;
    float       value_ = 0.0f;
    int         width  = 24;
    bool        indeterminate_ = false;
    int         frame_ = 0;
    StatusProgressTheme theme;

    StatusProgress& label(std::string s) { label_ = std::move(s); return *this; }
    StatusProgress& value(float v) { value_ = v; indeterminate_ = false; return *this; }
    StatusProgress& indeterminate(int frame) { indeterminate_ = true; frame_ = frame; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::string s; std::vector<StyledRun> r;
        auto put = [&](std::string_view t, Style st){ if(t.empty())return;
            r.push_back({s.size(), t.size(), st}); s += t; };
        if (!label_.empty()) put(label_ + "  ", Style{}.with_fg(theme.label));

        if (indeterminate_) {
            static const char* fr[] = {"\xe2\xa0\x8b","\xe2\xa0\x99","\xe2\xa0\xb9",
                "\xe2\xa0\xb8","\xe2\xa0\xbc","\xe2\xa0\xb4","\xe2\xa0\xa6",
                "\xe2\xa0\xa7","\xe2\xa0\x87","\xe2\xa0\x8f"};
            put(fr[((frame_ % 10) + 10) % 10], Style{}.with_fg(theme.fill).with_bold());
        } else {
            const int filled = std::clamp(static_cast<int>(value_ * width + 0.5f), 0, width);
            std::string on, off;
            for (int i = 0; i < filled; ++i) on += "\xe2\x94\x81";        // ━
            for (int i = filled; i < width; ++i) off += "\xe2\x94\x80";   // ─
            put(on, Style{}.with_fg(theme.fill));
            put(off, Style{}.with_fg(theme.track));
            put("  " + std::to_string(static_cast<int>(value_ * 100)) + "%",
                Style{}.with_fg(theme.pct));
        }
        return Element{TextElement{ .content = std::move(s), .style = Style{},
                                    .wrap = TextWrap::NoWrap, .runs = std::move(r) }};
    }
};

} // namespace maya
