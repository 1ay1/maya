#include "maya/components/text_input.hpp"

namespace maya::components {

int TextInput::line_count() const {
    int n = 1;
    for (char c : buf_) if (c == '\n') ++n;
    return n;
}

int TextInput::line_start(int pos) const {
    int i = pos;
    while (i > 0 && buf_[i - 1] != '\n') --i;
    return i;
}

int TextInput::line_end(int pos) const {
    int i = pos;
    while (i < static_cast<int>(buf_.size()) && buf_[i] != '\n') ++i;
    return i;
}

int TextInput::current_line() const {
    int line = 0;
    for (int i = 0; i < cursor_ && i < static_cast<int>(buf_.size()); ++i)
        if (buf_[i] == '\n') ++line;
    return line;
}

int TextInput::col_in_line() const {
    return cursor_ - line_start(cursor_);
}

int TextInput::pos_at_line_col(int target_line, int col) const {
    int line = 0, i = 0;
    while (line < target_line && i < static_cast<int>(buf_.size())) {
        if (buf_[i] == '\n') ++line;
        ++i;
    }
    int start = i;
    while (i < static_cast<int>(buf_.size()) && buf_[i] != '\n' && (i - start) < col) ++i;
    return i;
}

void TextInput::ensure_cursor_visible() {
    int line = current_line();
    int vis = std::max(1, props_.max_height > 0 ? props_.max_height - 2 : 100);
    if (line < scroll_) scroll_ = line;
    if (line >= scroll_ + vis) scroll_ = line - vis + 1;
}

bool TextInput::is_word_char(char c) const {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

int TextInput::word_left(int pos) const {
    if (pos <= 0) return 0;
    int i = pos - 1;
    while (i > 0 && !is_word_char(buf_[i])) --i;
    while (i > 0 && is_word_char(buf_[i - 1])) --i;
    return i;
}

int TextInput::word_right(int pos) const {
    int n = static_cast<int>(buf_.size());
    if (pos >= n) return n;
    int i = pos;
    while (i < n && !is_word_char(buf_[i])) ++i;
    while (i < n && is_word_char(buf_[i])) ++i;
    return i;
}

bool TextInput::update(const Event& ev) {
    if (!focused_ || props_.readonly) return false;

    auto* k = as_key(ev);
    if (!k) {
        // Handle paste
        std::string paste_content;
        if (pasted(ev, &paste_content)) {
            buf_.insert(cursor_, paste_content);
            cursor_ += static_cast<int>(paste_content.size());
            ensure_cursor_visible();
            return true;
        }
        return false;
    }

    bool handled = true;
    int n = static_cast<int>(buf_.size());

    if (auto* ch = std::get_if<CharKey>(&k->key)) {
        // Regular character input
        char c = static_cast<char>(ch->codepoint);
        if (ch->codepoint < 128 && c >= 32) {
            buf_.insert(buf_.begin() + cursor_, c);
            ++cursor_;
        } else if (ch->codepoint >= 128) {
            // UTF-8 encode
            char u8[4];
            int len = 0;
            char32_t cp = ch->codepoint;
            if (cp < 0x80) { u8[0] = static_cast<char>(cp); len = 1; }
            else if (cp < 0x800) {
                u8[0] = static_cast<char>(0xC0 | (cp >> 6));
                u8[1] = static_cast<char>(0x80 | (cp & 0x3F));
                len = 2;
            } else if (cp < 0x10000) {
                u8[0] = static_cast<char>(0xE0 | (cp >> 12));
                u8[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                u8[2] = static_cast<char>(0x80 | (cp & 0x3F));
                len = 3;
            } else {
                u8[0] = static_cast<char>(0xF0 | (cp >> 18));
                u8[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                u8[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                u8[3] = static_cast<char>(0x80 | (cp & 0x3F));
                len = 4;
            }
            buf_.insert(cursor_, u8, len);
            cursor_ += len;
        } else {
            handled = false;
        }
    } else if (auto* sk = std::get_if<SpecialKey>(&k->key)) {
        switch (*sk) {
            case SpecialKey::Enter:
                buf_.insert(buf_.begin() + cursor_, '\n');
                ++cursor_;
                break;
            case SpecialKey::Backspace:
                if (cursor_ > 0) {
                    // Handle UTF-8: back up past continuation bytes
                    int del = 1;
                    while (cursor_ - del > 0 &&
                           (buf_[cursor_ - del] & 0xC0) == 0x80) ++del;
                    buf_.erase(cursor_ - del, del);
                    cursor_ -= del;
                }
                break;
            case SpecialKey::Delete:
                if (cursor_ < n) {
                    int del = 1;
                    while (cursor_ + del < n &&
                           (buf_[cursor_ + del] & 0xC0) == 0x80) ++del;
                    buf_.erase(cursor_, del);
                }
                break;
            case SpecialKey::Left:
                if (k->mods.ctrl) cursor_ = word_left(cursor_);
                else if (cursor_ > 0) {
                    --cursor_;
                    while (cursor_ > 0 && (buf_[cursor_] & 0xC0) == 0x80) --cursor_;
                }
                break;
            case SpecialKey::Right:
                if (k->mods.ctrl) cursor_ = word_right(cursor_);
                else if (cursor_ < n) {
                    ++cursor_;
                    while (cursor_ < n && (buf_[cursor_] & 0xC0) == 0x80) ++cursor_;
                }
                break;
            case SpecialKey::Up: {
                int line = current_line();
                if (line > 0) {
                    cursor_ = pos_at_line_col(line - 1, col_in_line());
                }
                break;
            }
            case SpecialKey::Down: {
                int line = current_line();
                if (line < line_count() - 1) {
                    cursor_ = pos_at_line_col(line + 1, col_in_line());
                }
                break;
            }
            case SpecialKey::Home:
                if (k->mods.ctrl) cursor_ = 0;
                else cursor_ = line_start(cursor_);
                break;
            case SpecialKey::End:
                if (k->mods.ctrl) cursor_ = n;
                else cursor_ = line_end(cursor_);
                break;
            default:
                handled = false;
                break;
        }
    } else {
        handled = false;
    }

    if (handled) ensure_cursor_visible();
    return handled;
}

Element TextInput::render() const {
    using namespace maya::dsl;

    bool show_placeholder = buf_.empty() && !props_.placeholder.empty();

    // Split buffer into lines
    std::vector<std::string_view> lines;
    if (show_placeholder) {
        lines.push_back(props_.placeholder);
    } else {
        std::string_view sv = buf_;
        while (true) {
            auto pos = sv.find('\n');
            if (pos == std::string_view::npos) {
                lines.push_back(sv);
                break;
            }
            lines.push_back(sv.substr(0, pos));
            sv = sv.substr(pos + 1);
        }
    }

    // Determine visible range
    int total = static_cast<int>(lines.size());
    int max_vis = props_.max_height > 0 ? props_.max_height : total;
    if (props_.show_border) max_vis = std::max(1, max_vis - 2);
    int vis_start = scroll_;
    int vis_end = std::min(total, vis_start + max_vis);

    // Cursor position for rendering
    int cursor_line = 0, cursor_col = 0;
    if (focused_ && !show_placeholder) {
        cursor_line = current_line();
        cursor_col = col_in_line();
    }

    // Build line elements
    std::vector<Element> rows;
    rows.reserve(vis_end - vis_start);

    for (int i = vis_start; i < vis_end; ++i) {
        if (show_placeholder) {
            rows.push_back(text(std::string(lines[i]),
                                Style{}.with_fg(palette().dim).with_italic()));
            continue;
        }

        // Render line with cursor
        if (focused_ && i == cursor_line) {
            std::string line(lines[i]);
            if (cursor_col >= static_cast<int>(line.size())) {
                // Cursor at end of line — append block cursor
                rows.push_back(hstack()(
                    text(line, Style{}.with_fg(props_.text_color)),
                    text("█", Style{}.with_fg(props_.cursor_color))
                ));
            } else {
                // Cursor in middle — split and highlight
                std::string before = line.substr(0, cursor_col);
                std::string at(1, line[cursor_col]);
                std::string after = line.substr(cursor_col + 1);
                std::vector<Element> parts;
                if (!before.empty())
                    parts.push_back(text(std::move(before), Style{}.with_fg(props_.text_color)));
                parts.push_back(text(std::move(at),
                                     Style{}.with_fg(Color::rgb(14, 14, 22))
                                            .with_bg(props_.cursor_color)));
                if (!after.empty())
                    parts.push_back(text(std::move(after), Style{}.with_fg(props_.text_color)));
                rows.push_back(hstack()(std::move(parts)));
            }
        } else {
            rows.push_back(text(std::string(lines[i]),
                                Style{}.with_fg(props_.text_color)));
        }
    }

    // Ensure minimum height
    while (static_cast<int>(rows.size()) < props_.min_height) {
        rows.push_back(text(""));
    }

    auto builder = vstack();
    if (props_.show_border) {
        Color bc = focused_ ? props_.cursor_color : props_.border_color;
        builder = std::move(builder)
            .border(BorderStyle::Round)
            .border_color(bc)
            .padding(0, 1, 0, 1);
    }

    return builder(std::move(rows));
}

} // namespace maya::components
