#pragma once
// maya::widget::streaming_cursor — Animated typing/streaming indicator
//
// Shows a pulsing cursor or spinner to indicate that the assistant
// is actively streaming a response. Multiple animation styles.
//
//   StreamingCursor cursor;
//   cursor.set_label("Generating...");
//   cursor.tick();  // advance animation frame
//   auto ui = cursor.build();

#include <cstdint>
#include <string>
#include <vector>

#include "../dsl.hpp"
#include "../style/style.hpp"

namespace maya {

enum class CursorStyle : uint8_t {
    Block,    // █ blinking block
    Dots,     // ⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏ braille spinner
    Bar,      // ▏▎▍▌▋▊▉█ growing bar
    Pulse,    // ● ◉ ○ dimming circle
};

class StreamingCursor {
    CursorStyle style_  = CursorStyle::Dots;
    std::string label_;
    int frame_          = 0;
    bool active_        = true;

    static constexpr const char* braille_frames[] = {
        "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
        "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7",
        "\xe2\xa0\x87", "\xe2\xa0\x8f",
    };

    static constexpr const char* bar_frames[] = {
        "\xe2\x96\x8f", "\xe2\x96\x8e", "\xe2\x96\x8d", "\xe2\x96\x8c",
        "\xe2\x96\x8b", "\xe2\x96\x8a", "\xe2\x96\x89", "\xe2\x96\x88",
    };

    static constexpr const char* pulse_frames[] = {
        "\xe2\x97\x8f", // ●
        "\xe2\x97\x89", // ◉
        "\xe2\x97\x8b", // ○
        "\xe2\x97\x89", // ◉
    };

    [[nodiscard]] const char* current_frame() const {
        switch (style_) {
            case CursorStyle::Block:
                return (frame_ % 2 == 0) ? "\xe2\x96\x88" : " "; // █ or space
            case CursorStyle::Dots:
                return braille_frames[frame_ % 10];
            case CursorStyle::Bar:
                return bar_frames[frame_ % 8];
            case CursorStyle::Pulse:
                return pulse_frames[frame_ % 4];
        }
        return "\xe2\x96\x88";
    }

public:
    StreamingCursor() = default;

    void set_style(CursorStyle s) { style_ = s; }
    void set_label(std::string l) { label_ = std::move(l); }
    void set_active(bool b) { active_ = b; }
    void tick() { ++frame_; }
    void reset() { frame_ = 0; }

    [[nodiscard]] bool is_active() const { return active_; }
    [[nodiscard]] int frame() const { return frame_; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        if (!active_) return text("");

        Color accent = Color::rgb(198, 120, 221); // purple
        auto dim = Style{}.with_fg(Color::rgb(127, 132, 142));

        std::vector<Element> parts;

        // Spinner/cursor
        parts.push_back(text(current_frame(), Style{}.with_fg(accent).with_bold()));

        // Label
        if (!label_.empty()) {
            parts.push_back(text(" " + label_, dim));
        }

        return h(std::move(parts)).build();
    }
};

} // namespace maya
