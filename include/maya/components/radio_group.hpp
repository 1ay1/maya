#pragma once
// maya::components::RadioGroup — Inline radio selector (all options visible)
//
//   RadioGroup<std::string> radios({
//       .items = {"Dark", "Light", "System"},
//       .selected = 0,
//   });
//
//   // In event handler:
//   radios.update(ev);  // handles Up/Down/j/k, Left/Right if horizontal, 1-9
//
//   // In render:
//   radios.render()   // vertical:   ◉ Dark
//                     //              ○ Light
//                     //              ○ System

#include "core.hpp"

namespace maya::components {

template <typename T>
struct RadioGroupProps {
    std::vector<T> items      = {};
    int            selected   = 0;
    bool           horizontal = false;
    Color          color      = {};  // default = palette().primary
    std::function<std::string(const T&)> format = [](const T& t) {
        if constexpr (std::convertible_to<T, std::string>) return std::string(t);
        else if constexpr (std::convertible_to<T, std::string_view>) return std::string(std::string_view(t));
        else return std::string("?");
    };
};

template <typename T>
class RadioGroup {
    int selected_ = 0;
    RadioGroupProps<T> props_;

    Color effective_color() const {
        return props_.color == Color{} ? palette().primary : props_.color;
    }

public:
    explicit RadioGroup(RadioGroupProps<T> props = {})
        : selected_(props.selected), props_(std::move(props)) {}

    [[nodiscard]] int selected() const { return selected_; }

    [[nodiscard]] const T* selected_item() const {
        if (selected_ >= 0 && selected_ < static_cast<int>(props_.items.size()))
            return &props_.items[selected_];
        return nullptr;
    }

    void set_selected(int i) {
        int n = static_cast<int>(props_.items.size());
        if (n > 0 && i >= 0 && i < n) selected_ = i;
    }

    bool update(const Event& ev) {
        int n = static_cast<int>(props_.items.size());
        if (n == 0) return false;

        // Prev/next based on orientation
        bool prev = props_.horizontal
            ? (key(ev, SpecialKey::Left) || key(ev, 'k'))
            : (key(ev, SpecialKey::Up) || key(ev, 'k'));
        bool next = props_.horizontal
            ? (key(ev, SpecialKey::Right) || key(ev, 'j'))
            : (key(ev, SpecialKey::Down) || key(ev, 'j'));

        if (prev) { selected_ = (selected_ - 1 + n) % n; return true; }
        if (next) { selected_ = (selected_ + 1) % n; return true; }

        // Number keys 1-9 for direct selection
        for (int d = 1; d <= 9 && d <= n; ++d) {
            if (key(ev, static_cast<char>('0' + d))) {
                selected_ = d - 1; return true;
            }
        }

        return false;
    }

    [[nodiscard]] Element render() const {
        using namespace maya::dsl;
        auto& p = palette();
        Color col = effective_color();

        int n = static_cast<int>(props_.items.size());
        if (n == 0) {
            return text("  (empty)", Style{}.with_fg(p.dim).with_italic());
        }

        std::vector<Element> elems;
        elems.reserve(n);

        for (int i = 0; i < n; ++i) {
            bool sel = (i == selected_);
            std::string label = (sel ? "\u25C9 " : "\u25CB ") + props_.format(props_.items[i]);
            Style s = sel ? Style{}.with_bold().with_fg(col)
                          : Style{}.with_fg(p.text);
            elems.push_back(text(label, s));
        }

        if (props_.horizontal) {
            return hstack().gap(2)(std::move(elems));
        }
        return vstack()(std::move(elems));
    }
};

} // namespace maya::components
