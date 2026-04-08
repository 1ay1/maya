#pragma once
// maya::widget::textarea — Multi-line text editor with line numbers and scrolling
//
// A reactive multi-line text input that integrates with the focus system.
// Supports cursor movement, line-aware editing, and vertical scrolling.
//
//   ╭──────────────────────────────────╮
//   │  1 │ fn main() {                 │
//   │  2 │     println!("hello");      │
//   │  3 │     let x = 42;█            │
//   │  4 │ }                           │
//   │                          4 lines │
//   ╰──────────────────────────────────╯
//
// Usage:
//   TextArea editor;
//   editor.on_change([](std::string_view text) { save(text); });
//   auto ui = editor.build();

#include <algorithm>
#include <cstdint>
#include <cstdio>
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

struct TextAreaConfig {
    bool line_numbers    = true;
    bool show_line_count = true;
    int  visible_lines   = 0;  // 0 = show all lines
    int  max_length      = 0;  // 0 = unlimited
};

class TextArea {
    Signal<std::string> value_{std::string{}};
    Signal<int>         cursor_{0};  // byte position in value
    FocusNode           focus_;
    int                 scroll_offset_ = 0;
    TextAreaConfig      cfg_;

    std::move_only_function<void(std::string_view)> on_change_;
    std::move_only_function<void(std::string_view)> on_submit_;

    std::string placeholder_;

public:
    TextArea() = default;
    explicit TextArea(TextAreaConfig cfg) : cfg_(cfg) {}

    // -- Signal access --
    [[nodiscard]] const Signal<std::string>& value()  const { return value_; }
    [[nodiscard]] const Signal<int>&         cursor() const { return cursor_; }
    [[nodiscard]] const FocusNode& focus_node()       const { return focus_; }
    [[nodiscard]] FocusNode& focus_node()                    { return focus_; }

    void set_placeholder(std::string_view p) { placeholder_ = std::string{p}; }

    template <std::invocable<std::string_view> F>
    void on_change(F&& fn) { on_change_ = std::forward<F>(fn); }

    template <std::invocable<std::string_view> F>
    void on_submit(F&& fn) { on_submit_ = std::forward<F>(fn); }

    // -- Paste event handling --
    void handle_paste(const PasteEvent& pe) {
        for (std::size_t pos = 0; pos < pe.content.size(); ) {
            char32_t cp = decode_utf8(pe.content, pos);
            if (cp >= 0x20 || cp == '\n') insert_char(cp);
        }
    }

    // -- Key event handling --
    [[nodiscard]] bool handle(const KeyEvent& ev) {
        if (!focus_.focused()) return false;

        return std::visit(overload{
            [&](CharKey ck) -> bool {
                if (ev.mods.ctrl) {
                    switch (ck.codepoint) {
                        case 'a': case 'A': move_home_line(); return true;
                        case 'e': case 'E': move_end_line();  return true;
                        case 'k': case 'K': kill_to_end_of_line(); return true;
                        case 'u': case 'U': kill_to_start_of_line(); return true;
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
                    case SpecialKey::Up:        move_up();    return true;
                    case SpecialKey::Down:      move_down();  return true;
                    case SpecialKey::Home:      move_home_line(); return true;
                    case SpecialKey::End:       move_end_line();  return true;
                    case SpecialKey::Backspace: delete_backward(); return true;
                    case SpecialKey::Delete:    delete_forward();  return true;
                    case SpecialKey::Enter:
                        if (ev.mods.ctrl || ev.mods.shift) {
                            if (on_submit_) on_submit_(value_());
                            return true;
                        }
                        insert_char('\n');
                        return true;
                    default: return false;
                }
            },
        }, ev.key);
    }

    // -- Node concept --
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        const auto& val = value_();
        int cur = cursor_();
        bool focused = focus_.focused();

        // Split into lines
        auto lines = split_lines(val);
        int total_lines = static_cast<int>(lines.size());

        // Find cursor line and column
        int cursor_line = 0, cursor_col = 0;
        find_cursor_pos(val, cur, cursor_line, cursor_col);

        // Compute visible window
        int vis = cfg_.visible_lines > 0 ? cfg_.visible_lines : total_lines;
        int scroll = const_cast<TextArea*>(this)->compute_scroll(cursor_line, total_lines, vis);

        int start = scroll;
        int end = std::min(scroll + vis, total_lines);

        // Number gutter width
        int gutter_w = 0;
        if (cfg_.line_numbers) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d", total_lines);
            gutter_w = static_cast<int>(std::strlen(buf));
            if (gutter_w < 2) gutter_w = 2;
        }

        auto text_style    = Style{}.with_fg(Color::rgb(200, 204, 212));
        auto dim_style     = Style{}.with_fg(Color::rgb(92, 99, 112));
        auto cursor_style  = Style{}.with_inverse();
        auto gutter_sep    = Style{}.with_fg(Color::rgb(50, 54, 62));

