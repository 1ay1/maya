#pragma once
// maya::widget::input — Text input widget with cursor, editing, and history
//
// A reactive text input that integrates with the focus system.
// Supports single-line editing, cursor movement, history, and password masking.
//
// Usage:
//   Input<> prompt;
//   prompt.on_submit([](std::string_view text) { process(text); });
//
//   auto ui = h(text("> ") | Dim, prompt);

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "../core/focus.hpp"
#include "../core/overload.hpp"
#include "../core/signal.hpp"
#include "../element/builder.hpp"
#include "../style/style.hpp"
#include "../terminal/input.hpp"

namespace maya {

// ============================================================================
// InputConfig — compile-time configuration via structural NTTP
// ============================================================================

struct InputConfig {
    bool multiline  = false;
    bool history    = true;
    int  max_length = 0;       // 0 = unlimited
    bool password   = false;   // mask characters with bullet
};

// ============================================================================
// CursorPosition — thread-local for hardware cursor rendering
// ============================================================================

namespace detail {
    struct CursorPos {
        int x = -1, y = -1;
    };
    inline thread_local CursorPos active_cursor;
}

// ============================================================================
// Input — reactive text input widget
// ============================================================================

template <InputConfig Cfg = InputConfig{}>
class Input {
    Signal<std::string> value_{std::string{}};
    Signal<int>         cursor_{0};  // byte position in value
    FocusNode           focus_;

    // History
    struct History {
        std::vector<std::string> entries;
        int index = -1;
        std::string saved;
    };
    History history_{};

    // Callbacks
    std::move_only_function<void(std::string_view)> on_submit_;
    std::move_only_function<void(std::string_view)> on_change_;

    // Placeholder text
    std::string placeholder_;

public:
    Input() = default;

    // -- Signal access --
    [[nodiscard]] const Signal<std::string>& value()  const { return value_; }
    [[nodiscard]] const Signal<int>&         cursor() const { return cursor_; }
    [[nodiscard]] const FocusNode& focus_node()       const { return focus_; }
    [[nodiscard]] FocusNode& focus_node()                    { return focus_; }

    // -- Configuration --
    void set_placeholder(std::string_view p) { placeholder_ = std::string{p}; }

    // -- Callback registration --
    template <std::invocable<std::string_view> F>
    void on_submit(F&& fn) { on_submit_ = std::forward<F>(fn); }

    template <std::invocable<std::string_view> F>
    void on_change(F&& fn) { on_change_ = std::forward<F>(fn); }

    // -- Key event handling --
    [[nodiscard]] bool handle(const KeyEvent& ev) {
        if (!focus_.focused()) return false;

        const auto& val = value_();
        int cur = cursor_();

        return std::visit(overload{
            [&](CharKey ck) -> bool {
                if (ev.mods.ctrl) {
                    switch (ck.codepoint) {
                        case 'a': case 'A': move_home(); return true;
                        case 'e': case 'E': move_end();  return true;
                        case 'k': case 'K': kill_to_end(); return true;
                        case 'u': case 'U': kill_to_start(); return true;
                        case 'w': case 'W': delete_word_backward(); return true;
                        default: return false;
                    }
                }
                if (ev.mods.alt) return false;
                insert_char(ck.codepoint);
                return true;
            },
            [&](SpecialKey sk) -> bool {
                switch (sk) {
                    case SpecialKey::Left:      move_left();  return true;
                    case SpecialKey::Right:     move_right(); return true;
                    case SpecialKey::Home:      move_home();  return true;
                    case SpecialKey::End:       move_end();   return true;
                    case SpecialKey::Backspace: delete_backward(); return true;
                    case SpecialKey::Delete:    delete_forward();  return true;
                    case SpecialKey::Enter:     submit(); return true;
                    case SpecialKey::Up:
                        if constexpr (Cfg.history) { history_up(); return true; }
                        return false;
                    case SpecialKey::Down:
                        if constexpr (Cfg.history) { history_down(); return true; }
                        return false;
                    default: return false;
                }
            },
        }, ev.key);
    }

    // -- Node concept: build into Element --
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        const auto& val = value_();
        int cur = cursor_();

        std::string display;
        if constexpr (Cfg.password) {
            // Replace each character with bullet
            for (size_t i = 0; i < val.size(); ) {
                uint8_t ch = static_cast<uint8_t>(val[i]);
                if (ch < 0x80) { ++i; }
                else if ((ch & 0xE0) == 0xC0) { i += 2; }
                else if ((ch & 0xF0) == 0xE0) { i += 3; }
                else { i += 4; }
                display += "\xe2\x80\xa2"; // U+2022 bullet
            }
        } else {
            display = val;
        }

        bool is_focused = focus_.focused();
        bool is_empty = val.empty();

        if (is_empty && !placeholder_.empty() && !is_focused) {
            return Element{TextElement{
                .content = placeholder_,
                .style = Style{}.with_dim(),
            }};
        }

        if (is_focused) {
            // Render with cursor indicator
            // Split text at cursor position, insert a visible cursor
            std::string before = display.substr(0, static_cast<size_t>(cur));
            std::string after = cur < static_cast<int>(display.size())
                ? display.substr(static_cast<size_t>(cur))
                : "";

            // Use inverse style for cursor character
            std::string cursor_ch = after.empty() ? " " : after.substr(0, 1);
            if (!after.empty()) after = after.substr(1);

            std::vector<Element> parts;
            if (!before.empty())
                parts.push_back(Element{TextElement{.content = std::move(before)}});
            parts.push_back(Element{TextElement{
                .content = std::move(cursor_ch),
                .style = Style{}.with_inverse(),
            }});
            if (!after.empty())
                parts.push_back(Element{TextElement{.content = std::move(after)}});

            return detail::hstack()(std::move(parts));
        }

