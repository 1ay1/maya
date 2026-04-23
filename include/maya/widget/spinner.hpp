#pragma once
// maya::widget::spinner — Animated spinner indicators
//
// Frame-based animation spinners with multiple built-in styles.
// Updates via elapsed time — no threads needed.
//
// Usage:
//   Spinner spin;  // default dots style
//   spin.advance(dt);
//   auto ui = h(spin, text(" Loading..."));
//
//   Spinner<SpinnerStyle::Braille> s2;  // braille style

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "../element/builder.hpp"
#include "../style/style.hpp"

namespace maya {

// ============================================================================
// SpinnerStyle — built-in animation patterns
// ============================================================================

enum class SpinnerStyle : uint8_t {
    Dots,       // ⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏
    Line,       // -\|/
    Arc,        // ◜◠◝◞◡◟
    Arrow,      // ←↖↑↗→↘↓↙
    Bounce,     // ⠁⠂⠄⠂
    Bar,        // ▉▊▋▌▍▎▏▎▍▌▋▊▉
    Clock,      // 🕐🕑🕒🕓🕔🕕🕖🕗🕘🕙🕚🕛
    Star,       // ✶✸✹✺✹✷
    Pulse,      // ░▒▓█▓▒
};

namespace detail {

struct SpinnerFrames {
    const char* const* frames;
    int count;
    float interval;  // seconds per frame
};

// Dots
inline constexpr const char* dots_frames[] = {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"
};

// Line
inline constexpr const char* line_frames[] = {
    "-", "\\", "|", "/"
};

// Arc
inline constexpr const char* arc_frames[] = {
    "◜", "◠", "◝", "◞", "◡", "◟"
};

// Arrow
inline constexpr const char* arrow_frames[] = {
    "←", "↖", "↑", "↗", "→", "↘", "↓", "↙"
};

// Bounce
inline constexpr const char* bounce_frames[] = {
    "⠁", "⠂", "⠄", "⠂"
};

// Bar
inline constexpr const char* bar_frames[] = {
    "▉", "▊", "▋", "▌", "▍", "▎", "▏", "▎", "▍", "▌", "▋", "▊", "▉"
};

// Clock
inline constexpr const char* clock_frames[] = {
    "🕐", "🕑", "🕒", "🕓", "🕔", "🕕", "🕖", "🕗", "🕘", "🕙", "🕚", "🕛"
};

// Star
inline constexpr const char* star_frames[] = {
    "✶", "✸", "✹", "✺", "✹", "✷"
};

// Pulse
inline constexpr const char* pulse_frames[] = {
    "░", "▒", "▓", "█", "▓", "▒"
};

template <SpinnerStyle S>
consteval SpinnerFrames get_spinner_frames() {
    if constexpr (S == SpinnerStyle::Dots)    return {dots_frames,    10, 0.08f};
    if constexpr (S == SpinnerStyle::Line)    return {line_frames,     4, 0.10f};
    if constexpr (S == SpinnerStyle::Arc)     return {arc_frames,      6, 0.10f};
    if constexpr (S == SpinnerStyle::Arrow)   return {arrow_frames,    8, 0.10f};
    if constexpr (S == SpinnerStyle::Bounce)  return {bounce_frames,   4, 0.12f};
    if constexpr (S == SpinnerStyle::Bar)     return {bar_frames,     13, 0.08f};
    if constexpr (S == SpinnerStyle::Clock)   return {clock_frames,   12, 0.10f};
    if constexpr (S == SpinnerStyle::Star)    return {star_frames,     6, 0.08f};
    if constexpr (S == SpinnerStyle::Pulse)   return {pulse_frames,    6, 0.12f};
}

} // namespace detail

// ============================================================================
// Spinner — animated spinner widget
// ============================================================================

template <SpinnerStyle S = SpinnerStyle::Dots>
class Spinner {
    static constexpr auto frames_ = detail::get_spinner_frames<S>();
    float elapsed_ = 0.0f;
    int frame_ = 0;
    Style style_{};

public:
    Spinner() = default;

    explicit Spinner(Style s) : style_(s) {}

    void advance(float dt) {
        elapsed_ += dt;
        if (elapsed_ >= frames_.interval) {
            int steps = static_cast<int>(elapsed_ / frames_.interval);
            frame_ = (frame_ + steps) % frames_.count;
            elapsed_ -= steps * frames_.interval;
        }
    }

    void set_style(Style s) { style_ = s; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        return Element{TextElement{
            .content = std::string{frames_.frames[frame_]},
            .style = style_,
        }};
    }

    // Current frame glyph — lets callers wrap the spinner into a mixed-style
    // text run (e.g. status-bar pills) without constructing a standalone
    // Element just for the style override.
    [[nodiscard]] std::string_view current_frame() const noexcept {
        return frames_.frames[frame_];
    }

    // Current frame index — useful when handing the spinner's tick state
    // to another widget that owns its own frame mapping (e.g. Timeline,
    // which takes an int frame and resolves to its own spinner glyph).
    // Lets one Tick subscription drive multiple animated surfaces in
    // lockstep instead of each computing its own frame from the clock.
    [[nodiscard]] int frame_index() const noexcept { return frame_; }
};

} // namespace maya