        // Empty state
        if (val.empty() && !placeholder_.empty() && !focused) {
            auto inner = Element{TextElement{
                .content = placeholder_,
                .style = dim_style,
            }};
            auto border_color = Color::rgb(50, 54, 62);
            return (dsl::v(std::move(inner))
                | dsl::border(BorderStyle::Round) | dsl::bcolor(border_color)
                | dsl::padding(0, 1, 0, 1)).build();
        }

        // Build rows
        std::vector<Element> rows;
        rows.reserve(static_cast<size_t>(end - start));

        for (int i = start; i < end; ++i) {
            const auto& line = lines[static_cast<size_t>(i)];
            bool is_cursor_line = (i == cursor_line && focused);

            std::string content;
            std::vector<StyledRun> runs;

            // Line number gutter
            if (cfg_.line_numbers) {
                char num_buf[16];
                std::snprintf(num_buf, sizeof(num_buf), "%*d", gutter_w, i + 1);
                std::string num_str = num_buf;
                auto num_style = is_cursor_line
                    ? Style{}.with_fg(Color::rgb(171, 178, 191))
                    : dim_style;
                runs.push_back(StyledRun{content.size(), num_str.size(), num_style});
                content += num_str;

                std::string sep = " \xe2\x94\x82 ";  // " | "
                runs.push_back(StyledRun{content.size(), sep.size(), gutter_sep});
                content += sep;
            }

            // Line content with cursor
            if (is_cursor_line) {
                // Before cursor
                if (cursor_col > 0) {
                    std::string before = line.substr(0, static_cast<size_t>(cursor_col));
                    runs.push_back(StyledRun{content.size(), before.size(), text_style});
                    content += before;
                }
                // Cursor character
                std::string cursor_ch;
                size_t after_start = static_cast<size_t>(cursor_col);
                if (after_start < line.size()) {
                    size_t ch_len = utf8_char_len(line, after_start);
                    cursor_ch = line.substr(after_start, ch_len);
                    after_start += ch_len;
                } else {
                    cursor_ch = " ";
                }
                runs.push_back(StyledRun{content.size(), cursor_ch.size(), cursor_style});
                content += cursor_ch;
                // After cursor
                if (after_start < line.size()) {
                    std::string after = line.substr(after_start);
                    runs.push_back(StyledRun{content.size(), after.size(), text_style});
                    content += after;
                }
            } else {
                if (!line.empty()) {
                    runs.push_back(StyledRun{content.size(), line.size(), text_style});
                    content += line;
                }
            }

            // Ensure there is at least some content for the row
            if (content.empty()) {
                content = " ";
                runs.push_back(StyledRun{0, 1, text_style});
            }

            rows.push_back(Element{TextElement{
                .content = std::move(content),
                .style = text_style,
                .runs = std::move(runs),
            }});
        }

        // Line count footer
        if (cfg_.show_line_count) {
            char count_buf[32];
            std::snprintf(count_buf, sizeof(count_buf), "%d line%s",
                total_lines, total_lines == 1 ? "" : "s");
            rows.push_back(Element{TextElement{
                .content = std::string(count_buf),
                .style = dim_style,
            }});
        }

        auto inner = dsl::v(std::move(rows)).build();
        auto border_color = focused
            ? Color::rgb(97, 175, 239)
            : Color::rgb(50, 54, 62);

        return (dsl::v(std::move(inner))
            | dsl::border(BorderStyle::Round) | dsl::bcolor(border_color)
            | dsl::padding(0, 1, 0, 1)).build();
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
        scroll_offset_ = 0;
        fire_change();
    }

