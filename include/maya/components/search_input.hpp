#pragma once
// maya::components::SearchInput — Single-line search with live filtering
//
//   SearchInput search({.placeholder = "Search threads..."});
//
//   // In event handler:
//   search.update(ev);
//
//   // Access query:
//   search.query()
//
//   // Render:
//   search.render()

#include "core.hpp"

namespace maya::components {

struct SearchInputProps {
    std::string placeholder = "Search...";
    std::string icon        = "🔍";
    Color       color       = palette().primary;
    bool        focused     = true;
};

class SearchInput {
    std::string buf_;
    int cursor_ = 0;
    bool focused_;
    SearchInputProps props_;

    bool is_word_char(char c) const {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_';
    }

    int word_left(int pos) const {
        if (pos <= 0) return 0;
        int i = pos - 1;
        while (i > 0 && !is_word_char(buf_[i])) --i;
        while (i > 0 && is_word_char(buf_[i - 1])) --i;
        return i;
    }

    int word_right(int pos) const {
        int n = static_cast<int>(buf_.size());
        if (pos >= n) return n;
        int i = pos;
        while (i < n && !is_word_char(buf_[i])) ++i;
        while (i < n && is_word_char(buf_[i])) ++i;
        return i;
    }

public:
    explicit SearchInput(SearchInputProps props = {})
        : focused_(props.focused), props_(std::move(props)) {}

    [[nodiscard]] const std::string& query() const { return buf_; }
    void set_query(std::string q) { buf_ = std::move(q); cursor_ = static_cast<int>(buf_.size()); }
    void clear() { buf_.clear(); cursor_ = 0; }
    [[nodiscard]] bool empty() const { return buf_.empty(); }
    void focus() { focused_ = true; }
    void blur() { focused_ = false; }

    bool update(const Event& ev) {
        if (!focused_) return false;

        auto* k = as_key(ev);
        if (!k) return false;

        int n = static_cast<int>(buf_.size());
        bool handled = true;

        if (auto* ch = std::get_if<CharKey>(&k->key)) {
            char c = static_cast<char>(ch->codepoint);
            if (ch->codepoint < 128 && c >= 32) {
                buf_.insert(buf_.begin() + cursor_, c);
                ++cursor_;
            } else {
                handled = false;
            }
        } else if (auto* sk = std::get_if<SpecialKey>(&k->key)) {
            switch (*sk) {
                case SpecialKey::Backspace:
                    if (cursor_ > 0) { buf_.erase(--cursor_, 1); }
                    break;
                case SpecialKey::Delete:
                    if (cursor_ < n) buf_.erase(cursor_, 1);
                    break;
                case SpecialKey::Left:
                    if (k->mods.ctrl) cursor_ = word_left(cursor_);
                    else if (cursor_ > 0) --cursor_;
                    break;
                case SpecialKey::Right:
                    if (k->mods.ctrl) cursor_ = word_right(cursor_);
                    else if (cursor_ < n) ++cursor_;
                    break;
                case SpecialKey::Home: cursor_ = 0; break;
                case SpecialKey::End:  cursor_ = n; break;
                default: handled = false; break;
            }
        } else {
            handled = false;
        }

        // Ctrl+U: clear line
        if (ctrl(ev, 'u')) { buf_.clear(); cursor_ = 0; return true; }

        return handled;
    }

    [[nodiscard]] Element render() const {
        using namespace maya::dsl;

        std::vector<Element> parts;

        // Search icon
        parts.push_back(text(props_.icon, Style{}.with_fg(palette().muted)));

        if (buf_.empty()) {
            parts.push_back(text(props_.placeholder,
                                 Style{}.with_fg(palette().dim).with_italic()));
            if (focused_) {
                parts.push_back(text("█", Style{}.with_fg(props_.color)));
            }
        } else {
            // Buffer with cursor
            if (focused_) {
                std::string before = buf_.substr(0, cursor_);
                if (cursor_ >= static_cast<int>(buf_.size())) {
                    parts.push_back(text(std::move(before), Style{}.with_fg(palette().text)));
                    parts.push_back(text("█", Style{}.with_fg(props_.color)));
                } else {
                    std::string at(1, buf_[cursor_]);
                    std::string after = buf_.substr(cursor_ + 1);
                    if (!before.empty())
                        parts.push_back(text(std::move(before), Style{}.with_fg(palette().text)));
                    parts.push_back(text(std::move(at),
                                         Style{}.with_fg(Color::rgb(14, 14, 22))
                                                .with_bg(props_.color)));
                    if (!after.empty())
                        parts.push_back(text(std::move(after), Style{}.with_fg(palette().text)));
                }
            } else {
                parts.push_back(text(buf_, Style{}.with_fg(palette().text)));
            }
        }

        Color bc = focused_ ? props_.color : palette().border;
        return hstack()
            .gap(1)
            .border(BorderStyle::Round)
            .border_color(bc)
            .padding(0, 1, 0, 1)(std::move(parts));
    }
};

} // namespace maya::components
