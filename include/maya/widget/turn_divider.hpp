#pragma once
// maya::widget::turn_divider — Visual separator between conversation turns
//
// Renders a styled horizontal rule with optional turn number and role
// label. Used to visually separate user/assistant turns in a conversation.
//
//   TurnDivider div("user", 3);
//   auto ui = div.build();

#include <string>

#include "../dsl.hpp"
#include "../style/style.hpp"

namespace maya {

enum class TurnRole : uint8_t {
    User,
    Assistant,
    System,
    Tool,
};

class TurnDivider {
    TurnRole role_       = TurnRole::User;
    int      turn_num_   = 0;
    bool     show_role_  = true;

    [[nodiscard]] Color role_color() const {
        switch (role_) {
            case TurnRole::User:      return Color::blue();
            case TurnRole::Assistant: return Color::magenta();
            case TurnRole::System:    return Color::yellow();
            case TurnRole::Tool:      return Color::cyan();
        }
        return Color::bright_black();
    }

    [[nodiscard]] const char* role_label() const {
        switch (role_) {
            case TurnRole::User:      return "You";
            case TurnRole::Assistant: return "Claude";
            case TurnRole::System:    return "System";
            case TurnRole::Tool:      return "Tool";
        }
        return "";
    }

    [[nodiscard]] const char* role_icon() const {
        switch (role_) {
            case TurnRole::User:      return "\xe2\x9d\xaf"; // ❯
            case TurnRole::Assistant: return "\xe2\x9c\xa6"; // ✦
            case TurnRole::System:    return "\xe2\x97\x86"; // ◆
            case TurnRole::Tool:      return "\xe2\x9a\x99"; // ⚙
        }
        return "\xe2\x94\x80";
    }

public:
    TurnDivider() = default;
    TurnDivider(TurnRole role, int turn_num = 0)
        : role_(role), turn_num_(turn_num) {}

    void set_role(TurnRole r) { role_ = r; }
    void set_turn_number(int n) { turn_num_ = n; }
    void set_show_role(bool b) { show_role_ = b; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        Color rc = role_color();
        auto rule_style = Style{}.with_fg(rc).with_dim();
        auto dim = Style{}.with_dim();

        // Build rule segments: ─── ✦ Claude #3 ───
        std::string rule;
        for (int i = 0; i < 3; ++i) rule += "\xe2\x94\x80"; // ─

        std::vector<Element> parts;
        parts.push_back(text(rule + " ", rule_style));

        if (show_role_) {
            parts.push_back(text(role_icon(), Style{}.with_fg(rc)));
            parts.push_back(text(std::string(" ") + role_label(),
                Style{}.with_fg(rc).with_bold()));
        }

        if (turn_num_ > 0) {
            parts.push_back(text(" #" + std::to_string(turn_num_), dim));
        }

        parts.push_back(text(" " + rule, rule_style));

        return h(std::move(parts)).build();
    }
};

} // namespace maya
