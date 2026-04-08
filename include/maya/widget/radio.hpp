#pragma once
// maya::widget::radio — Radio button group with single selection
//
// Arrow-key navigable group of mutually exclusive options with focus integration.
//
//   ○ Light mode
//   ● Dark mode
//   ○ System default
//
// Usage:
//   Radio theme({"Light mode", "Dark mode", "System default"});
//   theme.on_change([](int idx, std::string_view label) {
//       apply_theme(label);
//   });
//   auto ui = v(text("Theme:") | Bold, theme);

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "../core/focus.hpp"
#include "../core/overload.hpp"
#include "../core/signal.hpp"
#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/style.hpp"
#include "../terminal/input.hpp"

namespace maya {

// ============================================================================
// RadioConfig — appearance configuration
// ============================================================================

struct RadioConfig {
    std::string selected_indicator   = "\xe2\x97\x8f ";  // "● "
    std::string unselected_indicator = "\xe2\x97\x8b ";  // "○ "
    Style selected_style   = Style{}.with_bold().with_fg(Color::rgb(97, 175, 239));
    Style unselected_style = Style{}.with_fg(Color::rgb(150, 156, 170));
    int visible_count      = 0;  // 0 = show all
};

// ============================================================================
// Radio — radio button group widget
// ============================================================================

class Radio {
    std::vector<std::string> items_;
    Signal<int> selected_{0};
    Signal<int> cursor_{0};
    FocusNode   focus_;
    RadioConfig cfg_;

    std::move_only_function<void(int, std::string_view)> on_change_;

public:
    explicit Radio(std::vector<std::string> items, RadioConfig cfg = {})
        : items_(std::move(items)), cfg_(std::move(cfg)) {}

    Radio(std::initializer_list<std::string_view> items, RadioConfig cfg = {})
        : cfg_(std::move(cfg))
    {
        items_.reserve(items.size());
        for (auto sv : items) items_.emplace_back(sv);
    }

    // -- Accessors --
    [[nodiscard]] const Signal<int>& selected()       const { return selected_; }
    [[nodiscard]] const Signal<int>& cursor()         const { return cursor_; }
    [[nodiscard]] FocusNode& focus_node()                    { return focus_; }
    [[nodiscard]] const FocusNode& focus_node()        const { return focus_; }
    [[nodiscard]] int size()                           const { return static_cast<int>(items_.size()); }

    [[nodiscard]] std::string_view selected_label() const {
        int idx = selected_();
        if (idx >= 0 && idx < static_cast<int>(items_.size()))
            return items_[static_cast<size_t>(idx)];
        return {};
    }

    // -- Callback --
    template <std::invocable<int, std::string_view> F>
    void on_change(F&& fn) { on_change_ = std::forward<F>(fn); }

    // -- Mutation --
    void set_items(std::vector<std::string> items) {
        items_ = std::move(items);
        int max_idx = std::max(0, static_cast<int>(items_.size()) - 1);
        if (selected_() > max_idx) selected_.set(max_idx);
        if (cursor_() > max_idx) cursor_.set(max_idx);
    }

    void set_selected(int idx) {
        if (idx >= 0 && idx < static_cast<int>(items_.size())) {
            selected_.set(idx);
            cursor_.set(idx);
        }
    }

    // -- Key event handling --
    [[nodiscard]] bool handle(const KeyEvent& ev) {
        if (!focus_.focused()) return false;
        if (items_.empty()) return false;

        return std::visit(overload{
            [&](SpecialKey sk) -> bool {
                switch (sk) {
                    case SpecialKey::Up:    move_up();   return true;
                    case SpecialKey::Down:  move_down(); return true;
                    case SpecialKey::Enter: select_current(); return true;
                    default: return false;
                }
            },
            [&](CharKey ck) -> bool {
                if (ck.codepoint == 'j') { move_down();       return true; }
                if (ck.codepoint == 'k') { move_up();         return true; }
                if (ck.codepoint == ' ') { select_current();  return true; }
                return false;
            },
        }, ev.key);
    }

    // -- Node concept --
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        int cur = cursor_();
        int sel = selected_();
        bool focused = focus_.focused();

        int total = static_cast<int>(items_.size());
        int start = 0;
        int count = total;

        if (cfg_.visible_count > 0 && cfg_.visible_count < total) {
            count = cfg_.visible_count;
            start = std::max(0, cur - count / 2);
            if (start + count > total) start = total - count;
        }

        std::vector<Element> rows;
        rows.reserve(static_cast<size_t>(count));

        auto cursor_bg = Style{}.with_fg(Color::rgb(200, 204, 212));

        for (int i = start; i < start + count; ++i) {
            bool is_selected = (i == sel);
            bool is_cursor   = (i == cur);

            std::string content;
            std::vector<StyledRun> runs;

            // Radio indicator
            std::string indicator = is_selected
                ? cfg_.selected_indicator
                : cfg_.unselected_indicator;

            Style indicator_style;
            if (is_selected) {
                indicator_style = Style{}.with_fg(Color::rgb(97, 175, 239)).with_bold();
            } else {
                indicator_style = Style{}.with_fg(Color::rgb(92, 99, 112));
            }

            runs.push_back(StyledRun{content.size(), indicator.size(), indicator_style});
            content += indicator;

            // Label
            const auto& label = items_[static_cast<size_t>(i)];
            Style label_style;
            if (is_selected) {
                label_style = cfg_.selected_style;
            } else if (is_cursor && focused) {
                label_style = cursor_bg;
            } else {
                label_style = cfg_.unselected_style;
            }

            runs.push_back(StyledRun{content.size(), label.size(), label_style});
            content += label;

            rows.push_back(Element{TextElement{
                .content = std::move(content),
                .style = {},
                .wrap = TextWrap::NoWrap,
                .runs = std::move(runs),
            }});
        }

        return dsl::v(std::move(rows)).build();
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

    void select_current() {
        int c = cursor_();
        selected_.set(c);
        if (on_change_) on_change_(c, items_[static_cast<size_t>(c)]);
    }
};

} // namespace maya
