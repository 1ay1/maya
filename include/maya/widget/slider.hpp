#pragma once
// maya::widget::slider — Numeric range slider with visual track
//
// A focusable slider for selecting a numeric value within a range.
// Uses Left/Right arrow keys (or h/l) to adjust by step increments.
//
//   Volume   ████████████────────────────  42%
//   Speed    ██████████████████████████──  89%
//
// Usage:
//   Slider volume("Volume", {.min = 0, .max = 100, .step = 5});
//   volume.on_change([](float v) { set_volume(v); });
//   auto ui = volume.build();

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "../core/focus.hpp"
#include "../core/overload.hpp"
#include "../core/signal.hpp"
#include "../element/builder.hpp"
#include "../element/text.hpp"
#include "../style/style.hpp"
#include "../terminal/input.hpp"

namespace maya {

// ============================================================================
// SliderConfig — range and appearance
// ============================================================================

struct SliderConfig {
    float min  = 0.0f;
    float max  = 1.0f;
    float step = 0.01f;
    int   width = 0;       // track width in columns; 0 = fill available width
    Color fill_color   = Color::blue();
    Color track_color  = Color::bright_black();
    bool  show_percent = true;
};

// ============================================================================
// Slider — numeric range slider widget
// ============================================================================

class Slider {
    Signal<float> value_;
    FocusNode     focus_;
    std::string   label_;
    SliderConfig  cfg_;

    std::move_only_function<void(float)> on_change_;

public:
    Slider() : value_(0.0f) {}

    explicit Slider(std::string label, SliderConfig cfg = {})
        : value_(cfg.min)
        , label_(std::move(label))
        , cfg_(cfg) {}

    // -- Signal access --
    [[nodiscard]] const Signal<float>& value()       const { return value_; }
    [[nodiscard]] const FocusNode& focus_node()      const { return focus_; }
    [[nodiscard]] FocusNode& focus_node()                   { return focus_; }
    [[nodiscard]] std::string_view label()           const { return label_; }

    void set_value(float v) {
        value_.set(std::clamp(v, cfg_.min, cfg_.max));
    }

    void set_label(std::string_view l) { label_ = std::string{l}; }

    template <std::invocable<float> F>
    void on_change(F&& fn) { on_change_ = std::forward<F>(fn); }

    // -- Key event handling --
    [[nodiscard]] bool handle(const KeyEvent& ev) {
        if (!focus_.focused()) return false;

        return std::visit(overload{
            [&](SpecialKey sk) -> bool {
                switch (sk) {
                    case SpecialKey::Left:  adjust(-cfg_.step); return true;
                    case SpecialKey::Right: adjust(cfg_.step);  return true;
                    default: return false;
                }
            },
            [&](CharKey ck) -> bool {
                if (ck.codepoint == 'h') { adjust(-cfg_.step); return true; }
                if (ck.codepoint == 'l') { adjust(cfg_.step);  return true; }
                return false;
            },
        }, ev.key);
    }

    // -- Node concept --
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        if (cfg_.width > 0) {
            return build_track(cfg_.width);
        }
        // Dynamic width via ComponentElement
        return Element{ComponentElement{
            .render = [this](int w, int /*h*/) -> Element {
                int reserved = 0;
                if (!label_.empty()) reserved += static_cast<int>(label_.size()) + 2;
                if (cfg_.show_percent) reserved += 6;
                int track_w = w - reserved;
                if (track_w < 8) track_w = 8;
                return build_track(track_w);
            },
            .layout = {},
        }};
    }

private:
    void adjust(float delta) {
        float cur = value_();
        float next = std::clamp(cur + delta, cfg_.min, cfg_.max);
        // Snap to step
        float steps = std::round((next - cfg_.min) / cfg_.step);
        next = cfg_.min + steps * cfg_.step;
        next = std::clamp(next, cfg_.min, cfg_.max);
        value_.set(next);
        if (on_change_) on_change_(next);
    }

    [[nodiscard]] Element build_track(int track_w) const {
        bool focused = focus_.focused();
        float cur = value_();
        float range = cfg_.max - cfg_.min;
        float ratio = range > 0.0f ? (cur - cfg_.min) / range : 0.0f;
        ratio = std::clamp(ratio, 0.0f, 1.0f);

        int filled = static_cast<int>(ratio * static_cast<float>(track_w));
        filled = std::clamp(filled, 0, track_w);
        int empty = track_w - filled;

        std::string content;
        std::vector<StyledRun> runs;

        // Label
        if (!label_.empty()) {
            auto label_style = focused
                ? Style{}.with_bold()
                : Style{};
            runs.push_back(StyledRun{content.size(), label_.size(), label_style});
            content += label_;
            content += "  ";
            runs.push_back(StyledRun{content.size() - 2, 2, Style{}});
        }

        // Filled portion: ████
        if (filled > 0) {
            std::string filled_str;
            for (int i = 0; i < filled; ++i) filled_str += "\xe2\x96\x88";  // █
            auto fill_style = focused
                ? Style{}.with_fg(cfg_.fill_color)
                : Style{}.with_fg(cfg_.fill_color).with_dim();
            runs.push_back(StyledRun{content.size(), filled_str.size(), fill_style});
            content += filled_str;
        }

        // Empty track: ────
        if (empty > 0) {
            std::string track_str;
            for (int i = 0; i < empty; ++i) track_str += "\xe2\x94\x80";  // ─
            runs.push_back(StyledRun{content.size(), track_str.size(),
                Style{}.with_fg(cfg_.track_color)});
            content += track_str;
        }

        // Percentage
        if (cfg_.show_percent) {
            int pct = static_cast<int>(ratio * 100.0f);
            char pct_buf[8];
            std::snprintf(pct_buf, sizeof(pct_buf), "  %d%%", pct);
            std::string pct_str = pct_buf;
            auto pct_style = Style{};
            runs.push_back(StyledRun{content.size(), pct_str.size(), pct_style});
            content += pct_str;
        }

        return Element{TextElement{
            .content = std::move(content),
            .style = {},
            .wrap = TextWrap::NoWrap,
            .runs = std::move(runs),
        }};
    }
};

} // namespace maya
