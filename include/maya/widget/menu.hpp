#pragma once
// maya::widget::menu — Vertical menu with keybind hints
//
// Arrow-key navigable menu with shortcut display, separators, and disabled items.
//
// Usage:
//   Menu menu({
//       {.label = "New File",   .shortcut = "Ctrl+N"},
//       {.label = "Open File",  .shortcut = "Ctrl+O"},
//       {.separator = true},
//       {.label = "Quit",       .shortcut = "Ctrl+Q"},
//   });
//   menu.on_select([](int idx) { /* handle */ });

#include <algorithm>
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

struct MenuItem {
    std::string label;
    std::string shortcut;
    bool enabled   = true;
    bool separator = false;
};

class Menu {
    std::vector<MenuItem> items_;
    Signal<int> cursor_{0};
    FocusNode focus_;

    std::move_only_function<void(int)> on_select_;

public:
    explicit Menu(std::vector<MenuItem> items)
        : items_(std::move(items))
    {
        // Ensure cursor starts on a non-separator, enabled item
        skip_to_valid(1);
    }

    // -- Accessors --
    [[nodiscard]] const Signal<int>& cursor()    const { return cursor_; }
    [[nodiscard]] FocusNode& focus_node()               { return focus_; }
    [[nodiscard]] const FocusNode& focus_node()  const { return focus_; }
    [[nodiscard]] int size()                     const { return static_cast<int>(items_.size()); }

    // -- Callback --
    template <std::invocable<int> F>
    void on_select(F&& fn) { on_select_ = std::forward<F>(fn); }

    // -- Mutation --
    void set_items(std::vector<MenuItem> items) {
        items_ = std::move(items);
        cursor_.set(0);
        skip_to_valid(1);
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
                        if (is_selectable(cursor_())) {
                            if (on_select_) on_select_(cursor_());
                        }
                        return true;
                    default: return false;
                }
            },
            [&](CharKey ck) -> bool {
                if (ck.codepoint == 'k') { move_up();   return true; }
                if (ck.codepoint == 'j') { move_down(); return true; }
                return false;
            },
        }, ev.key);
    }

    // -- Node concept: build into Element --
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        int cur = cursor_();
        bool focused = focus_.focused();

        auto normal_style   = Style{}.with_fg(Color::rgb(200, 204, 212));
        auto shortcut_style = Style{}.with_fg(Color::rgb(150, 156, 170));
        auto active_style   = Style{}.with_fg(Color::rgb(97, 175, 239)).with_bold();
        auto active_sc      = Style{}.with_fg(Color::rgb(97, 175, 239));
        auto disabled_style = Style{}.with_fg(Color::rgb(92, 99, 112));
        auto sep_style      = Style{}.with_fg(Color::rgb(50, 54, 62));

        std::vector<Element> rows;
        rows.reserve(items_.size());

        for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
            const auto& item = items_[static_cast<size_t>(i)];

            if (item.separator) {
                // Thin separator line using ComponentElement for full width
                rows.push_back(Element{ComponentElement{
                    .render = [sep_style](int w, int /*h*/) -> Element {
                        std::string line;
                        for (int j = 0; j < w; ++j) line += "\xe2\x94\x88"; // "┈"
                        return Element{TextElement{
                            .content = std::move(line),
                            .style = sep_style,
                        }};
                    },
                    .layout = {},
                }});
                continue;
            }

            bool is_active = (i == cur) && focused;
            bool is_disabled = !item.enabled;

            // Build row: "  label    shortcut"
            std::string content = "  " + item.label;
            std::vector<StyledRun> runs;

            if (is_disabled) {
                // Entire row dimmed
                if (!item.shortcut.empty()) {
                    content += "    " + item.shortcut;
                }
                runs.push_back(StyledRun{0, content.size(), disabled_style});
            } else if (is_active) {
                // Cursor indicator + highlight
                content[0] = '\xe2'; content[1] = '\x80'; // overwrite "  " with marker
                // Actually, let's rebuild with proper indicator
                content = "\xe2\x9d\xaf " + item.label; // "❯ "
                runs.push_back(StyledRun{0, content.size(), active_style});
                if (!item.shortcut.empty()) {
                    size_t sc_start = content.size();
                    content += "    " + item.shortcut;
                    // Re-run: label part active, shortcut part lighter
                    runs.clear();
                    runs.push_back(StyledRun{0, sc_start, active_style});
                    runs.push_back(StyledRun{sc_start, content.size() - sc_start, active_sc});
                }
            } else {
                // Normal item
                runs.push_back(StyledRun{0, content.size(), normal_style});
                if (!item.shortcut.empty()) {
                    size_t sc_start = content.size();
                    content += "    " + item.shortcut;
                    runs.push_back(StyledRun{sc_start, content.size() - sc_start, shortcut_style});
                }
            }

            rows.push_back(Element{TextElement{
                .content = std::move(content),
                .style = normal_style,
                .runs = std::move(runs),
            }});
        }

        return dsl::v(std::move(rows)).build();
    }

private:
    [[nodiscard]] bool is_selectable(int idx) const {
        if (idx < 0 || idx >= static_cast<int>(items_.size())) return false;
        const auto& item = items_[static_cast<size_t>(idx)];
        return !item.separator && item.enabled;
    }

    void skip_to_valid(int dir) {
        int n = static_cast<int>(items_.size());
        if (n == 0) return;
        int c = cursor_();
        for (int tries = 0; tries < n; ++tries) {
            if (is_selectable(c)) { cursor_.set(c); return; }
            c = (c + dir + n) % n;
        }
        cursor_.set(0);
    }

    void move_up() {
        int n = static_cast<int>(items_.size());
        int c = cursor_();
        for (int tries = 0; tries < n; ++tries) {
            c = (c - 1 + n) % n;
            if (is_selectable(c)) { cursor_.set(c); return; }
        }
    }

    void move_down() {
        int n = static_cast<int>(items_.size());
        int c = cursor_();
        for (int tries = 0; tries < n; ++tries) {
            c = (c + 1) % n;
            if (is_selectable(c)) { cursor_.set(c); return; }
        }
    }
};

} // namespace maya
