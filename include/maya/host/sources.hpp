#pragma once
// maya/host/sources.hpp — what the terminal reports, as jaal subscriptions.
//
// One router per event kind, so a program subscribes to exactly what it uses
// and a host that doesn't produce, say, mouse events would reject a program
// that asks for them at compile time.
//
//   using Sub = jaal::Sub<Msg, maya::on_key, maya::on_resize>;
//
// `keys<Sub>({...})` is the common case spelled once; anything richer is
// Sub::on(on_key{}, fn).

#include <initializer_list>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include <jaal/jaal.hpp>

#include "../element/element.hpp"
#include "../terminal/input.hpp"
#include "../device/keys.hpp"     // key_is, SpecialKey

namespace maya {

// ── what the host reports, as subscription kinds ───────────────────────────
using on_key    = jaal::router<KeyEvent,    "on_key">;
using on_mouse  = jaal::router<MouseEvent,  "on_mouse">;
using on_paste  = jaal::router<PasteEvent,  "on_paste">;
using on_focus  = jaal::router<FocusEvent,  "on_focus">;
using on_resize = jaal::router<ResizeEvent, "on_resize">;

/// A jaal program that maya can draw: it has a view() returning an Element.
template <class P>
concept Program = jaal::Program<P> && jaal::Viewable<P, Element>;

// ── keys: the most common subscription ───────────────────────────────────────────
// A table from keys to messages, as a subscription in the program's own Sub
// type (so the router is checked against the program's row like any other):
//
//   static Sub subscribe(const Model&) {
//       return keys<Sub>({{'q', Quit{}}, {SpecialKey::Up, Inc{}}});
//   }
//
// Matching is key_is(): a plain key with no modifiers, so 'q' doesn't also
// fire on Ctrl+Q. For anything richer, write Sub::on(on_key{}, fn).

using KeySpec = std::variant<char, SpecialKey>;

template <class S>
[[nodiscard]] S keys(
    std::initializer_list<std::pair<KeySpec, typename S::msg_type>> entries) {
    using Msg = typename S::msg_type;
    return S::on(on_key{},
        [table = std::vector(entries.begin(), entries.end())](const KeyEvent& k)
            -> std::optional<Msg> {
            for (const auto& [key, msg] : table) {
                const bool hit = std::visit([&](auto want) { return key_is(k, want); }, key);
                if (hit) return msg;
            }
            return std::nullopt;
        });
}

}  // namespace maya
