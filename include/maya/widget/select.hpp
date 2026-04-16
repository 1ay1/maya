#pragma once
// maya::widget::select — Interactive selection menu
//
// Arrow-key navigable list of options with focus integration.
// Supports both static labels and dynamic items.
//
// Usage:
//   Select menu({"Option A", "Option B", "Option C"});
//   menu.on_select([](int idx, std::string_view label) {
//       // handle selection
//   });
//   auto ui = v(text("Choose:") | Bold, menu);

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
// SelectConfig — appearance configuration
// ============================================================================

struct SelectConfig {
    std::string indicator       = "\xe2\x9d\xaf ";   // "❯ " — Zed-style chevron
    std::string inactive_prefix = "  ";   // shown next to inactive items
    Style active_style   = Style{}.with_bold().with_fg(Color::blue());
    Style inactive_style = Style{}.with_dim();
    int visible_count    = 0;  // 0 = show all items
};

// ============================================================================
// Select — interactive selection widget
// ============================================================================

class Select {
    std::vector<std::string> items_;
    Signal<int> cursor_{0};
    FocusNode focus_;
    SelectConfig cfg_;

    std::move_only_function<void(int, std::string_view)> on_select_;

public:
    explicit Select(std::vector<std::string> items, SelectConfig cfg = {})
        : items_(std::move(items)), cfg_(std::move(cfg)) {}

    Select(std::initializer_list<std::string_view> items, SelectConfig cfg = {})
        : cfg_(std::move(cfg))
    {
        items_.reserve(items.size());
        for (auto sv : items) items_.emplace_back(sv);
    }

    // -- Accessors --
    [[nodiscard]] const Signal<int>& cursor()     const { return cursor_; }
    [[nodiscard]] FocusNode& focus_node()                { return focus_; }
    [[nodiscard]] const FocusNode& focus_node()    const { return focus_; }
    [[nodiscard]] int size()                       const { return static_cast<int>(items_.size()); }

    [[nodiscard]] std::string_view selected_label() const {
        int idx = cursor_();
        if (idx >= 0 && idx < static_cast<int>(items_.size()))
            return items_[static_cast<size_t>(idx)];
        return {};
    }

    // -- Callback --
    template <std::invocable<int, std::string_view> F>
    void on_select(F&& fn) { on_select_ = std::forward<F>(fn); }

    // -- Mutation --
    void set_items(std::vector<std::string> items) {
        items_ = std::move(items);
        if (cursor_() >= static_cast<int>(items_.size()))
            cursor_.set(std::max(0, static_cast<int>(items_.size()) - 1));
    }

    // -- Key event handling --
    [[nodiscard]] bool handle(const KeyEvent& ev) {
        if (!focus_.focused()) return false;
        if (items_.empty()) return false;

        return std::visit(overload{
            [&](SpecialKey sk) -> bool {
                switch (sk) {
                    case SpecialKey::Up:   move_up();   return true;
                    case SpecialKey::Down: move_down(); return true;
                    case SpecialKey::Enter:
                        if (on_select_) on_select_(cursor_(), items_[static_cast<size_t>(cursor_())]);
                        return true;
                    default: return false;
                }
            },
            [&](CharKey ck) -> bool {
                if (ck.codepoint == 'j') { move_down(); return true; }
                if (ck.codepoint == 'k') { move_up();   return true; }
                return false;
            },
        }, ev.key);
    }

    /// Handle mouse events for click selection and scroll wheel navigation.
    /// render_y_start: the terminal row where this widget's first item is rendered.
    /// Returns true if the event was consumed.
    bool handle_mouse(const MouseEvent& me, int render_y_start = 0) {
        if (items_.empty()) return false;

        if (me.kind == MouseEventKind::Press && me.button == MouseButton::Left) {
            int clicked_row = me.y.value - render_y_start;
            if (clicked_row >= 0 && clicked_row < static_cast<int>(items_.size())) {
                cursor_.set(clicked_row);
                if (on_select_) on_select_(cursor_(), items_[static_cast<size_t>(cursor_())]);
                return true;
            }
        }
        // Scroll wheel
        if (me.kind == MouseEventKind::Press) {
            if (me.button == MouseButton::ScrollUp)   { move_up();   return true; }
            if (me.button == MouseButton::ScrollDown) { move_down(); return true; }
        }
        return false;
    }

private:
    void move_up() {
        int c = cursor_();
        cursor_.set(c > 0 ? c - 1 : static_cast<int>(items_.size()) - 1);
    }

    void move_down() {
        int c = cursor_();
        cursor_.set((c + 1) % static_cast<int>(items_.size()));
    }

public:
    // -- Node concept: build into Element --
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        int cur = cursor_();
        bool focused = focus_.focused();

        int total = static_cast<int>(items_.size());
        int start = 0;
        int count = total;

        // Windowed display if visible_count is set
        if (cfg_.visible_count > 0 && cfg_.visible_count < total) {
            count = cfg_.visible_count;
            start = std::max(0, cur - count / 2);
            if (start + count > total) start = total - count;
        }

        std::vector<Element> rows;
        rows.reserve(static_cast<size_t>(count));

        for (int i = start; i < start + count; ++i) {
            bool active = (i == cur);
            std::string line;
            if (active) {
                line = cfg_.indicator + items_[static_cast<size_t>(i)];
            } else {
                line = cfg_.inactive_prefix + items_[static_cast<size_t>(i)];
            }

            Style s = active
                ? (focused ? cfg_.active_style : Style{}.with_bold())
                : cfg_.inactive_style;

            rows.push_back(Element{TextElement{
                .content = std::move(line),
                .style = s,
            }});
        }

        return dsl::v(std::move(rows)).build();
    }
};

} // namespace maya
