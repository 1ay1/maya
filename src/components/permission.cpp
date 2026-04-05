#include "maya/components/permission.hpp"

namespace maya::components {

void PermissionPrompt::update(const Event& ev) {
    if (decided()) return;

    // Dropdown navigation
    if (dropdown_open_) {
        if (key(ev, SpecialKey::Up) || key(ev, 'k')) {
            if (dropdown_idx_ > 0) --dropdown_idx_;
            return;
        }
        if (key(ev, SpecialKey::Down) || key(ev, 'j')) {
            if (dropdown_idx_ < static_cast<int>(props_.options.size()) - 1)
                ++dropdown_idx_;
            return;
        }
        if (key(ev, SpecialKey::Enter)) {
            if (!props_.options.empty())
                decision_ = props_.options[dropdown_idx_].decision;
            else
                decision_ = PermissionDecision::AllowAlways;
            dropdown_open_ = false;
            return;
        }
        if (key(ev, SpecialKey::Escape)) {
            dropdown_open_ = false;
            return;
        }
        return;
    }

    // Allow: 'a' or 'y'
    if (key(ev, 'a') || key(ev, 'y')) {
        decision_ = PermissionDecision::AllowOnce;
        return;
    }

    // Deny: 'x' or 'n'
    if (key(ev, 'x') || key(ev, 'n')) {
        decision_ = PermissionDecision::DenyOnce;
        return;
    }

    // Always allow: 'A'
    if (key(ev, 'A')) {
        if (!props_.options.empty())
            decision_ = props_.options[dropdown_idx_].decision;
        else
            decision_ = PermissionDecision::AllowAlways;
        return;
    }

    // Always deny: 'X'
    if (key(ev, 'X')) {
        decision_ = PermissionDecision::DenyAlways;
        return;
    }

    // Open dropdown: Tab or 'd'
    if (props_.style == PermissionStyle::Dropdown && !props_.options.empty()) {
        if (key(ev, SpecialKey::Tab) || key(ev, 'd')) {
            dropdown_open_ = true;
            return;
        }
    }
}

Element PermissionPrompt::render(int frame) const {
    using namespace maya::dsl;
    auto& p = palette();

    std::vector<Element> rows;

    // ── Warning banner (dangerous mode) ─────────────────────────────
    if (props_.style == PermissionStyle::Dangerous || !props_.warning.empty()) {
        std::string warn = props_.warning.empty()
            ? "This operation may be dangerous" : props_.warning;
        rows.push_back(
            hstack().gap(1)(
                text("⚠", Style{}.with_fg(p.error)),
                text(warn, Style{}.with_fg(p.error).with_bold())
            )
        );
    }

    // ── Title ───────────────────────────────────────────────────────
    if (!props_.title.empty()) {
        rows.push_back(text(props_.title, Style{}.with_fg(p.text)));
    }

    // ── Buttons ─────────────────────────────────────────────────────
    bool is_dangerous = (props_.style == PermissionStyle::Dangerous);

    auto allow_btn = Button(ButtonProps{
        .label   = "Allow",
        .icon    = "✓",
        .variant = is_dangerous ? Variant::Ghost : Variant::Tinted,
        .color   = p.success,
    });

    auto deny_btn = Button(ButtonProps{
        .label   = "Deny",
        .icon    = "✗",
        .variant = is_dangerous ? Variant::Tinted : Variant::Ghost,
        .color   = p.error,
    });

    std::vector<Element> btn_row;
    btn_row.push_back(std::move(allow_btn));
    btn_row.push_back(std::move(deny_btn));

    // Dropdown trigger (for Dropdown style)
    if (props_.style == PermissionStyle::Dropdown && !props_.options.empty()) {
        std::string current = (dropdown_idx_ < static_cast<int>(props_.options.size()))
            ? props_.options[dropdown_idx_].label : "Options";

        btn_row.push_back(Element(space));
        btn_row.push_back(
            text(current + (dropdown_open_ ? " ▴" : " ▾"),
                 Style{}.with_fg(p.muted))
        );
    }

    rows.push_back(hstack().gap(2)(std::move(btn_row)));

    // ── Dropdown (when open) ────────────────────────────────────────
    if (dropdown_open_ && !props_.options.empty()) {
        std::vector<Element> opts;
        for (int i = 0; i < static_cast<int>(props_.options.size()); ++i) {
            bool selected = (i == dropdown_idx_);
            auto& opt = props_.options[i];

            Color ic;
            std::string icon;
            switch (opt.decision) {
                case PermissionDecision::AllowOnce:
                case PermissionDecision::AllowAlways:
                    ic = p.success;
                    icon = (opt.decision == PermissionDecision::AllowAlways) ? "✓✓" : "✓ ";
                    break;
                case PermissionDecision::DenyOnce:
                case PermissionDecision::DenyAlways:
                    ic = p.error;
                    icon = "✗ ";
                    break;
                default:
                    ic = p.muted;
                    icon = "  ";
                    break;
            }

            Style label_style = selected
                ? Style{}.with_bold().with_fg(p.text)
                : Style{}.with_fg(p.muted);

            opts.push_back(
                hstack().gap(1)(
                    text(selected ? "▸" : " ", Style{}.with_fg(p.primary)),
                    text(icon, Style{}.with_fg(ic)),
                    text(opt.label, label_style)
                )
            );
        }

        auto sides = BorderSides{.top = true, .right = false, .bottom = false, .left = false};
        rows.push_back(
            vstack()
                .border(BorderStyle::Round)
                .border_color(p.dim)
                .border_sides(sides)
                .padding(0, 1, 0, 1)(std::move(opts))
        );
    }

    // ── Keyhints ────────────────────────────────────────────────────
    if (props_.show_keyhints && !dropdown_open_) {
        std::vector<KeyBindingProps> hints = {
            {.keys = "a", .label = "allow"},
            {.keys = "x", .label = "deny"},
        };
        if (props_.style == PermissionStyle::Dropdown && !props_.options.empty()) {
            hints.push_back({.keys = "A", .label = "always"});
            hints.push_back({.keys = "d", .label = "options"});
        }
        rows.push_back(KeyBindings(std::move(hints)));
    } else if (dropdown_open_) {
        rows.push_back(KeyBindings({
            {.keys = "↑↓", .label = "navigate"},
            {.keys = "Enter", .label = "select"},
            {.keys = "Esc", .label = "close"},
        }));
    }

    // Wrap in a bordered section
    auto sides = BorderSides{.top = true, .right = false, .bottom = false, .left = false};
    return vstack()
        .border(BorderStyle::Round)
        .border_color(is_dangerous ? p.error : p.warning)
        .border_sides(sides)
        .padding(0, 1, 0, 1)(std::move(rows));
}

} // namespace maya::components
