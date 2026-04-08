#pragma once
// maya::widget::checkbox — Toggle checkbox and toggle switch
//
// A reactive checkbox/toggle that integrates with the focus system.
// Two variants: Checkbox (tick box) and ToggleSwitch (sliding indicator).
//
// Checkbox:
//   [ ] Enable notifications
//   [x] Dark mode
//
// ToggleSwitch:
//   ◯━━━  Off
//   ━━━●  On
//
// Usage:
//   Checkbox cb("Enable dark mode");
//   cb.on_change([](bool checked) { update_theme(checked); });
//
//   ToggleSwitch ts("Notifications");
//   ts.on_change([](bool on) { toggle_notifs(on); });

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
// Checkbox — [x] / [ ] style toggle
// ============================================================================

class Checkbox {
    Signal<bool> checked_{false};
    FocusNode    focus_;
    std::string  label_;

    std::move_only_function<void(bool)> on_change_;

public:
    Checkbox() = default;
    explicit Checkbox(std::string label, bool initial = false)
        : checked_(initial), label_(std::move(label)) {}

    // -- Signal access --
    [[nodiscard]] const Signal<bool>& checked()      const { return checked_; }
    [[nodiscard]] const FocusNode& focus_node()      const { return focus_; }
    [[nodiscard]] FocusNode& focus_node()                   { return focus_; }
    [[nodiscard]] std::string_view label()           const { return label_; }

    void set_label(std::string_view l)  { label_ = std::string{l}; }
    void set_checked(bool v)            { checked_.set(v); }

    template <std::invocable<bool> F>
    void on_change(F&& fn) { on_change_ = std::forward<F>(fn); }

    // -- Key event handling --
    [[nodiscard]] bool handle(const KeyEvent& ev) {
        if (!focus_.focused()) return false;

        return std::visit(overload{
            [&](SpecialKey sk) -> bool {
                switch (sk) {
                    case SpecialKey::Enter:
                    case SpecialKey::Tab:  // Space comes through as Tab in some terminals
                        toggle();
                        return true;
                    default: return false;
                }
            },
            [&](CharKey ck) -> bool {
                if (ck.codepoint == ' ') { toggle(); return true; }
                return false;
            },
        }, ev.key);
    }

    // -- Node concept --
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        bool is_checked = checked_();
        bool focused = focus_.focused();

        // Build: "[x] label" or "[ ] label"
        std::string content;
        std::vector<StyledRun> runs;

        // Checkbox indicator
        std::string indicator = is_checked ? "[x]" : "[ ]";
        auto indicator_style = is_checked
            ? Style{}.with_fg(Color::rgb(152, 195, 121)).with_bold()  // green
            : Style{}.with_fg(Color::rgb(92, 99, 112));               // dim

        if (focused) {
            indicator_style = is_checked
                ? Style{}.with_fg(Color::rgb(152, 195, 121)).with_bold()
                : Style{}.with_fg(Color::rgb(97, 175, 239));  // blue when focused
        }

        runs.push_back(StyledRun{content.size(), indicator.size(), indicator_style});
        content += indicator;

        // Space + label
        content += " ";
        runs.push_back(StyledRun{content.size() - 1, 1, Style{}});

        auto label_style = focused
            ? Style{}.with_fg(Color::rgb(200, 204, 212))
            : Style{}.with_fg(Color::rgb(171, 178, 191));

        runs.push_back(StyledRun{content.size(), label_.size(), label_style});
        content += label_;

        return Element{TextElement{
            .content = std::move(content),
            .style = {},
            .wrap = TextWrap::NoWrap,
            .runs = std::move(runs),
        }};
    }

private:
    void toggle() {
        bool next = !checked_();
        checked_.set(next);
        if (on_change_) on_change_(next);
    }
};

// ============================================================================
// ToggleSwitch — sliding indicator style: ◯━━━ / ━━━●
// ============================================================================

class ToggleSwitch {
    Signal<bool> on_{false};
    FocusNode    focus_;
    std::string  label_;

    std::move_only_function<void(bool)> on_change_;

public:
    ToggleSwitch() = default;
    explicit ToggleSwitch(std::string label, bool initial = false)
        : on_(initial), label_(std::move(label)) {}

    // -- Signal access --
    [[nodiscard]] const Signal<bool>& on()           const { return on_; }
    [[nodiscard]] const FocusNode& focus_node()      const { return focus_; }
    [[nodiscard]] FocusNode& focus_node()                   { return focus_; }
    [[nodiscard]] std::string_view label()           const { return label_; }

    void set_label(std::string_view l)  { label_ = std::string{l}; }
    void set_on(bool v)                 { on_.set(v); }

    template <std::invocable<bool> F>
    void on_change(F&& fn) { on_change_ = std::forward<F>(fn); }

    // -- Key event handling --
    [[nodiscard]] bool handle(const KeyEvent& ev) {
        if (!focus_.focused()) return false;

        return std::visit(overload{
            [&](SpecialKey sk) -> bool {
                switch (sk) {
                    case SpecialKey::Enter:
                    case SpecialKey::Tab:
                        toggle();
                        return true;
                    default: return false;
                }
            },
            [&](CharKey ck) -> bool {
                if (ck.codepoint == ' ') { toggle(); return true; }
                return false;
            },
        }, ev.key);
    }

    // -- Node concept --
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        bool is_on = on_();
        bool focused = focus_.focused();

        std::string content;
        std::vector<StyledRun> runs;

        // Track: "━━━●" when on, "◯━━━" when off
        if (is_on) {
            // ━━━●
            std::string track = "\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81";  // ━━━
            std::string knob  = "\xe2\x97\x8f";                            // ●
            auto track_style = Style{}.with_fg(Color::rgb(152, 195, 121)); // green
            auto knob_style  = Style{}.with_fg(Color::rgb(152, 195, 121)).with_bold();
            runs.push_back(StyledRun{content.size(), track.size(), track_style});
            content += track;
            runs.push_back(StyledRun{content.size(), knob.size(), knob_style});
            content += knob;
        } else {
            // ◯━━━
            std::string knob  = "\xe2\x97\xaf";                            // ◯
            std::string track = "\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81";  // ━━━
            auto knob_style  = Style{}.with_fg(Color::rgb(92, 99, 112));
            auto track_style = Style{}.with_fg(Color::rgb(92, 99, 112));
            runs.push_back(StyledRun{content.size(), knob.size(), knob_style});
            content += knob;
            runs.push_back(StyledRun{content.size(), track.size(), track_style});
            content += track;
        }

        // Space + label
        content += "  ";
        runs.push_back(StyledRun{content.size() - 2, 2, Style{}});

        auto label_style = focused
            ? Style{}.with_fg(Color::rgb(200, 204, 212))
            : Style{}.with_fg(Color::rgb(171, 178, 191));

        runs.push_back(StyledRun{content.size(), label_.size(), label_style});
        content += label_;

        return Element{TextElement{
            .content = std::move(content),
            .style = {},
            .wrap = TextWrap::NoWrap,
            .runs = std::move(runs),
        }};
    }

private:
    void toggle() {
        bool next = !on_();
        on_.set(next);
        if (on_change_) on_change_(next);
    }
};

} // namespace maya
