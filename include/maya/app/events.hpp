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

#include <optional>
#include <string>

namespace maya {

// ── Key predicates ─────────────────────────────────────────────────────────
// All return true only if the event is a KeyEvent with the given key.

/// Match a printable character key (no modifiers required).
[[nodiscard]] bool key(const Event& ev, char c) noexcept;

/// Match a Unicode codepoint key.
[[nodiscard]] bool key(const Event& ev, char32_t cp) noexcept;

/// Match a special key (arrow, function key, enter, etc.).
[[nodiscard]] bool key(const Event& ev, SpecialKey sk) noexcept;

/// Match Ctrl+<letter>. Letter should be lowercase ('c', 'a', …).
[[nodiscard]] bool ctrl(const Event& ev, char c) noexcept;

/// Match Alt+<character>.
[[nodiscard]] bool alt(const Event& ev, char c) noexcept;

/// Match Shift+<special key> (e.g. Shift+Tab → BackTab).
[[nodiscard]] bool shift(const Event& ev, SpecialKey sk) noexcept;

/// True for any key event (useful as a catch-all in input boxes).
[[nodiscard]] bool any_key(const Event& ev) noexcept;

/// Return the KeyEvent if the event is a key event, nullptr otherwise.
[[nodiscard]] const KeyEvent* as_key(const Event& ev) noexcept;

// ── Mouse predicates ───────────────────────────────────────────────────────

/// A 0-based (col, row) terminal cell position.
struct MousePos {
    int col = 0;
    int row = 0;
};

/// True if the event is a mouse button press. Defaults to left button.
[[nodiscard]] bool mouse_clicked(const Event& ev,
                                  MouseButton btn = MouseButton::Left) noexcept;

/// True if the event is a mouse button release. Defaults to left button.
[[nodiscard]] bool mouse_released(const Event& ev,
                                   MouseButton btn = MouseButton::Left) noexcept;

/// True if the event is a mouse motion event.
[[nodiscard]] bool mouse_moved(const Event& ev) noexcept;

/// True if the mouse scroll wheel was rolled up.
[[nodiscard]] bool scrolled_up(const Event& ev) noexcept;

/// True if the mouse scroll wheel was rolled down.
[[nodiscard]] bool scrolled_down(const Event& ev) noexcept;

/// Extract the mouse position from any mouse event. Returns nullopt for non-mouse events.
[[nodiscard]] std::optional<MousePos> mouse_pos(const Event& ev) noexcept;

/// Return the MouseEvent pointer, or nullptr.
[[nodiscard]] const MouseEvent* as_mouse(const Event& ev) noexcept;

// ── Resize predicate ───────────────────────────────────────────────────────

/// True if the terminal was resized. Optionally writes the new size to w/h.
[[nodiscard]] bool resized(const Event& ev,
                            int* w = nullptr,
                            int* h = nullptr) noexcept;

// ── Paste predicate ────────────────────────────────────────────────────────

/// True if the event is a bracketed paste. Optionally writes content to *out.
[[nodiscard]] bool pasted(const Event& ev,
                           std::string* out = nullptr);

/// Extract paste event from a variant Event.
[[nodiscard]] inline const PasteEvent* as_paste(const Event& ev) noexcept {
    return std::get_if<PasteEvent>(&ev);
}

// ── Focus predicates ───────────────────────────────────────────────────────

/// True if the terminal window gained focus.
[[nodiscard]] bool focused(const Event& ev) noexcept;

/// True if the terminal window lost focus.
[[nodiscard]] bool unfocused(const Event& ev) noexcept;

// ── on() — fire-and-forget key binding ─────────────────────────────────────
// Calls `action` if the event matches the given key. Returns true if matched.
//
// Usage:
//   on(ev, 'q', [&] { quit(); });
//   on(ev, '+', '=', [&] { count++; });               // two keys, same action
//   on(ev, SpecialKey::Up, [&] { scroll(-1); });

template <typename Fn>
    requires std::invocable<Fn>
inline bool on(const Event& ev, char c, Fn&& action) {
    if (key(ev, c)) { std::forward<Fn>(action)(); return true; }
    return false;
}

template <typename Fn>
    requires std::invocable<Fn>
inline bool on(const Event& ev, char c1, char c2, Fn&& action) {
    if (key(ev, c1) || key(ev, c2)) { std::forward<Fn>(action)(); return true; }
    return false;
}

template <typename Fn>
    requires std::invocable<Fn>
inline bool on(const Event& ev, SpecialKey sk, Fn&& action) {
    if (key(ev, sk)) { std::forward<Fn>(action)(); return true; }
    return false;
}

} // namespace maya