private:
    void fire_change() {
        if (on_change_) on_change_(value_());
    }

    static std::vector<std::string> split_lines(const std::string& s) {
        std::vector<std::string> lines;
        size_t pos = 0;
        while (pos <= s.size()) {
            size_t nl = s.find('\n', pos);
            if (nl == std::string::npos) {
                lines.push_back(s.substr(pos));
                break;
            }
            lines.push_back(s.substr(pos, nl - pos));
            pos = nl + 1;
            if (pos == s.size()) lines.emplace_back();
        }
        if (lines.empty()) lines.emplace_back();
        return lines;
    }

    static void find_cursor_pos(const std::string& val, int cur,
                                 int& out_line, int& out_col) {
        out_line = 0;
        int accum = 0;
        auto lines = split_lines(val);
        for (size_t i = 0; i < lines.size(); ++i) {
            int line_len = static_cast<int>(lines[i].size());
            if (cur <= accum + line_len) {
                out_line = static_cast<int>(i);
                out_col = cur - accum;
                return;
            }
            accum += line_len + 1;
        }
        out_line = std::max(0, static_cast<int>(lines.size()) - 1);
        out_col = static_cast<int>(lines.back().size());
    }

    int compute_scroll(int cursor_line, int total_lines, int visible) {
        if (visible >= total_lines) { scroll_offset_ = 0; return 0; }
        if (cursor_line < scroll_offset_) scroll_offset_ = cursor_line;
        if (cursor_line >= scroll_offset_ + visible)
            scroll_offset_ = cursor_line - visible + 1;
        scroll_offset_ = std::clamp(scroll_offset_, 0, total_lines - visible);
        return scroll_offset_;
    }

    static size_t utf8_char_len(const std::string& s, size_t pos) {
        if (pos >= s.size()) return 0;
        uint8_t lead = static_cast<uint8_t>(s[pos]);
        size_t len = 1;
        if (lead >= 0xF0) len = 4;
        else if (lead >= 0xE0) len = 3;
        else if (lead >= 0xC0) len = 2;
        return std::min(len, s.size() - pos);
    }

    void insert_char(char32_t cp) {
        int cur = cursor_();

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

        if (cfg_.max_length > 0 &&
            static_cast<int>(value_().size()) + len > cfg_.max_length)
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
        const auto& val = value_();
        int prev = cur - 1;
        while (prev > 0 &&
               (static_cast<uint8_t>(val[static_cast<size_t>(prev)]) & 0xC0) == 0x80)
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
        int next = cur + 1;
        while (next < static_cast<int>(val.size()) &&
               (static_cast<uint8_t>(val[static_cast<size_t>(next)]) & 0xC0) == 0x80)
            ++next;
        value_.update([&](std::string& v) {
            v.erase(static_cast<size_t>(cur), static_cast<size_t>(next - cur));
        });
        fire_change();
    }

    void move_left() {
        int cur = cursor_();
        if (cur <= 0) return;
        const auto& val = value_();
        int prev = cur - 1;
        while (prev > 0 &&
               (static_cast<uint8_t>(val[static_cast<size_t>(prev)]) & 0xC0) == 0x80)
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

    void move_up() {
        const auto& val = value_();
        int cur = cursor_();
        int line_start = cur;
        while (line_start > 0 && val[static_cast<size_t>(line_start - 1)] != '\n')
            --line_start;
        if (line_start == 0) return;
        int col = cur - line_start;
        int prev_end = line_start - 1;
        int prev_start = prev_end;
        while (prev_start > 0 && val[static_cast<size_t>(prev_start - 1)] != '\n')
            --prev_start;
        int prev_len = prev_end - prev_start;
        cursor_.set(prev_start + std::min(col, prev_len));
    }

    void move_down() {
        const auto& val = value_();
        int cur = cursor_();
        int len = static_cast<int>(val.size());
        int line_start = cur;
        while (line_start > 0 && val[static_cast<size_t>(line_start - 1)] != '\n')
            --line_start;
        int col = cur - line_start;
        int line_end = cur;
        while (line_end < len && val[static_cast<size_t>(line_end)] != '\n')
            ++line_end;
        if (line_end >= len) return;
        int next_start = line_end + 1;
        int next_end = next_start;
        while (next_end < len && val[static_cast<size_t>(next_end)] != '\n')
            ++next_end;
        int next_len = next_end - next_start;
        cursor_.set(next_start + std::min(col, next_len));
    }

    void move_home_line() {
        const auto& val = value_();
        int cur = cursor_();
        while (cur > 0 && val[static_cast<size_t>(cur - 1)] != '\n') --cur;
        cursor_.set(cur);
    }

    void move_end_line() {
        const auto& val = value_();
        int cur = cursor_();
        int len = static_cast<int>(val.size());
        while (cur < len && val[static_cast<size_t>(cur)] != '\n') ++cur;
        cursor_.set(cur);
    }

    void kill_to_end_of_line() {
        const auto& val = value_();
        int cur = cursor_();
        int end = cur;
        int len = static_cast<int>(val.size());
        while (end < len && val[static_cast<size_t>(end)] != '\n') ++end;
        if (end == cur && end < len) ++end;  // delete the newline itself
        value_.update([&](std::string& v) {
            v.erase(static_cast<size_t>(cur), static_cast<size_t>(end - cur));
        });
        fire_change();
    }

    void kill_to_start_of_line() {
        const auto& val = value_();
        int cur = cursor_();
        int start = cur;
        while (start > 0 && val[static_cast<size_t>(start - 1)] != '\n') --start;
        value_.update([&](std::string& v) {
            v.erase(static_cast<size_t>(start), static_cast<size_t>(cur - start));
        });
        cursor_.set(start);
        fire_change();
    }
};

} // namespace maya
