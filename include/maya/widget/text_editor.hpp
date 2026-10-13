#pragma once
// maya::widget::TextEditor — interactive code buffer (renders via CodeView)
//
// A real editable buffer, not a read-only view: multi-line text with a cursor,
// shift-selection, undo/redo, clipboard (internal register), and viewport
// scrolling. Key handling mirrors the usual editor bindings; rendering reuses
// CodeView so you get syntax highlight, the gutter, the git ribbon, indent
// guides, the current-line shade, the caret, and the selection for free.
//
// Unlike the read-only display widgets, a TextEditor holds mutable state and
// MUST be stored (in your Model), not built as a temporary — its render reads
// live buffer state each frame.
//
// Usage (Elm):
//   struct Model { TextEditor ed; };
//   init:    m.ed.set_text(src); m.ed.set_lang(syntax::Lang::Cpp);
//   update:  if (auto* k = as_key(ev)) m.ed.handle(*k);
//            if (auto* p = as_paste(ev)) m.ed.handle_paste(*p);
//   view:    return m.ed | dsl::grow();

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../core/overload.hpp"
#include "../terminal/input.hpp"
#include "code_view.hpp"

namespace maya {

class TextEditor {
public:
    struct Config {
        syntax::Lang lang      = syntax::Lang::Generic;
        int          tab_width = 4;
        CodeViewTheme theme    = {};
    };

    TextEditor() { lines_ = {""}; }
    explicit TextEditor(Config cfg) : cfg_(cfg) { lines_ = {""}; }

    // ── content ─────────────────────────────────────────────────────────────
    void set_text(std::string_view text) {
        lines_.clear();
        size_t pos = 0;
        while (pos <= text.size()) {
            size_t nl = text.find('\n', pos);
            size_t end = (nl == std::string_view::npos) ? text.size() : nl;
            lines_.emplace_back(text.substr(pos, end - pos));
            if (nl == std::string_view::npos) break;
            pos = nl + 1;
        }
        if (lines_.empty()) lines_ = {""};
        row_ = col_ = 0; vs_->top = 0; clear_sel(); undo_.clear(); redo_.clear();
    }
    [[nodiscard]] std::string text() const {
        std::string s;
        for (size_t i = 0; i < lines_.size(); ++i) { if (i) s += '\n'; s += lines_[i]; }
        return s;
    }
    TextEditor& set_lang(syntax::Lang l) { cfg_.lang = l; return *this; }

    [[nodiscard]] int line() const { return row_ + 1; }
    [[nodiscard]] int column() const { return col_ + 1; }
    [[nodiscard]] int line_count() const { return static_cast<int>(lines_.size()); }

    // ── input ───────────────────────────────────────────────────────────────
    void handle_paste(const PasteEvent& pe) {
        push_undo();
        if (has_sel()) delete_selection(false);
        for (size_t p = 0; p < pe.content.size(); ) {
            char32_t cp = decode_utf8(pe.content, p);
            if (cp == '\n') split_line();
            else if (!maya::unicode::is_control(cp)) insert_cp(cp);
        }
    }

    [[nodiscard]] bool handle(const KeyEvent& ev) {
        return std::visit(overload{
            [&](CharKey ck) -> bool {
                if (ev.mods.ctrl) return ctrl_key(ck.codepoint);
                if (ev.mods.alt) return false;
                push_undo_coalesced();
                if (has_sel()) delete_selection(false);
                insert_cp(ck.codepoint);
                return true;
            },
            [&](SpecialKey sk) -> bool { return special_key(sk, ev.mods); },
        }, ev.key);
    }

    // ── render (reuses CodeView) ─────────────────────────────────────────────
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        // Snapshot the buffer + cursor BY VALUE and share the scroll state via a
        // shared_ptr, so the deferred paint never dereferences `this` (the
        // editor may be converted/copied into the element tree as a temporary).
        Frame f{lines_, row_, col_, arow_, acol_, cfg_.lang, cfg_.theme};
        auto vs = vs_;
        return Element{ComponentElement{
            .render = [f = std::move(f), vs](int, int h) -> Element {
                return render_frame(f, *vs, h);
            },
            .measure = [](int mw) -> Size { return Size{Columns(mw), Rows(1)}; },
        }};
    }

private:
    struct ViewState { int top = 0; int last_h = 20; };
    struct Frame {
        std::vector<std::string> lines;
        int row, col, arow, acol;
        syntax::Lang lang;
        CodeViewTheme theme;
    };
    std::shared_ptr<ViewState> vs_ = std::make_shared<ViewState>();

