#pragma once
// maya::widget::DebugToolbar — debugger control bar (background-free)
//
// The floating run/step controls shown while debugging: continue/pause, step
// over, step into, step out, restart, stop — the active-when-relevant ones
// bright, the rest dim, plus a state label (Running / Paused / Stopped).
// Thin rounded border. Foreground-only.
//
// Usage:
//   DebugToolbar d{DebugToolbar::Paused};
//   Element ui = d;

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

struct DebugToolbarTheme {
    Color control  = Color::hex(0xCDD6F4);
    Color idle     = Color::hex(0x494D64);
    Color go       = Color::hex(0xA6E3A1); // continue
    Color stop      = Color::hex(0xF38BA8);
    Color restart   = Color::hex(0xE2B341);
    Color label      = Color::hex(0x9399B2);
};

struct DebugToolbar {
    enum State : uint8_t { Running, Paused, Stopped };
    State            state = Paused;
    DebugToolbarTheme theme;

    explicit DebugToolbar(State s = Paused) : state(s) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        const bool live = (state != Stopped);
        std::string s; std::vector<StyledRun> r;
        auto put = [&](std::string_view t, Style st){ if(t.empty())return;
            r.push_back({s.size(), t.size(), st}); s += t; };
        auto btn = [&](const char* glyph, Color c, bool enabled) {
            put(glyph, Style{}.with_fg(enabled ? c : theme.idle).with_bold());
            put("   ", Style{});
        };

        put(" ", Style{});
        // continue (paused) OR pause (running)
        if (state == Running) btn("\xef\x81\x8c", theme.control, true);   //  pause
        else                  btn("\xef\x81\x8b", theme.go, state != Stopped); //  play
        btn("\xef\x8b\x92", theme.control, live);   //  step over
        btn("\xef\x8b\x91", theme.control, live);   //  step into
        btn("\xef\x8b\x93", theme.control, live);   //  step out
        btn("\xef\x8b\x9e", theme.restart, live);   //  restart
        btn("\xef\x81\x8d", theme.stop, live);      //  stop

        const char* lbl = state == Running ? "Running"
                        : state == Paused  ? "Paused" : "Stopped";
        Color lc = state == Running ? theme.go
                 : state == Paused  ? theme.restart : theme.idle;
        put(" \xe2\x94\x82  ", Style{}.with_fg(theme.idle)); // │
        put(lbl, Style{}.with_fg(lc).with_bold());
        put(" ", Style{});

        Element rowE{TextElement{ .content = std::move(s), .style = Style{},
                                  .wrap = TextWrap::NoWrap, .runs = std::move(r) }};
        return maya::detail::box()
            .border(BorderStyle::Round).border_color(Color::hex(0x313244))
            (rowE);
    }
};

} // namespace maya
