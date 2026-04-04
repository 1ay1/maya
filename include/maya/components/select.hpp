#pragma once
// maya::components::Select — Dropdown selector (model picker, mode picker, etc.)
//
//   Select<std::string> selector({
//       .items = {"claude-opus-4-6", "claude-sonnet-4-6", "gpt-4o"},
//       .selected = 0,
//       .label = "Model"
//   });
//
//   // In event handler:
//   selector.update(ev);
//
//   // Render:
//   selector.render()   // closed: "Model: claude-opus-4-6 ▾"
//                       // open: bordered list of options

#include "core.hpp"

namespace maya::components {

template <typename T>
struct SelectProps {
    std::vector<T> items = {};
    int    selected = 0;
    std::string label = "";
    Color  color = palette().primary;
    // Format function: item -> display string
    std::function<std::string(const T&)> format = [](const T& t) {
        if constexpr (std::convertible_to<T, std::string>) return std::string(t);
        else if constexpr (std::convertible_to<T, std::string_view>) return std::string(std::string_view(t));
        else return std::string("?");
    };
};

template <typename T>
class Select {
    int selected_ = 0;
    bool open_ = false;
    int hover_ = 0;
    SelectProps<T> props_;

public:
    explicit Select(SelectProps<T> props = {})
        : selected_(props.selected), hover_(props.selected), props_(std::move(props)) {}

    [[nodiscard]] int selected() const { return selected_; }
    [[nodiscard]] bool is_open() const { return open_; }
    void set_selected(int i) { selected_ = i; }
    void open() { open_ = true; hover_ = selected_; }
    void close() { open_ = false; }
    void toggle() { open_ ? close() : open(); }

    [[nodiscard]] const T* selected_item() const {
        if (selected_ >= 0 && selected_ < static_cast<int>(props_.items.size()))
            return &props_.items[selected_];
        return nullptr;
    }

    void set_items(std::vector<T> items) {
        props_.items = std::move(items);
        if (selected_ >= static_cast<int>(props_.items.size()))
            selected_ = props_.items.empty() ? 0 : static_cast<int>(props_.items.size()) - 1;
    }

    bool update(const Event& ev) {
        int n = static_cast<int>(props_.items.size());
        if (n == 0) return false;

        if (!open_) {
            if (key(ev, SpecialKey::Enter) || key(ev, ' ')) {
                open(); return true;
            }
            // Left/right to cycle when closed
            if (key(ev, SpecialKey::Left)) {
                selected_ = (selected_ - 1 + n) % n; return true;
            }
            if (key(ev, SpecialKey::Right)) {
                selected_ = (selected_ + 1) % n; return true;
            }
            return false;
        }

        // Open state
        if (key(ev, SpecialKey::Up) || key(ev, 'k')) {
            hover_ = (hover_ - 1 + n) % n; return true;
        }
        if (key(ev, SpecialKey::Down) || key(ev, 'j')) {
            hover_ = (hover_ + 1) % n; return true;
        }
        if (key(ev, SpecialKey::Enter) || key(ev, ' ')) {
            selected_ = hover_; close(); return true;
        }
        if (key(ev, SpecialKey::Escape)) {
            close(); return true;
        }

        return false;
    }

    [[nodiscard]] Element render() const {
        using namespace maya::dsl;
        auto& p = palette();

        std::string display = props_.items.empty() ? "(none)"
            : props_.format(props_.items[selected_]);

        // Trigger button
        std::vector<Element> trigger;
        if (!props_.label.empty()) {
            trigger.push_back(text(props_.label + ": ",
                                    Style{}.with_fg(p.muted)));
        }
        trigger.push_back(text(display, Style{}.with_fg(props_.color)));
        trigger.push_back(text(open_ ? " ▴" : " ▾", Style{}.with_fg(p.dim)));

        if (!open_) {
            return hstack()(std::move(trigger));
        }

        // Open dropdown
        std::vector<Element> rows;
        rows.push_back(hstack()(std::move(trigger)));

        std::vector<Element> options;
        for (int i = 0; i < static_cast<int>(props_.items.size()); ++i) {
            bool is_selected = (i == selected_);
            bool is_hover = (i == hover_);
            std::string item_text = props_.format(props_.items[i]);

            Style s;
            if (is_hover) {
                s = Style{}.with_bold().with_fg(p.primary).with_bg(Color::rgb(30, 30, 45));
            } else if (is_selected) {
                s = Style{}.with_fg(props_.color);
            } else {
                s = Style{}.with_fg(p.text);
            }

            std::string prefix = is_selected ? " ✓ " : "   ";
            options.push_back(text(prefix + item_text + " ", s));
        }

        rows.push_back(
            vstack()
                .border(BorderStyle::Round)
                .border_color(p.border)
                .padding(0, 0, 0, 0)(std::move(options))
        );

        return vstack()(std::move(rows));
    }
};

} // namespace maya::components
