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
#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/border.hpp"
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

    // -- Paste event handling --
    /// Handle a paste event — insert pasted text at cursor position.
    void handle_paste(const PasteEvent& pe) {
        // Insert each character from the pasted text
        for (std::size_t pos = 0; pos < pe.content.size(); ) {
            char32_t cp = decode_utf8(pe.content, pos);
            if (cp >= 0x20) insert_char(cp);  // skip control chars
        }
    }

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
                    case SpecialKey::Enter:
                        if constexpr (Cfg.multiline) {
                            if (ev.mods.ctrl || ev.mods.shift) {
                                submit(); return true;
                            }
                            insert_char('\n'); return true;
                        } else {
                            submit(); return true;
                        }
                    case SpecialKey::Up:
                        if constexpr (Cfg.multiline) {
                            if (move_up()) return true;
                        }
                        if constexpr (Cfg.history) { history_up(); return true; }
                        return false;
                    case SpecialKey::Down:
                        if constexpr (Cfg.multiline) {
                            if (move_down()) return true;
                        }
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

        // Build inner content (text + cursor)
        Element inner{TextElement{}};
        if (is_empty && !placeholder_.empty() && !is_focused) {
            inner = Element{TextElement{
                .content = placeholder_,
                .style = Style{}.with_fg(Color::rgb(92, 99, 112)),
            }};
        } else if constexpr (Cfg.multiline) {
            inner = build_multiline(display, cur, is_focused);
        } else {
            inner = build_singleline(display, cur, is_focused);
        }

        // Zed style: wrap in a bordered box with prompt indicator
        auto border_color = is_focused
            ? Color::rgb(97, 175, 239)   // blue when focused
            : Color::rgb(50, 54, 62);    // muted when blurred

        return (dsl::v(
                dsl::h(
                    Element{TextElement{
                        .content = "\xe2\x9d\xaf ",  // "❯ "
                        .style = Style{}.with_fg(
                            is_focused ? Color::rgb(97, 175, 239)
                                       : Color::rgb(92, 99, 112)),
                    }},
                    std::move(inner)
                ).build()
            ) | dsl::border(BorderStyle::Round) | dsl::bcolor(border_color)
              | dsl::padding(0, 1, 0, 1)).build();
    }

private:
    [[nodiscard]] Element build_singleline(const std::string& display,
                                            int cur, bool focused) const {
        if (focused) {
            // Build as single TextElement with StyledRuns for cursor
            std::string content;
            std::vector<StyledRun> runs;
            auto text_style = Style{}.with_fg(Color::rgb(200, 204, 212));
            auto cursor_style = Style{}.with_inverse();

            // Before cursor
            if (cur > 0) {
                std::string before = display.substr(0, static_cast<size_t>(cur));
                runs.push_back(StyledRun{content.size(), before.size(), text_style});
                content += before;
            }

            // Cursor character
            std::string cursor_ch;
            size_t after_start = static_cast<size_t>(cur);
            if (after_start < display.size()) {
                // Get one UTF-8 character at cursor
                size_t ch_len = 1;
                uint8_t lead = static_cast<uint8_t>(display[after_start]);
                if (lead >= 0xF0) ch_len = 4;
                else if (lead >= 0xE0) ch_len = 3;
                else if (lead >= 0xC0) ch_len = 2;
                ch_len = std::min(ch_len, display.size() - after_start);
                cursor_ch = display.substr(after_start, ch_len);
                after_start += ch_len;
            } else {
                cursor_ch = " ";
            }
            runs.push_back(StyledRun{content.size(), cursor_ch.size(), cursor_style});
            content += cursor_ch;

            // After cursor
            if (after_start < display.size()) {
                std::string after = display.substr(after_start);
                runs.push_back(StyledRun{content.size(), after.size(), text_style});
                content += after;
            }

            return Element{TextElement{
                .content = std::move(content),
                .style = text_style,
                .runs = std::move(runs),
            }};
        }
        return Element{TextElement{
            .content = display,
            .style = Style{}.with_fg(Color::rgb(200, 204, 212)),
        }};
    }

    [[nodiscard]] Element build_multiline(const std::string& display,
                                           int cur, bool focused) const {
        // Split into lines
        std::vector<std::string> lines;
        size_t pos = 0;
        while (pos <= display.size()) {
            size_t nl = display.find('\n', pos);
            if (nl == std::string::npos) {
                lines.push_back(display.substr(pos));
                break;
            }
            lines.push_back(display.substr(pos, nl - pos));
            pos = nl + 1;
            if (pos == display.size()) lines.emplace_back(); // trailing newline
        }

        auto text_style = Style{}.with_fg(Color::rgb(200, 204, 212));

        if (!focused) {
            std::vector<Element> rows;
            for (auto& line : lines)
                rows.push_back(Element{TextElement{.content = std::move(line), .style = text_style}});
            return dsl::v(std::move(rows)).build();
        }

        // Find which line the cursor is on
        int cursor_line = 0;
        int cursor_col = cur;
        {
            int accum = 0;
            for (size_t i = 0; i < lines.size(); ++i) {
                int line_len = static_cast<int>(lines[i].size());
                if (cur <= accum + line_len) {
                    cursor_line = static_cast<int>(i);
                    cursor_col = cur - accum;
                    break;
                }
                accum += line_len + 1; // +1 for \n
            }
        }

        // Render each line, with cursor on the right one
        std::vector<Element> rows;
        for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
            if (i == cursor_line) {
                rows.push_back(build_singleline(lines[static_cast<size_t>(i)],
                                                 cursor_col, true));
            } else {
                rows.push_back(Element{TextElement{
                    .content = lines[static_cast<size_t>(i)],
                    .style = text_style}});
            }
        }
        return dsl::v(std::move(rows)).build();
    }

public:

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

    // Multiline cursor movement — returns true if cursor moved
    bool move_up() {
        const auto& val = value_();
        int cur = cursor_();
        // Find start of current line
        int line_start = cur;
        while (line_start > 0 && val[static_cast<size_t>(line_start - 1)] != '\n')
            --line_start;
        if (line_start == 0) return false; // already on first line
        int col = cur - line_start;
        // Find start of previous line
        int prev_end = line_start - 1; // the \n
        int prev_start = prev_end;
        while (prev_start > 0 && val[static_cast<size_t>(prev_start - 1)] != '\n')
            --prev_start;
        int prev_len = prev_end - prev_start;
        cursor_.set(prev_start + std::min(col, prev_len));
        return true;
    }

    bool move_down() {
        const auto& val = value_();
        int cur = cursor_();
        int len = static_cast<int>(val.size());
        // Find start of current line
        int line_start = cur;
        while (line_start > 0 && val[static_cast<size_t>(line_start - 1)] != '\n')
            --line_start;
        int col = cur - line_start;
        // Find end of current line
        int line_end = cur;
        while (line_end < len && val[static_cast<size_t>(line_end)] != '\n')
            ++line_end;
        if (line_end >= len) return false; // already on last line
        // Next line starts after \n
        int next_start = line_end + 1;
        int next_end = next_start;
        while (next_end < len && val[static_cast<size_t>(next_end)] != '\n')
            ++next_end;
        int next_len = next_end - next_start;
        cursor_.set(next_start + std::min(col, next_len));
        return true;
    }

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
