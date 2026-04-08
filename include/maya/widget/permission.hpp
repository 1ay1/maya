#pragma once
// maya::widget::permission — Tool call permission prompt
//
// Shows a permission dialog for tool/command execution approval.
// Supports Allow/Deny with optional "always allow" memory.
//
// Usage:
//   Permission perm("bash", "rm -rf build/");
//   perm.on_decide([](PermissionResult r) { ... });
//
//   // In event handler:
//   if (auto* ke = as_key(ev)) perm.handle(*ke);

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "../core/focus.hpp"
#include "../core/overload.hpp"
#include "../element/builder.hpp"
#include "../style/border.hpp"
#include "../style/style.hpp"
#include "../terminal/input.hpp"

namespace maya {

enum class PermissionResult : uint8_t {
    Allow,
    AlwaysAllow,
    Deny,
};

struct PermissionConfig {
    Style tool_style      = Style{}.with_bold().with_fg(Color::rgb(180, 140, 255));
    Style command_style   = Style{}.with_fg(Color::rgb(220, 220, 240));
    Style allow_style     = Style{}.with_bold().with_fg(Color::rgb(80, 220, 120));
    Style deny_style      = Style{}.with_bold().with_fg(Color::rgb(255, 80, 80));
    Style hint_style      = Style{}.with_dim();
    Color border_color    = Color::rgb(180, 140, 255);
    bool show_always      = true;   // show "always allow" option
};

class Permission {
    std::string tool_name_;
    std::string command_;
    std::optional<PermissionResult> result_;
    FocusNode focus_;
    PermissionConfig cfg_;
    std::move_only_function<void(PermissionResult)> on_decide_;

public:
    Permission() = default;

    explicit Permission(std::string_view tool, std::string_view command,
                        PermissionConfig cfg = {})
        : tool_name_(tool), command_(command), cfg_(std::move(cfg)) {}

    [[nodiscard]] bool answered() const { return result_.has_value(); }
    [[nodiscard]] std::optional<PermissionResult> result() const { return result_; }
    [[nodiscard]] FocusNode& focus_node() { return focus_; }

    void set_request(std::string_view tool, std::string_view command) {
        tool_name_ = std::string{tool};
        command_ = std::string{command};
        result_.reset();
    }

    void reset() { result_.reset(); }

    template <std::invocable<PermissionResult> F>
    void on_decide(F&& fn) { on_decide_ = std::forward<F>(fn); }

    [[nodiscard]] bool handle(const KeyEvent& ev) {
        if (!focus_.focused() || result_.has_value()) return false;

        return std::visit(overload{
            [&](CharKey ck) -> bool {
                if (ck.codepoint == 'y' || ck.codepoint == 'Y') {
                    decide(PermissionResult::Allow); return true;
                }
                if (ck.codepoint == 'n' || ck.codepoint == 'N') {
                    decide(PermissionResult::Deny); return true;
                }
                if (cfg_.show_always && (ck.codepoint == 'a' || ck.codepoint == 'A')) {
                    decide(PermissionResult::AlwaysAllow); return true;
                }
                return false;
            },
            [&](SpecialKey sk) -> bool {
                if (sk == SpecialKey::Enter) {
                    decide(PermissionResult::Allow); return true;
                }
                if (sk == SpecialKey::Escape) {
                    decide(PermissionResult::Deny); return true;
                }
                return false;
            },
        }, ev.key);
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        if (result_.has_value()) {
            return build_result();
        }
        return build_prompt();
    }

private:
    void decide(PermissionResult r) {
        result_ = r;
        if (on_decide_) on_decide_(r);
    }

    [[nodiscard]] Element build_prompt() const {
        // Tool name + command
        auto tool_line = detail::hstack()(
            Element{TextElement{.content = tool_name_, .style = cfg_.tool_style}},
            Element{TextElement{.content = " ", .style = Style{}}},
            Element{TextElement{.content = command_, .style = cfg_.command_style}});

        // Key hints
        std::string hints = cfg_.show_always
            ? "[Y]es  [A]lways  [N]o"
            : "[Y]es  [N]o";
        auto hint_line = Element{TextElement{
            .content = std::move(hints), .style = cfg_.hint_style}};

        return detail::vstack()
            .border(BorderStyle::Round)
            .border_color(cfg_.border_color)
            .border_text("Allow tool?", BorderTextPos::Top)
            .padding(0, 1, 0, 1)(
                std::move(tool_line),
                Element{TextElement{.content = "", .style = Style{}}},
                std::move(hint_line));
    }

    [[nodiscard]] Element build_result() const {
        const char* label = "Denied";
        Style s = cfg_.deny_style;
        if (*result_ == PermissionResult::Allow) {
            label = "Allowed";
            s = cfg_.allow_style;
        } else if (*result_ == PermissionResult::AlwaysAllow) {
            label = "Always allowed";
            s = cfg_.allow_style;
        }

        return detail::hstack()(
            Element{TextElement{.content = tool_name_ + " ", .style = cfg_.tool_style}},
            Element{TextElement{.content = label, .style = s}});
    }
};

} // namespace maya