    Config                    cfg_{};
    std::vector<std::string>  lines_{""};
    int                       row_ = 0, col_ = 0;   // cursor (col = byte index)
    int                       arow_ = -1, acol_ = -1; // selection anchor
    std::string               clip_;

    struct Snapshot { std::vector<std::string> lines; int row, col; };
    std::vector<Snapshot> undo_, redo_;
    bool                  coalescing_ = false;

    // ── selection helpers ─────────────────────────────────────────────────
    bool has_sel() const { return arow_ >= 0 && !(arow_ == row_ && acol_ == col_); }
    void clear_sel() { arow_ = -1; acol_ = -1; }
    void start_sel_if_needed(bool shift) {
        if (shift) { if (arow_ < 0) { arow_ = row_; acol_ = col_; } }
        else clear_sel();
    }
    // ordered (start,end) of the selection
    void sel_range(int& sr, int& sc, int& er, int& ec) const {
        sr = arow_; sc = acol_; er = row_; ec = col_;
        if (sr > er || (sr == er && sc > ec)) { std::swap(sr, er); std::swap(sc, ec); }
    }

    // ── UTF-8 cursor stepping (col is a byte index) ───────────────────────
    const std::string& cur() const { return lines_[static_cast<size_t>(row_)]; }
    std::string& cur() { return lines_[static_cast<size_t>(row_)]; }
    static int cp_len_at(const std::string& s, int i) {
        unsigned char c = static_cast<unsigned char>(s[static_cast<size_t>(i)]);
        int n = (c < 0x80) ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
        return std::min(n, static_cast<int>(s.size()) - i);
    }
    static int prev_cp_start(const std::string& s, int i) {
        int j = i - 1;
        while (j > 0 && (static_cast<unsigned char>(s[static_cast<size_t>(j)]) & 0xC0) == 0x80) --j;
        return std::max(0, j);
    }

    // ── mutations ─────────────────────────────────────────────────────────
    void insert_cp(char32_t cp) {
        char buf[4]; int n = encode_utf8(cp, buf);
        cur().insert(static_cast<size_t>(col_), buf, static_cast<size_t>(n));
        col_ += n;
    }
    void split_line() {
        std::string tail = cur().substr(static_cast<size_t>(col_));
        cur().erase(static_cast<size_t>(col_));
        lines_.insert(lines_.begin() + row_ + 1, tail);
        row_++; col_ = 0;
    }
    void backspace() {
        if (has_sel()) { delete_selection(false); return; }
        if (col_ > 0) {
            int p = prev_cp_start(cur(), col_);
            cur().erase(static_cast<size_t>(p), static_cast<size_t>(col_ - p));
            col_ = p;
        } else if (row_ > 0) {
            col_ = static_cast<int>(lines_[static_cast<size_t>(row_ - 1)].size());
            lines_[static_cast<size_t>(row_ - 1)] += cur();
            lines_.erase(lines_.begin() + row_);
            row_--;
        }
    }
    void del_forward() {
        if (has_sel()) { delete_selection(false); return; }
        if (col_ < static_cast<int>(cur().size())) {
            cur().erase(static_cast<size_t>(col_), static_cast<size_t>(cp_len_at(cur(), col_)));
        } else if (row_ + 1 < static_cast<int>(lines_.size())) {
            cur() += lines_[static_cast<size_t>(row_ + 1)];
            lines_.erase(lines_.begin() + row_ + 1);
        }
    }
    std::string extract_selection() const {
        int sr, sc, er, ec; sel_range(sr, sc, er, ec);
        if (sr == er) return lines_[static_cast<size_t>(sr)].substr(
            static_cast<size_t>(sc), static_cast<size_t>(ec - sc));
        std::string out = lines_[static_cast<size_t>(sr)].substr(static_cast<size_t>(sc));
        for (int r = sr + 1; r < er; ++r) { out += '\n'; out += lines_[static_cast<size_t>(r)]; }
        out += '\n'; out += lines_[static_cast<size_t>(er)].substr(0, static_cast<size_t>(ec));
        return out;
    }
    void delete_selection(bool to_clip) {
        int sr, sc, er, ec; sel_range(sr, sc, er, ec);
        if (to_clip) clip_ = extract_selection();
        if (sr == er) {
            lines_[static_cast<size_t>(sr)].erase(static_cast<size_t>(sc),
                                                  static_cast<size_t>(ec - sc));
        } else {
            std::string head = lines_[static_cast<size_t>(sr)].substr(0, static_cast<size_t>(sc));
            std::string tail = lines_[static_cast<size_t>(er)].substr(static_cast<size_t>(ec));
            head += tail;
            lines_.erase(lines_.begin() + sr + 1, lines_.begin() + er + 1);
            lines_[static_cast<size_t>(sr)] = head;
        }
        row_ = sr; col_ = sc; clear_sel();
    }
    void insert_text(std::string_view t) {
        for (size_t p = 0; p < t.size(); ) {
            char32_t cp = decode_utf8_sv(t, p);
            if (cp == '\n') split_line(); else insert_cp(cp);
        }
    }

