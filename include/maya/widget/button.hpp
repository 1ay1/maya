#pragma once
// maya::widget::button — Clickable button with variant styles
//
// A focusable button that triggers an action on Enter/Space.
// Supports multiple visual variants for different semantic contexts.
//
// Primary (blue fill):
//   ╭──────────────╮
//   │  Submit      │    (blue bg, white text)
//   ╰──────────────╯
//
// Default:
//   ╭──────────────╮
//   │  Cancel      │    (blue border when focused, dim otherwise)
//   ╰──────────────╯
//
// Ghost (no border):
//   Skip
//
// Danger (red):
//   ╭──────────────╮
//   │  Delete      │    (red border/fill)
//   ╰──────────────╯
//
// Usage:
//   Button submit("Submit", [] { save(); });
//   Button cancel("Cancel", [] { close(); }, ButtonVariant::Ghost);
//   auto ui = h(submit, text(" "), cancel);

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "../core/focus.hpp"
#include "../core/overload.hpp"
#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/border.hpp"
#include "../style/style.hpp"
#include "../terminal/input.hpp"

namespace maya {

// ============================================================================
// ButtonVariant — visual style presets
// ============================================================================

enum class ButtonVariant : uint8_t {
    Default,   // bordered, blue border when focused
    Primary,   // blue background fill
    Danger,    // red border / fill
    Ghost,     // no border, text only
};

// ============================================================================
// Button — clickable button widget
// ============================================================================

class Button {
    std::string label_;
    FocusNode   focus_;
    ButtonVariant variant_ = ButtonVariant::Default;

    std::move_only_function<void()> on_click_;

public:
    Button() = default;

    explicit Button(std::string label,
                    std::move_only_function<void()> on_click = {},
                    ButtonVariant variant = ButtonVariant::Default)
        : label_(std::move(label))
        , variant_(variant)
        , on_click_(std::move(on_click)) {}

    // -- Accessors --
    [[nodiscard]] const FocusNode& focus_node()  const { return focus_; }
    [[nodiscard]] FocusNode& focus_node()               { return focus_; }
    [[nodiscard]] std::string_view label()       const { return label_; }
    [[nodiscard]] ButtonVariant variant()         const { return variant_; }

    void set_label(std::string_view l)               { label_ = std::string{l}; }
    void set_variant(ButtonVariant v)                 { variant_ = v; }

    template <std::invocable F>
    void on_click(F&& fn) { on_click_ = std::forward<F>(fn); }

    // -- Key event handling --
    [[nodiscard]] bool handle(const KeyEvent& ev) {
        if (!focus_.focused()) return false;

        return std::visit(overload{
            [&](SpecialKey sk) -> bool {
                switch (sk) {
                    case SpecialKey::Enter:
                        click();
                        return true;
                    default: return false;
                }
            },
            [&](CharKey ck) -> bool {
                if (ck.codepoint == ' ') { click(); return true; }
                return false;
            },
        }, ev.key);
    }

    // -- Node concept --
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        bool focused = focus_.focused();

        switch (variant_) {
            case ButtonVariant::Ghost:  return build_ghost(focused);
            case ButtonVariant::Primary: return build_filled(focused,
                Color::blue(),   // blue bg
                Color::white()); // white text
            case ButtonVariant::Danger: return build_filled(focused,
                Color::red(),    // red bg
                Color::white()); // white text
            case ButtonVariant::Default:
            default: return build_default(focused);
        }
    }

private:
    void click() {
        if (on_click_) on_click_();
    }

    [[nodiscard]] Element build_default(bool focused) const {
        auto border_color = focused
            ? Color::blue()           // blue
            : Color::bright_black();  // dim border

        auto text_style = focused
            ? Style{}.with_bold()
            : Style{};

        auto inner = Element{TextElement{
            .content = label_,
            .style = text_style,
            .wrap = TextWrap::NoWrap,
        }};

        return (dsl::v(std::move(inner))
            | dsl::border(BorderStyle::Round)
            | dsl::bcolor(border_color)
            | dsl::padding(0, 1, 0, 1)).build();
    }

    [[nodiscard]] Element build_filled(bool focused, Color bg_color, Color text_color) const {
        auto text_style = Style{}.with_fg(text_color).with_bold();
        auto bg_style = focused
            ? bg_color
            : Color::bright_black();  // dim when not focused

        auto inner = Element{TextElement{
            .content = label_,
            .style = text_style,
            .wrap = TextWrap::NoWrap,
        }};

        auto border_color = focused ? bg_color : Color::bright_black();

        return (dsl::v(std::move(inner))
            | dsl::border(BorderStyle::Round)
            | dsl::bcolor(border_color)
            | dsl::bgc(bg_style)
            | dsl::padding(0, 1, 0, 1)).build();
    }

    [[nodiscard]] Element build_ghost(bool focused) const {
        auto text_style = focused
            ? Style{}.with_fg(Color::blue()).with_underline()
            : Style{}.with_dim();

        return Element{TextElement{
            .content = label_,
            .style = text_style,
            .wrap = TextWrap::NoWrap,
        }};
    }
};

} // namespace maya
