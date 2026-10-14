#pragma once
// maya/host/sources.hpp — what the terminal reports, and how a program hears it.
//
// The terminal sends one of terminal_events. A program handles input the way
// it handles messages, one jaal on() overload per kind it cares about,
// reading the model as it is when the event arrives:
//
//   static std::optional<Msg> on(const Model& m, const KeyEvent& k);
//   static std::optional<Msg> on(const Model& m, const ResizeEvent& r);
//
// Kinds with no overload are dropped. keymap<Msg> is the common key table.

#include <array>
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <jaal/jaal.hpp>

#include "../element/element.hpp"
#include "../terminal/input.hpp"
#include "../device/keys.hpp"     // key_is, SpecialKey

namespace maya {

/// Everything the terminal sends a program. Same type as maya::Event.
using terminal_events = Event;
static_assert(std::is_same_v<terminal_events,
                             std::variant<KeyEvent, MouseEvent, PasteEvent, FocusEvent, ResizeEvent>>);

/// A jaal program that maya can draw: it has a view() returning an Element.
template <class P>
concept Program = jaal::Program<P> && jaal::Viewable<P, Element>;

/// A plain char or a SpecialKey, matched with no modifiers (key_is).
using KeySpec = std::variant<char, SpecialKey>;

/// A fixed key → Msg table with O(1) lookup. Build it once, as a static:
///
///   static std::optional<Msg> on(const Model&, const KeyEvent& k) {
///       static const maya::keymap<Msg> keys{{'q', Quit{}}, {SpecialKey::Up, Up{}}};
///       return keys(k);
///   }
///
/// Same matching as key_is: no modifiers. The first entry for a key wins.
template <class Msg>
class keymap {
  public:
    keymap(std::initializer_list<std::pair<KeySpec, Msg>> entries) {
        assert(entries.size() < kNone && "keymap: too many entries");
        chars_.fill(kNone);
        special_.fill(kNone);
        msgs_.reserve(entries.size());
        for (const auto& [spec, msg] : entries) {
            const auto idx = static_cast<std::uint8_t>(msgs_.size());
            msgs_.push_back(msg);
            std::uint8_t& slot = std::holds_alternative<char>(spec)
                ? chars_[static_cast<unsigned char>(std::get<char>(spec))]
                : special_[static_cast<std::size_t>(std::get<SpecialKey>(spec))];
            if (slot == kNone) slot = idx;
        }
    }

    [[nodiscard]] std::optional<Msg> operator()(const KeyEvent& k) const {
        if (!k.mods.none()) return std::nullopt;
        std::uint8_t idx = kNone;
        if (const auto* ck = std::get_if<CharKey>(&k.key)) {
            if (ck->codepoint < 256) idx = chars_[ck->codepoint];
        } else {
            idx = special_[static_cast<std::size_t>(std::get<SpecialKey>(k.key))];
        }
        if (idx == kNone) return std::nullopt;
        return msgs_[idx];
    }

  private:
    static constexpr std::uint8_t kNone = 0xFF;
    static constexpr std::size_t  kSpecial = static_cast<std::size_t>(SpecialKey::F12) + 1;

    std::array<std::uint8_t, 256>      chars_{};
    std::array<std::uint8_t, kSpecial> special_{};
    std::vector<Msg>                   msgs_;
};

}  // namespace maya