    // ── movement ──────────────────────────────────────────────────────────
    void move_left(bool shift) {
        start_sel_if_needed(shift);
        if (col_ > 0) col_ = prev_cp_start(cur(), col_);
        else if (row_ > 0) { row_--; col_ = static_cast<int>(cur().size()); }
        if (!shift) clear_sel();
    }
    void move_right(bool shift) {
        start_sel_if_needed(shift);
        if (col_ < static_cast<int>(cur().size())) col_ += cp_len_at(cur(), col_);
        else if (row_ + 1 < static_cast<int>(lines_.size())) { row_++; col_ = 0; }
        if (!shift) clear_sel();
    }
    void move_vert(int dr, bool shift) {
        start_sel_if_needed(shift);
        int nr = std::clamp(row_ + dr, 0, static_cast<int>(lines_.size()) - 1);
        row_ = nr;
        col_ = std::min(col_, static_cast<int>(cur().size()));
        col_ = clamp_to_cp_boundary(cur(), col_);
        if (!shift) clear_sel();
    }
    void move_home(bool shift) { start_sel_if_needed(shift); col_ = 0; if (!shift) clear_sel(); }
    void move_end(bool shift)  { start_sel_if_needed(shift); col_ = static_cast<int>(cur().size()); if (!shift) clear_sel(); }
    static int clamp_to_cp_boundary(const std::string& s, int i) {
        while (i > 0 && i < static_cast<int>(s.size()) &&
               (static_cast<unsigned char>(s[static_cast<size_t>(i)]) & 0xC0) == 0x80) --i;
        return i;
    }

    // ── undo / redo ────────────────────────────────────────────────────────
    void push_undo() {
        undo_.push_back({lines_, row_, col_});
        if (undo_.size() > 500) undo_.erase(undo_.begin());
        redo_.clear();
        coalescing_ = false;
    }
    void push_undo_coalesced() {
        if (coalescing_) return;            // group a run of typed chars
        push_undo();
        coalescing_ = true;
    }
    void undo() {
        if (undo_.empty()) return;
        redo_.push_back({lines_, row_, col_});
        auto s = std::move(undo_.back()); undo_.pop_back();
        lines_ = std::move(s.lines); row_ = s.row; col_ = s.col; clear_sel();
        coalescing_ = false;
    }
    void redo() {
        if (redo_.empty()) return;
        undo_.push_back({lines_, row_, col_});
        auto s = std::move(redo_.back()); redo_.pop_back();
        lines_ = std::move(s.lines); row_ = s.row; col_ = s.col; clear_sel();
    }

