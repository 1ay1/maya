#pragma once
// maya/device/keys.hpp — which keys are navigation (each gets its own frame), and
// the key predicates key_is / ctrl_is / alt_is.

#include <variant>

#include "../terminal/input.hpp"

namespace maya {

namespace detail {

// ── Per-key frame fidelity ────────────────────────────────────────────
//
// A fast terminal delivers a whole key-repeat run in ONE read(), and the
// host folds every event but paints ONCE for the batch. For most
// input that is exactly right: the intermediate states are not interesting
// and painting them is wasted work.
//
// For NAVIGATION it is wrong, and it is what users report as "I hold Down,
// one press does nothing, then the next moves two rows". Nothing was
// dropped — the row was computed and overwritten before it reached the
// terminal. With two arrows per read the visible sequence is 2 → 4 → 6
// where the user pressed 1 2 3 4 5 6, so half the rows they steered
// through never existed on screen.
//
// The cursor position IS the feedback for an arrow key, so it has to be
// shown. This predicate marks the events whose intermediate frames are
// worth painting: plain arrows, Home/End, PageUp/PageDown, Tab. Everything
// else (typing, mouse motion, paste, resize) keeps batching, because there
// the end state is the only state anyone wants.
//
// Deliberately NOT "render every event": a paste arrives as hundreds of
// CharKeys and painting each one would turn a paste into a visible crawl.
[[nodiscard]] inline bool is_navigation_key(const Event& ev) noexcept {
    const auto* ke = std::get_if<KeyEvent>(&ev);
    if (!ke) return false;
    // A modified arrow is usually a different verb (word-jump, resize pane),
    // but it is still navigation and still wants its own frame. Ctrl/Alt
    // combos that are NOT arrows fall through to the default batching.
    if (const auto* sk = std::get_if<SpecialKey>(&ke->key)) {
        switch (*sk) {
            case SpecialKey::Up:
            case SpecialKey::Down:
            case SpecialKey::Left:
            case SpecialKey::Right:
            case SpecialKey::Home:
            case SpecialKey::End:
            case SpecialKey::PageUp:
            case SpecialKey::PageDown:
            case SpecialKey::Tab:
            case SpecialKey::BackTab:
                return true;
            default:
                return false;
        }
    }
    return false;
}

}  // namespace detail

// ============================================================================
// Key event predicates — pure functions for use inside subscribe() filters
// ============================================================================

[[nodiscard]] inline bool key_is(const KeyEvent& k, char c) noexcept {
    auto* ck = std::get_if<CharKey>(&k.key);
    return ck && ck->codepoint == static_cast<char32_t>(c) && k.mods.none();
}

[[nodiscard]] inline bool key_is(const KeyEvent& k, char32_t c) noexcept {
    auto* ck = std::get_if<CharKey>(&k.key);
    return ck && ck->codepoint == c && k.mods.none();
}

[[nodiscard]] inline bool key_is(const KeyEvent& k, SpecialKey s) noexcept {
    auto* sk = std::get_if<SpecialKey>(&k.key);
    return sk && *sk == s && k.mods.none();
}

[[nodiscard]] inline bool ctrl_is(const KeyEvent& k, char c) noexcept {
    auto* ck = std::get_if<CharKey>(&k.key);
    return ck && ck->codepoint == static_cast<char32_t>(c) && k.mods.ctrl && !k.mods.alt;
}

[[nodiscard]] inline bool alt_is(const KeyEvent& k, char c) noexcept {
    auto* ck = std::get_if<CharKey>(&k.key);
    return ck && ck->codepoint == static_cast<char32_t>(c) && k.mods.alt && !k.mods.ctrl;
}

} // namespace maya
