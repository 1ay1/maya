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
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <stdexcept>
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

namespace host_detail {
template <class E> inline constexpr bool sent_by_terminal =
    std::is_same_v<E, KeyEvent> || std::is_same_v<E, MouseEvent> || std::is_same_v<E, PasteEvent>
    || std::is_same_v<E, FocusEvent> || std::is_same_v<E, ResizeEvent> || std::is_same_v<E, jaal::sig>;
template <class E> inline constexpr bool all_sent_by_terminal = sent_by_terminal<E>;
template <class... Es>
inline constexpr bool all_sent_by_terminal<std::variant<Es...>> = (sent_by_terminal<Es> && ...);
}  // namespace host_detail

/// What maya::run checks about a program before it takes the terminal, so a
/// mistake that would drop input silently fails to compile instead.
template <Program P>
consteval void check_program() {
    // jaal already rejects an on() that takes nothing this host sends (an
    // on(const Key&) written for a host whose keys are KeyEvent).
    if constexpr (requires { typename P::Event; }) {
        if constexpr (!std::is_same_v<typename P::Event, jaal::kernel::no_events>)
            static_assert(host_detail::all_sent_by_terminal<typename P::Event>,
                "maya: P::Event lists input the terminal never sends. A terminal "
                "program takes KeyEvent, MouseEvent, PasteEvent, FocusEvent, "
                "ResizeEvent and jaal::sig.");
    }
}

/// One keymap key: a printable ASCII char or a SpecialKey, matched with no
/// modifiers (key_is). consteval, so a key that can never match is a compile
/// error: '\t', '\r', '\x1b', '\x7f' arrive as SpecialKey::Tab, Enter,
/// Escape, Backspace, and other control bytes arrive as Ctrl+letter.
class KeySpec {
  public:
    consteval KeySpec(char c) : special_(false), code_(static_cast<unsigned char>(c)) {  // NOLINT: implicit on purpose
        if (c == '\t' || c == '\r' || c == '\n') throw "keymap: use SpecialKey::Tab / SpecialKey::Enter, not a control char";
        if (c == '\x1b') throw "keymap: use SpecialKey::Escape, not '\\x1b'";
        if (c == '\x7f' || c == '\b') throw "keymap: use SpecialKey::Backspace, not a control char";
        if (static_cast<unsigned char>(c) < 0x20) throw "keymap: control chars arrive as Ctrl+letter; match with ctrl_is()";
        if (static_cast<unsigned char>(c) >= 0x80) throw "keymap: non-ASCII byte; a keymap matches ASCII keys only";
    }
    consteval KeySpec(SpecialKey k) : special_(true), code_(static_cast<std::uint8_t>(k)) {}  // NOLINT

    [[nodiscard]] constexpr bool          special() const noexcept { return special_; }
    [[nodiscard]] constexpr std::uint8_t  code() const noexcept { return code_; }

  private:
    bool         special_;
    std::uint8_t code_;
};

/// A fixed key -> Msg table with O(1) lookup. Build it once, as a static:
///
///   static std::optional<Msg> on(const Model&, const KeyEvent& k) {
///       static const maya::keymap<Msg> keys{{'q', Quit{}}, {SpecialKey::Up, Up{}}};
///       return keys(k);
///   }
///
/// Same matching as key_is: no modifiers. A key listed twice is a bug (the
/// second entry could never fire) and throws std::logic_error.
template <class Msg>
class keymap {
  public:
    /// One row. An aggregate, NOT std::pair: pair's forwarding constructor
    /// would pass the key to KeySpec's consteval constructor as a runtime
    /// parameter. GCC and Clang accept that by making pair's constructor
    /// immediate too (P2564); MSVC rejects it. Aggregate init calls KeySpec
    /// on the literal itself, which every compiler accepts.
    struct entry {
        KeySpec key;
        Msg     msg;
    };

    keymap(std::initializer_list<entry> entries) {
        if (entries.size() >= kNone) throw std::logic_error("maya::keymap: too many entries");
        chars_.fill(kNone);
        special_.fill(kNone);
        msgs_.reserve(entries.size());
        for (const auto& [spec, msg] : entries) {
            std::uint8_t& slot = spec.special() ? special_[spec.code()] : chars_[spec.code()];
            if (slot != kNone) throw std::logic_error("maya::keymap: a key is listed twice");
            slot = static_cast<std::uint8_t>(msgs_.size());
            msgs_.push_back(msg);
        }
    }

    [[nodiscard]] std::optional<Msg> operator()(const KeyEvent& k) const {
        if (!k.mods.none()) return std::nullopt;
        std::uint8_t idx = kNone;
        if (const auto* ck = std::get_if<CharKey>(&k.key)) {
            if (ck->codepoint < 0x80) idx = chars_[ck->codepoint];
        } else {
            idx = special_[static_cast<std::size_t>(std::get<SpecialKey>(k.key))];
        }
        if (idx == kNone) return std::nullopt;
        return msgs_[idx];
    }

  private:
    static constexpr std::uint8_t kNone = 0xFF;
    static constexpr std::size_t  kSpecial = static_cast<std::size_t>(SpecialKey::F12) + 1;

    std::array<std::uint8_t, 0x80>     chars_{};
    std::array<std::uint8_t, kSpecial> special_{};
    std::vector<Msg>                   msgs_;
};

}  // namespace maya
