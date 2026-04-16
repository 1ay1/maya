#pragma once
// maya::widget::modal — Modal dialog box with title, content, and action buttons
//
// A bordered dialog with Tab-navigable buttons, Enter to activate, Escape to dismiss.
//
// Usage:
//   Modal modal("Confirm", text("Are you sure?"), {
//       {"Cancel", ModalButton::Default, [&]{ modal.hide(); }},
//       {"OK",     ModalButton::Primary, [&]{ do_thing(); modal.hide(); }},
//   });
//   modal.show();
//   auto ui = modal.build();

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "../core/focus.hpp"
#include "../core/overload.hpp"
#include "../core/signal.hpp"
#include "../dsl.hpp"
#include "../style/style.hpp"
#include "../terminal/input.hpp"

namespace maya {

// ============================================================================
// ModalButton — a labeled action button for the modal footer
// ============================================================================

struct ModalButton {
    enum Variant : uint8_t { Default, Primary, Danger };

    std::string label;
    Variant variant = Default;
    std::move_only_function<void()> callback;
};

// ============================================================================
// Modal — interactive dialog widget
// ============================================================================

class Modal {
    std::string title_;
    Element content_;
    std::vector<ModalButton> buttons_;
    bool visible_ = false;
    FocusNode focus_;
    Signal<int> focused_button_{0};

public:
    Modal() = default;

    Modal(std::string title, Element content, std::vector<ModalButton> buttons)
        : title_(std::move(title))
        , content_(std::move(content))
        , buttons_(std::move(buttons)) {}

    // -- Visibility --
    void show()  { visible_ = true; }
    void hide()  { visible_ = false; }
    [[nodiscard]] bool visible() const { return visible_; }

    // -- Accessors --
    void set_content(Element content) { content_ = std::move(content); }
    void set_title(std::string_view t) { title_ = std::string{t}; }
    [[nodiscard]] FocusNode& focus_node() { return focus_; }
    [[nodiscard]] const FocusNode& focus_node() const { return focus_; }
    [[nodiscard]] const Signal<int>& focused_button() const { return focused_button_; }

    // -- Key event handling --
    [[nodiscard]] bool handle(const KeyEvent& ev) {
        if (!visible_ || !focus_.focused()) return false;

        return std::visit(overload{
            [&](SpecialKey sk) -> bool {
                switch (sk) {
                    case SpecialKey::Tab:
                        cycle_next();
                        return true;
                    case SpecialKey::BackTab:
                        cycle_prev();
                        return true;
                    case SpecialKey::Enter:
                        activate();
                        return true;
                    case SpecialKey::Escape:
                        hide();
                        return true;
                    default: return false;
                }
            },
            [&](CharKey) -> bool { return false; },
        }, ev.key);
    }

    // -- Build --
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        if (!visible_) {
            return Element{TextElement{.content = ""}};
        }

        // Content row
        auto body = content_;

        // Build button row
        std::vector<Element> btn_elements;
        btn_elements.reserve(buttons_.size());

        int cur = focused_button_();
        bool focused = focus_.focused();

        for (int i = 0; i < static_cast<int>(buttons_.size()); ++i) {
            const auto& btn = buttons_[static_cast<size_t>(i)];
            bool active = (i == cur) && focused;

            // Determine button colors
            Color fg_color = Color::white();
            Color border_color = Color::bright_black();

            switch (btn.variant) {
                case ModalButton::Primary:
                    fg_color = Color::blue();
                    break;
                case ModalButton::Danger:
                    fg_color = Color::red();
                    break;
                case ModalButton::Default:
                    break;
            }

            if (active) {
                border_color = Color::blue();
            }

            std::string label = " " + btn.label + " ";
            auto btn_elem = (dsl::v(Element{TextElement{
                    .content = std::move(label),
                    .style = Style{}.with_fg(fg_color).with_bold(),
                }}) | dsl::border(BorderStyle::Round) | dsl::bcolor(border_color)).build();

            btn_elements.push_back(std::move(btn_elem));
        }

        auto button_row = (dsl::h(std::move(btn_elements)) | dsl::gap(1) | dsl::justify(Justify::End)).build();

        // Assemble the modal
        auto inner = (dsl::v(std::move(body), std::move(button_row)) | dsl::gap(1)).build();

        auto border_color = Color::bright_black();
        return (dsl::v(std::move(inner))
            | dsl::border(BorderStyle::Round) | dsl::bcolor(border_color)
            | dsl::btext(title_, BorderTextPos::Top)
            | dsl::padding(1, 2)).build();
    }

private:
    void cycle_next() {
        if (buttons_.empty()) return;
        int c = focused_button_();
        focused_button_.set((c + 1) % static_cast<int>(buttons_.size()));
    }

    void cycle_prev() {
        if (buttons_.empty()) return;
        int c = focused_button_();
        focused_button_.set(c > 0 ? c - 1 : static_cast<int>(buttons_.size()) - 1);
    }

    void activate() {
        int idx = focused_button_();
        if (idx >= 0 && idx < static_cast<int>(buttons_.size())) {
            if (buttons_[static_cast<size_t>(idx)].callback) {
                buttons_[static_cast<size_t>(idx)].callback();
            }
        }
    }
};

} // namespace maya
