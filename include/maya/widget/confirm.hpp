#pragma once
// maya::widget::confirm — Inline yes/no confirmation prompt
//
// Usage:
//   Confirm dialog("Delete this file?");
//   dialog.on_confirm([](bool yes) { if (yes) do_delete(); });
//
//   // in event handler:
//   if (auto* ke = as_key(ev)) dialog.handle(*ke);

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "../core/focus.hpp"
#include "../core/overload.hpp"
#include "../core/signal.hpp"
#include "../element/builder.hpp"
#include "../style/style.hpp"
#include "../terminal/input.hpp"

namespace maya {

struct ConfirmConfig {
    std::string yes_label = "Y";
    std::string no_label  = "n";
    bool default_yes      = true;
    Style prompt_style    = Style{}.with_bold();
    Style key_style       = Style{}.with_fg(Color::rgb(100, 200, 255));
    Style yes_style       = Style{}.with_fg(Color::rgb(80, 220, 120));
    Style no_style        = Style{}.with_fg(Color::rgb(255, 80, 80));
};

class Confirm {
    std::string prompt_;
    std::optional<bool> result_;
    FocusNode focus_;
    ConfirmConfig cfg_;
    std::move_only_function<void(bool)> on_confirm_;

public:
    explicit Confirm(std::string_view prompt, ConfirmConfig cfg = {})
        : prompt_(prompt), cfg_(std::move(cfg)) {}

    [[nodiscard]] bool answered() const { return result_.has_value(); }
    [[nodiscard]] std::optional<bool> result() const { return result_; }
    [[nodiscard]] FocusNode& focus_node() { return focus_; }

    template <std::invocable<bool> F>
    void on_confirm(F&& fn) { on_confirm_ = std::forward<F>(fn); }

    void reset() { result_.reset(); }

    [[nodiscard]] bool handle(const KeyEvent& ev) {
        if (!focus_.focused() || result_.has_value()) return false;

        return std::visit(overload{
            [&](CharKey ck) -> bool {
                if (ck.codepoint == 'y' || ck.codepoint == 'Y') {
                    confirm(true); return true;
                }
                if (ck.codepoint == 'n' || ck.codepoint == 'N') {
                    confirm(false); return true;
                }
                return false;
            },
            [&](SpecialKey sk) -> bool {
                if (sk == SpecialKey::Enter) {
                    confirm(cfg_.default_yes); return true;
                }
                if (sk == SpecialKey::Escape) {
                    confirm(false); return true;
                }
                return false;
            },
        }, ev.key);
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        if (result_.has_value()) {
            // Show the chosen answer
            return detail::hstack()(
                Element{TextElement{.content = prompt_ + " ", .style = cfg_.prompt_style}},
                Element{TextElement{
                    .content = *result_ ? "Yes" : "No",
                    .style = *result_ ? cfg_.yes_style : cfg_.no_style,
                }}
            );
        }

        // Show the prompt with key hints
        std::string hint = "[" + cfg_.yes_label + "/" + cfg_.no_label + "]";
        return detail::hstack()(
            Element{TextElement{.content = prompt_ + " ", .style = cfg_.prompt_style}},
            Element{TextElement{.content = std::move(hint), .style = cfg_.key_style}},
            Element{TextElement{.content = " ", .style = Style{}}}
        );
    }

private:
    void confirm(bool value) {
        result_ = value;
        if (on_confirm_) on_confirm_(value);
    }
};

} // namespace maya
