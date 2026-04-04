#pragma once
// maya::components::List — Scrollable list with keyboard selection
//
//   List<std::string> list({.items = {"apple", "banana", "cherry"}});
//
//   // In event handler:
//   list.update(ev);  // handles Up/Down/Enter/PgUp/PgDn
//
//   // In render:
//   list.render([](const std::string& item, int i, bool selected) {
//       return text((selected ? "▸ " : "  ") + item,
//                   selected ? Style{}.with_bold().with_fg(palette().primary)
//                            : Style{}.with_fg(palette().text));
//   })

#include "core.hpp"

namespace maya::components {

template <typename T>
struct ListProps {
    std::vector<T> items      = {};
    int            max_visible = 10;
    bool           wrap       = true;   // wrap around at boundaries
};

template <typename T>
class List {
    int selected_ = 0;
    int scroll_   = 0;
    ListProps<T> props_;

    void clamp() {
        int n = static_cast<int>(props_.items.size());
        if (n == 0) { selected_ = 0; return; }
        if (selected_ < 0) selected_ = props_.wrap ? n - 1 : 0;
        if (selected_ >= n) selected_ = props_.wrap ? 0 : n - 1;
        ensure_visible();
    }

    void ensure_visible() {
        if (selected_ < scroll_) scroll_ = selected_;
        if (selected_ >= scroll_ + props_.max_visible)
            scroll_ = selected_ - props_.max_visible + 1;
        if (scroll_ < 0) scroll_ = 0;
    }

public:
    explicit List(ListProps<T> props = {})
        : props_(std::move(props)) {}

    // ── State access ─────────────────────────────────────────────────────────

    [[nodiscard]] int selected() const { return selected_; }
    void set_selected(int i) { selected_ = i; clamp(); }

    [[nodiscard]] const T* selected_item() const {
        if (props_.items.empty()) return nullptr;
        return &props_.items[selected_];
    }

    void set_items(std::vector<T> items) {
        props_.items = std::move(items);
        clamp();
    }

    [[nodiscard]] const std::vector<T>& items() const { return props_.items; }
    [[nodiscard]] bool empty() const { return props_.items.empty(); }
    [[nodiscard]] int size() const { return static_cast<int>(props_.items.size()); }

    // ── Event handling ───────────────────────────────────────────────────────

    bool update(const Event& ev) {
        if (props_.items.empty()) return false;

        if (key(ev, SpecialKey::Up) || key(ev, 'k')) {
            --selected_; clamp(); return true;
        }
        if (key(ev, SpecialKey::Down) || key(ev, 'j')) {
            ++selected_; clamp(); return true;
        }
        if (key(ev, SpecialKey::PageUp)) {
            selected_ -= props_.max_visible; clamp(); return true;
        }
        if (key(ev, SpecialKey::PageDown)) {
            selected_ += props_.max_visible; clamp(); return true;
        }
        if (key(ev, SpecialKey::Home)) {
            selected_ = 0; clamp(); return true;
        }
        if (key(ev, SpecialKey::End)) {
            selected_ = static_cast<int>(props_.items.size()) - 1; clamp(); return true;
        }
        if (scrolled_up(ev)) {
            --selected_; clamp(); return true;
        }
        if (scrolled_down(ev)) {
            ++selected_; clamp(); return true;
        }

        return false;
    }

    // ── Render ───────────────────────────────────────────────────────────────
    // render_item: (const T& item, int index, bool selected) -> Element

    template <typename RenderFn>
    [[nodiscard]] Element render(RenderFn&& render_item) const {
        using namespace maya::dsl;

        int n = static_cast<int>(props_.items.size());
        if (n == 0) {
            return text("  (empty)", Style{}.with_fg(palette().dim).with_italic());
        }

        int vis_end = std::min(n, scroll_ + props_.max_visible);
        std::vector<Element> rows;
        rows.reserve(vis_end - scroll_);

        for (int i = scroll_; i < vis_end; ++i) {
            rows.push_back(render_item(props_.items[i], i, i == selected_));
        }

        // Scroll indicators
        if (n > props_.max_visible) {
            std::string indicator;
            if (scroll_ > 0) indicator += "↑ ";
            indicator += std::to_string(selected_ + 1) + "/" + std::to_string(n);
            if (vis_end < n) indicator += " ↓";
            rows.push_back(text(indicator, Style{}.with_fg(palette().dim)));
        }

        return vstack()(std::move(rows));
    }
};

} // namespace maya::components
