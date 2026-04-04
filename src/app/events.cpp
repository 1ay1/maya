#include "maya/app/events.hpp"

namespace maya {

// ── Internal extraction helpers ──────────────────────────────────────────────

namespace {

template <typename T>
constexpr auto as(const Event& ev) noexcept -> const T* {
    return std::get_if<T>(&ev);
}

template <typename T>
constexpr auto key_as(const KeyEvent& ke) noexcept -> const T* {
    return std::get_if<T>(&ke.key);
}

auto get_key_event(const Event& ev) noexcept -> const KeyEvent* {
    return as<KeyEvent>(ev);
}

auto get_mouse_event(const Event& ev) noexcept -> const MouseEvent* {
    return as<MouseEvent>(ev);
}

} // namespace

// ── Key predicates ─────────────────────────────────────────────────────────

bool key(const Event& ev, char c) noexcept {
    auto* ke = get_key_event(ev);
    auto* ck = ke ? key_as<CharKey>(*ke) : nullptr;
    return ck && ck->codepoint == static_cast<char32_t>(c);
}

bool key(const Event& ev, char32_t cp) noexcept {
    auto* ke = get_key_event(ev);
    auto* ck = ke ? key_as<CharKey>(*ke) : nullptr;
    return ck && ck->codepoint == cp;
}

bool key(const Event& ev, SpecialKey sk) noexcept {
    auto* ke = get_key_event(ev);
    auto* sp = ke ? key_as<SpecialKey>(*ke) : nullptr;
    return sp && *sp == sk;
}

bool ctrl(const Event& ev, char c) noexcept {
    auto* ke = get_key_event(ev);
    if (!ke || !ke->mods.ctrl) return false;
    auto* ck = key_as<CharKey>(*ke);
    return ck && ck->codepoint == static_cast<char32_t>(c);
}

bool alt(const Event& ev, char c) noexcept {
    auto* ke = get_key_event(ev);
    if (!ke || !ke->mods.alt) return false;
    auto* ck = key_as<CharKey>(*ke);
    return ck && ck->codepoint == static_cast<char32_t>(c);
}

bool shift(const Event& ev, SpecialKey sk) noexcept {
    auto* ke = get_key_event(ev);
    if (!ke || !ke->mods.shift) return false;
    auto* sp = key_as<SpecialKey>(*ke);
    return sp && *sp == sk;
}

bool any_key(const Event& ev) noexcept {
    return get_key_event(ev) != nullptr;
}

const KeyEvent* as_key(const Event& ev) noexcept {
    return get_key_event(ev);
}

// ── Mouse predicates ───────────────────────────────────────────────────────

bool mouse_clicked(const Event& ev, MouseButton btn) noexcept {
    auto* me = get_mouse_event(ev);
    return me && me->kind == MouseEventKind::Press && me->button == btn;
}

bool mouse_released(const Event& ev, MouseButton btn) noexcept {
    auto* me = get_mouse_event(ev);
    return me && me->kind == MouseEventKind::Release && me->button == btn;
}

bool mouse_moved(const Event& ev) noexcept {
    auto* me = get_mouse_event(ev);
    return me && me->kind == MouseEventKind::Move;
}

bool scrolled_up(const Event& ev) noexcept {
    return mouse_clicked(ev, MouseButton::ScrollUp);
}

bool scrolled_down(const Event& ev) noexcept {
    return mouse_clicked(ev, MouseButton::ScrollDown);
}

std::optional<MousePos> mouse_pos(const Event& ev) noexcept {
    auto* me = get_mouse_event(ev);
    if (!me) return std::nullopt;
    return MousePos{me->x.value, me->y.value};
}

const MouseEvent* as_mouse(const Event& ev) noexcept {
    return get_mouse_event(ev);
}

// ── Resize predicate ───────────────────────────────────────────────────────

bool resized(const Event& ev, int* w, int* h) noexcept {
    auto* re = as<ResizeEvent>(ev);
    if (!re) return false;
    if (w) *w = re->width.value;
    if (h) *h = re->height.value;
    return true;
}

// ── Paste predicate ────────────────────────────────────────────────────────

bool pasted(const Event& ev, std::string* out) {
    auto* pe = as<PasteEvent>(ev);
    if (!pe) return false;
    if (out) *out = pe->content;
    return true;
}

// ── Focus predicates ───────────────────────────────────────────────────────

bool focused(const Event& ev) noexcept {
    auto* fe = as<FocusEvent>(ev);
    return fe && fe->focused;
}

bool unfocused(const Event& ev) noexcept {
    auto* fe = as<FocusEvent>(ev);
    return fe && !fe->focused;
}

} // namespace maya
