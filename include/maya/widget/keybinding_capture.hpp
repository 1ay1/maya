#pragma once
// maya::widget::KeybindingCapture — key-chord recorder field (background-free)
//
// The "press desired key combination" input used in keybinding editors: a
// bordered field that shows the captured chord as key caps, or a pulsing prompt
// while recording and empty.
//
// Usage:
//   KeybindingCapture k; k.recording(true).keys({"Ctrl","K","Ctrl","S"});
//   Element ui = k | dsl::width(40);

#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/builder.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct KeybindingCaptureTheme {
    Color border   = Color::hex(0x45475A);
    Color recording = Color::hex(0xF38BA8);
    Color prompt    = Color::hex(0x585B70);
    Color cap        = Color::hex(0xE6EDF3);
    Color edge        = Color::hex(0x45475A);
    Color plus         = Color::hex(0x585B70);
};

struct KeybindingCapture {
    std::vector<std::string> keys_;
    bool                     recording_ = false;
    KeybindingCaptureTheme   theme;

    KeybindingCapture& keys(std::vector<std::string> k) { keys_ = std::move(k); return *this; }
    KeybindingCapture& recording(bool v = true) { recording_ = v; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::string s; std::vector<StyledRun> r;
        auto put=[&](std::string_view t, Style st){ if(t.empty())return;
            r.push_back({s.size(),t.size(),st}); s+=t; };
        if (recording_)
            put("\xe2\x97\x8f ", Style{}.with_fg(theme.recording).with_bold()); // ● rec dot
        if (keys_.empty()) {
            put("Press desired key combination\xe2\x80\xa6", Style{}.with_fg(theme.prompt).with_italic());
        } else {
            for (size_t i = 0; i < keys_.size(); ++i) {
                if (i) put(" + ", Style{}.with_fg(theme.plus));
                put("\xe2\x9d\xb4", Style{}.with_fg(theme.edge));
                put(keys_[i], Style{}.with_fg(theme.cap).with_bold());
                put("\xe2\x9d\xb5", Style{}.with_fg(theme.edge));
            }
        }
        Element row{TextElement{ .content=std::move(s), .style=Style{},
                                 .wrap=TextWrap::NoWrap, .runs=std::move(r) }};
        Color bc = recording_ ? theme.recording : theme.border;
        return maya::detail::box().border(BorderStyle::Round).border_color(bc)
            .padding(0,1,0,1)(row);
    }
};

} // namespace maya