        return Element{TextElement{.content = std::move(display)}};
    }

    // -- Programmatic access --
    void set_value(std::string_view v) {
        value_.set(std::string{v});
        cursor_.set(std::min(cursor_(), static_cast<int>(v.size())));
        fire_change();
    }

    void clear() {
        value_.set(std::string{});
        cursor_.set(0);
        fire_change();
    }

private:
    void fire_change() {
        if (on_change_) on_change_(value_());
    }

    void insert_char(char32_t cp) {
        auto& val = const_cast<std::string&>(value_());
        int cur = cursor_();

        // Encode UTF-8
        char buf[4];
        int len = 0;
        if (cp < 0x80) {
            buf[0] = static_cast<char>(cp); len = 1;
        } else if (cp < 0x800) {
            buf[0] = static_cast<char>(0xC0 | (cp >> 6));
            buf[1] = static_cast<char>(0x80 | (cp & 0x3F));
            len = 2;
        } else if (cp < 0x10000) {
            buf[0] = static_cast<char>(0xE0 | (cp >> 12));
            buf[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            buf[2] = static_cast<char>(0x80 | (cp & 0x3F));
            len = 3;
        } else {
            buf[0] = static_cast<char>(0xF0 | (cp >> 18));
            buf[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            buf[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            buf[3] = static_cast<char>(0x80 | (cp & 0x3F));
            len = 4;
        }

        if (Cfg.max_length > 0 && static_cast<int>(val.size()) + len > Cfg.max_length)
            return;

        value_.update([&](std::string& v) {
            v.insert(static_cast<size_t>(cur), buf, static_cast<size_t>(len));
        });
        cursor_.set(cur + len);
        fire_change();
    }

    void delete_backward() {
        int cur = cursor_();
        if (cur <= 0) return;
        // Find start of previous UTF-8 character
        const auto& val = value_();
        int prev = cur - 1;
        while (prev > 0 && (static_cast<uint8_t>(val[static_cast<size_t>(prev)]) & 0xC0) == 0x80)
            --prev;
        value_.update([&](std::string& v) {
            v.erase(static_cast<size_t>(prev), static_cast<size_t>(cur - prev));
        });
        cursor_.set(prev);
        fire_change();
    }

    void delete_forward() {
        int cur = cursor_();
        const auto& val = value_();
        if (cur >= static_cast<int>(val.size())) return;
        // Find end of current UTF-8 character
        int next = cur + 1;
        while (next < static_cast<int>(val.size()) &&
               (static_cast<uint8_t>(val[static_cast<size_t>(next)]) & 0xC0) == 0x80)
            ++next;
        value_.update([&](std::string& v) {
            v.erase(static_cast<size_t>(cur), static_cast<size_t>(next - cur));
        });
        fire_change();
    }

    void delete_word_backward() {
        int cur = cursor_();
        if (cur <= 0) return;
        const auto& val = value_();
        int pos = cur - 1;
        // Skip spaces
        while (pos > 0 && val[static_cast<size_t>(pos)] == ' ') --pos;
        // Skip word
        while (pos > 0 && val[static_cast<size_t>(pos - 1)] != ' ') --pos;
        value_.update([&](std::string& v) {
            v.erase(static_cast<size_t>(pos), static_cast<size_t>(cur - pos));
        });
        cursor_.set(pos);
        fire_change();
    }

    void kill_to_end() {
        int cur = cursor_();
        value_.update([&](std::string& v) {
            v.erase(static_cast<size_t>(cur));
        });
        fire_change();
    }

    void kill_to_start() {
        int cur = cursor_();
        value_.update([&](std::string& v) {
            v.erase(0, static_cast<size_t>(cur));
        });
        cursor_.set(0);
        fire_change();
    }

    void move_left() {
        int cur = cursor_();
        if (cur <= 0) return;
        const auto& val = value_();
        int prev = cur - 1;
        while (prev > 0 && (static_cast<uint8_t>(val[static_cast<size_t>(prev)]) & 0xC0) == 0x80)
            --prev;
        cursor_.set(prev);
    }

    void move_right() {
        int cur = cursor_();
        const auto& val = value_();
        if (cur >= static_cast<int>(val.size())) return;
        int next = cur + 1;
        while (next < static_cast<int>(val.size()) &&
               (static_cast<uint8_t>(val[static_cast<size_t>(next)]) & 0xC0) == 0x80)
            ++next;
        cursor_.set(next);
    }

    void move_home() { cursor_.set(0); }
    void move_end()  { cursor_.set(static_cast<int>(value_().size())); }

    void submit() {
        if (on_submit_) on_submit_(value_());
        if constexpr (Cfg.history) {
            const auto& val = value_();
            if (!val.empty()) {
                history_.entries.push_back(val);
            }
            history_.index = -1;
            history_.saved.clear();
        }
    }

    void history_up() {
        if (history_.entries.empty()) return;
        if (history_.index < 0) {
            history_.saved = value_();
            history_.index = static_cast<int>(history_.entries.size()) - 1;
        } else if (history_.index > 0) {
            --history_.index;
        } else {
            return;
        }
        auto& entry = history_.entries[static_cast<size_t>(history_.index)];
        value_.set(entry);
        cursor_.set(static_cast<int>(entry.size()));
    }

    void history_down() {
        if (history_.index < 0) return;
        ++history_.index;
        if (history_.index >= static_cast<int>(history_.entries.size())) {
            history_.index = -1;
            value_.set(history_.saved);
            cursor_.set(static_cast<int>(history_.saved.size()));
        } else {
            auto& entry = history_.entries[static_cast<size_t>(history_.index)];
            value_.set(entry);
            cursor_.set(static_cast<int>(entry.size()));
        }
    }
};

} // namespace maya
