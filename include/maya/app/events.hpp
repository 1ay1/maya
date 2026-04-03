#pragma once
// maya::events — Stable predicate helpers for Event dispatch
//
// Instead of pattern-matching std::variant<KeyEvent, MouseEvent, ...> by hand,
// use these free functions. The internal Event representation can change; these
// predicates are the stable interface users depend on.
//
// Usage:
//   if (key(ev, 'q'))              return false;   // char
//   if (key(ev, SpecialKey::Up))   scroll(-1);     // special key
//   if (ctrl(ev, 'c'))             quit();         // modifier combo
//   if (mouse_clicked(ev))         { auto p = mouse_pos(ev); }
//   if (resized(ev, &w, &h))       relayout(w, h);

#include "../terminal/input.hpp"

namespace maya {

// ── Key predicates ─────────────────────────────────────────────────────────
// All return true only if the event is a KeyEvent with the given key.

/// Match a printable character key (no modifiers required).
[[nodiscard]] inline bool key(const Event& ev, char c) noexcept {
    const auto* ke = std::get_if<KeyEvent>(&ev);
    if (!ke) return false;
    const auto* ck = std::get_if<CharKey>(&ke->key);
    return ck && ck->codepoint == static_cast<char32_t>(c);
}

/// Match a Unicode codepoint key.
[[nodiscard]] inline bool key(const Event& ev, char32_t cp) noexcept {
    const auto* ke = std::get_if<KeyEvent>(&ev);
    if (!ke) return false;
    const auto* ck = std::get_if<CharKey>(&ke->key);
    return ck && ck->codepoint == cp;
}

/// Match a special key (arrow, function key, enter, etc.).
[[nodiscard]] inline bool key(const Event& ev, SpecialKey sk) noexcept {
    const auto* ke = std::get_if<KeyEvent>(&ev);
    if (!ke) return false;
    const auto* sp = std::get_if<SpecialKey>(&ke->key);
    return sp && *sp == sk;
}

/// Match Ctrl+<letter>. Letter should be lowercase ('c', 'a', …).
[[nodiscard]] inline bool ctrl(const Event& ev, char c) noexcept {
    const auto* ke = std::get_if<KeyEvent>(&ev);
    if (!ke || !ke->mods.ctrl) return false;
    const auto* ck = std::get_if<CharKey>(&ke->key);
    return ck && ck->codepoint == static_cast<char32_t>(c);
}

/// Match Alt+<character>.
[[nodiscard]] inline bool alt(const Event& ev, char c) noexcept {
    const auto* ke = std::get_if<KeyEvent>(&ev);
    if (!ke || !ke->mods.alt) return false;
    const auto* ck = std::get_if<CharKey>(&ke->key);
    return ck && ck->codepoint == static_cast<char32_t>(c);
}

/// Match Shift+<special key> (e.g. Shift+Tab → BackTab).
[[nodiscard]] inline bool shift(const Event& ev, SpecialKey sk) noexcept {
    const auto* ke = std::get_if<KeyEvent>(&ev);
    if (!ke || !ke->mods.shift) return false;
    const auto* sp = std::get_if<SpecialKey>(&ke->key);
    return sp && *sp == sk;
}

/// True for any key event (useful as a catch-all in input boxes).
[[nodiscard]] inline bool any_key(const Event& ev) noexcept {
    return std::get_if<KeyEvent>(&ev) != nullptr;
}

/// Return the KeyEvent if the event is a key event, nullptr otherwise.
[[nodiscard]] inline const KeyEvent* as_key(const Event& ev) noexcept {
    return std::get_if<KeyEvent>(&ev);
}

// ── Mouse predicates ───────────────────────────────────────────────────────

/// A 0-based (col, row) terminal cell position.
struct MousePos {
    int col = 0;
    int row = 0;
};

/// True if the event is a mouse button press. Defaults to left button.
[[nodiscard]] inline bool mouse_clicked(const Event& ev,
                                         MouseButton btn = MouseButton::Left) noexcept {
    const auto* me = std::get_if<MouseEvent>(&ev);
    return me && me->kind == MouseEventKind::Press && me->button == btn;
}

/// True if the event is a mouse button release. Defaults to left button.
[[nodiscard]] inline bool mouse_released(const Event& ev,
                                          MouseButton btn = MouseButton::Left) noexcept {
    const auto* me = std::get_if<MouseEvent>(&ev);
    return me && me->kind == MouseEventKind::Release && me->button == btn;
}

/// True if the event is a mouse motion event.
[[nodiscard]] inline bool mouse_moved(const Event& ev) noexcept {
    const auto* me = std::get_if<MouseEvent>(&ev);
    return me && me->kind == MouseEventKind::Move;
}

/// True if the mouse scroll wheel was rolled up.
[[nodiscard]] inline bool scrolled_up(const Event& ev) noexcept {
    return mouse_clicked(ev, MouseButton::ScrollUp);
}

/// True if the mouse scroll wheel was rolled down.
[[nodiscard]] inline bool scrolled_down(const Event& ev) noexcept {
    return mouse_clicked(ev, MouseButton::ScrollDown);
}

/// Extract the mouse position from any mouse event. Returns nullopt for non-mouse events.
[[nodiscard]] inline std::optional<MousePos> mouse_pos(const Event& ev) noexcept {
    const auto* me = std::get_if<MouseEvent>(&ev);
    if (!me) return std::nullopt;
    return MousePos{me->x.value, me->y.value};
}

/// Return the MouseEvent pointer, or nullptr.
[[nodiscard]] inline const MouseEvent* as_mouse(const Event& ev) noexcept {
    return std::get_if<MouseEvent>(&ev);
}

// ── Resize predicate ───────────────────────────────────────────────────────

/// True if the terminal was resized. Optionally writes the new size to w/h.
[[nodiscard]] inline bool resized(const Event& ev,
                                   int* w = nullptr,
                                   int* h = nullptr) noexcept {
    const auto* re = std::get_if<ResizeEvent>(&ev);
    if (!re) return false;
    if (w) *w = re->width.value;
    if (h) *h = re->height.value;
    return true;
}

// ── Paste predicate ────────────────────────────────────────────────────────

/// True if the event is a bracketed paste. Optionally writes content to *out.
[[nodiscard]] inline bool pasted(const Event& ev,
                                  std::string* out = nullptr) {
    const auto* pe = std::get_if<PasteEvent>(&ev);
    if (!pe) return false;
    if (out) *out = pe->content;
    return true;
}

// ── Focus predicates ───────────────────────────────────────────────────────

/// True if the terminal window gained focus.
[[nodiscard]] inline bool focused(const Event& ev) noexcept {
    const auto* fe = std::get_if<FocusEvent>(&ev);
    return fe && fe->focused;
}

/// True if the terminal window lost focus.
[[nodiscard]] inline bool unfocused(const Event& ev) noexcept {
    const auto* fe = std::get_if<FocusEvent>(&ev);
    return fe && !fe->focused;
}

} // namespace maya