    // ── key dispatch ─────────────────────────────────────────────────────
    bool special_key(SpecialKey sk, Modifiers m) {
        switch (sk) {
            case SpecialKey::Left:  move_left(m.shift);  return true;
            case SpecialKey::Right: move_right(m.shift); return true;
            case SpecialKey::Up:    move_vert(-1, m.shift); return true;
            case SpecialKey::Down:  move_vert(+1, m.shift); return true;
            case SpecialKey::Home:  move_home(m.shift); return true;
            case SpecialKey::End:   move_end(m.shift);  return true;
            case SpecialKey::Enter: push_undo(); if (has_sel()) delete_selection(false);
                                    split_line(); return true;
            case SpecialKey::Backspace: push_undo_coalesced(); backspace(); return true;
            case SpecialKey::Delete:    push_undo_coalesced(); del_forward(); return true;
            case SpecialKey::Tab:   push_undo(); if (has_sel()) delete_selection(false);
                                    for (int i = 0; i < cfg_.tab_width; ++i) insert_cp(' ');
                                    return true;
            default: return false;
        }
    }
    bool ctrl_key(char32_t cp) {
        switch (cp) {
            case 'z': case 'Z': undo(); return true;
            case 'y': case 'Y': redo(); return true;
            case 'a': case 'A': arow_ = 0; acol_ = 0;
                                row_ = static_cast<int>(lines_.size()) - 1;
                                col_ = static_cast<int>(cur().size()); return true;
            case 'c': case 'C': if (has_sel()) clip_ = extract_selection(); return true;
            case 'x': case 'X': if (has_sel()) { push_undo(); delete_selection(true); } return true;
            case 'v': case 'V': push_undo(); if (has_sel()) delete_selection(false);
                                insert_text(clip_); return true;
            case 'k': case 'K': push_undo();               // kill to end of line
                                cur().erase(static_cast<size_t>(col_)); return true;
            default: return false;
        }
    }

    // ── rendering ────────────────────────────────────────────────────────
    static Element render_frame(const Frame& f, ViewState& vs, int h) {
        vs.last_h = std::max(1, h);
        const int nlines = static_cast<int>(f.lines.size());
        // keep the cursor within the viewport
        if (f.row < vs.top) vs.top = f.row;
        else if (f.row >= vs.top + vs.last_h) vs.top = f.row - vs.last_h + 1;
        vs.top = std::clamp(vs.top, 0, std::max(0, nlines - vs.last_h));

        int lo = vs.top, hi = std::min(nlines, vs.top + vs.last_h);
        std::string src;
        for (int i = lo; i < hi; ++i) { if (i > lo) src += '\n'; src += f.lines[static_cast<size_t>(i)]; }

        CodeView cv{src, {.lang = f.lang, .theme = f.theme, .first_line = lo + 1}};
        cv.set_caret(f.row + 1, f.col);
        if (f.arow >= 0 && !(f.arow == f.row && f.acol == f.col)) {
            int sr = f.arow, sc = f.acol, er = f.row, ec = f.col;
            if (sr > er || (sr == er && sc > ec)) { std::swap(sr, er); std::swap(sc, ec); }
            cv.set_selection(sr + 1, sc, er + 1, ec);
        }
        return cv.build();
    }

    // ── utf-8 codecs ───────────────────────────────────────────────────────
    static int encode_utf8(char32_t cp, char out[4]) {
        if (cp < 0x80) { out[0] = static_cast<char>(cp); return 1; }
        if (cp < 0x800) { out[0] = static_cast<char>(0xC0 | (cp >> 6));
                          out[1] = static_cast<char>(0x80 | (cp & 0x3F)); return 2; }
        if (cp < 0x10000) { out[0] = static_cast<char>(0xE0 | (cp >> 12));
                            out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            out[2] = static_cast<char>(0x80 | (cp & 0x3F)); return 3; }
        out[0] = static_cast<char>(0xF0 | (cp >> 18));
        out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[3] = static_cast<char>(0x80 | (cp & 0x3F)); return 4;
    }
    static char32_t decode_utf8(const std::string& s, size_t& p) {
        std::string_view sv{s}; return decode_utf8_sv(sv, p);
    }
    static char32_t decode_utf8_sv(std::string_view s, size_t& p) {
        unsigned char c = static_cast<unsigned char>(s[p]);
        char32_t cp; int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 0x6) { cp = c & 0x1F; n = 2; }
        else if ((c >> 4) == 0xE) { cp = c & 0x0F; n = 3; }
        else { cp = c & 0x07; n = 4; }
        for (int i = 1; i < n && p + i < s.size(); ++i)
            cp = (cp << 6) | (static_cast<unsigned char>(s[p + i]) & 0x3F);
        p += n;
        return cp;
    }
};

} // namespace maya
