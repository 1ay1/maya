#include "maya/terminal/input.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace maya {

// ============================================================================
// InputParser public methods
// ============================================================================

auto InputParser::feed(std::string_view bytes) -> std::vector<Event> {
    std::vector<Event> events;

    for (size_t i = 0; i < bytes.size(); ++i) {
        auto ch = static_cast<uint8_t>(bytes[i]);

        switch (state_) {
        case State::Ground:
            if (ch == 0x1b) {
                // Start of escape sequence
                buf_.clear();
                buf_ += static_cast<char>(ch);
                state_ = State::Escape;
                escape_start_ = clock::now();
            } else {
                ground_byte(ch, events);
            }
            break;

        case State::Escape:
            buf_ += static_cast<char>(ch);
            if (ch == '[') {
                state_ = State::Csi;
            } else if (ch == 'O') {
                state_ = State::Ss3;
            } else if (ch == ']') {
                state_ = State::Osc;
            } else {
                // Alt + key
                emit_alt_key(ch, events);
                state_ = State::Ground;
            }
            break;

        case State::Csi:
            buf_ += static_cast<char>(ch);
            if (is_csi_final(ch)) {
                parse_csi(events);
                if (state_ == State::Csi)
                    state_ = State::Ground;
            }
            // Intermediate and parameter bytes: keep accumulating
            break;

        case State::Ss3:
            buf_ += static_cast<char>(ch);
            parse_ss3(ch, events);
            state_ = State::Ground;
            break;

        case State::Osc:
            buf_ += static_cast<char>(ch);
            // OSC is terminated by BEL (0x07) or ST (ESC \)
            if (ch == 0x07) {
                parse_osc(events);
                state_ = State::Ground;
            } else if (buf_.size() >= 2
                       && buf_[buf_.size() - 2] == '\x1b'
                       && ch == '\\') {
                parse_osc(events);
                state_ = State::Ground;
            }
            break;

        case State::Utf8:
            if ((ch & 0xC0) == 0x80) {
                // Valid continuation byte
                utf8_cp_ = (utf8_cp_ << 6) | (ch & 0x3F);
                utf8_raw_ += static_cast<char>(ch);
                if (--utf8_remaining_ == 0) {
                    events.emplace_back(KeyEvent{
                        .key = CharKey{utf8_cp_},
                        .mods = {},
                        .raw_sequence = std::move(utf8_raw_),
                    });
                    state_ = State::Ground;
                }
            } else {
                // Invalid continuation — emit what we had and reprocess
                state_ = State::Ground;
                --i; // reprocess this byte
            }
            break;

        case State::BracketedPaste:
            buf_ += static_cast<char>(ch);
            // Look for the terminator: ESC [201~
            if (buf_.size() >= 6) {
                auto tail = std::string_view(buf_).substr(buf_.size() - 6);
                if (tail == "\x1b[201~") {
                    // Emit paste content (strip the terminator)
                    auto content = buf_.substr(0, buf_.size() - 6);
                    events.emplace_back(PasteEvent{std::move(content)});
                    state_ = State::Ground;
                }
            }
            break;
        }
    }

    return events;
}

auto InputParser::flush_timeout() -> std::vector<Event> {
    std::vector<Event> events;

    if (state_ == State::Escape && !buf_.empty()) {
        auto elapsed = clock::now() - escape_start_;
        if (elapsed >= escape_timeout_) {
            events.emplace_back(KeyEvent{
                .key = SpecialKey::Escape,
                .mods = {},
                .raw_sequence = std::move(buf_),
            });
            buf_.clear();
            state_ = State::Ground;
        }
    }

    return events;
}

bool InputParser::has_pending() const noexcept {
    return state_ != State::Ground;
}

void InputParser::reset() noexcept {
    state_ = State::Ground;
    buf_.clear();
}

// ============================================================================
// InputParser private methods
// ============================================================================

void InputParser::ground_byte(uint8_t ch, std::vector<Event>& events) {
    if (ch == '\r' || ch == '\n') {
        events.emplace_back(KeyEvent{
            .key = SpecialKey::Enter,
            .mods = {},
            .raw_sequence = std::string(1, static_cast<char>(ch)),
        });
    } else if (ch == '\t') {
        events.emplace_back(KeyEvent{
            .key = SpecialKey::Tab,
            .mods = {},
            .raw_sequence = "\t",
        });
    } else if (ch == 0x7f) {
        events.emplace_back(KeyEvent{
            .key = SpecialKey::Backspace,
            .mods = {},
            .raw_sequence = std::string(1, static_cast<char>(ch)),
        });
    } else if (ch == 0x08) {
        events.emplace_back(KeyEvent{
            .key = SpecialKey::Backspace,
            .mods = {.ctrl = true},
            .raw_sequence = std::string(1, static_cast<char>(ch)),
        });
    } else if (ch == 0x1f) {
        events.emplace_back(KeyEvent{
            .key = CharKey{U'/'},
            .mods = {.ctrl = true},
            .raw_sequence = std::string(1, static_cast<char>(ch)),
        });
    } else if (ch < 0x20) {
        // Ctrl + letter (Ctrl-A = 0x01, Ctrl-Z = 0x1a)
        char32_t letter = static_cast<char32_t>(ch + 'a' - 1);
        events.emplace_back(KeyEvent{
            .key = CharKey{letter},
            .mods = {.ctrl = true},
            .raw_sequence = std::string(1, static_cast<char>(ch)),
        });
    } else if (ch < 0x80) {
        // Printable ASCII
        events.emplace_back(KeyEvent{
            .key = CharKey{static_cast<char32_t>(ch)},
            .mods = {},
            .raw_sequence = std::string(1, static_cast<char>(ch)),
        });
    } else if ((ch & 0xE0) == 0xC0) {
        // UTF-8 2-byte lead
        utf8_cp_ = ch & 0x1F;
        utf8_remaining_ = 1;
        utf8_raw_.assign(1, static_cast<char>(ch));
        state_ = State::Utf8;
    } else if ((ch & 0xF0) == 0xE0) {
        // UTF-8 3-byte lead
        utf8_cp_ = ch & 0x0F;
        utf8_remaining_ = 2;
        utf8_raw_.assign(1, static_cast<char>(ch));
        state_ = State::Utf8;
    } else if ((ch & 0xF8) == 0xF0) {
        // UTF-8 4-byte lead
        utf8_cp_ = ch & 0x07;
        utf8_remaining_ = 3;
        utf8_raw_.assign(1, static_cast<char>(ch));
        state_ = State::Utf8;
    } else {
        // Invalid byte, emit as-is
        events.emplace_back(KeyEvent{
            .key = CharKey{static_cast<char32_t>(ch)},
            .mods = {},
            .raw_sequence = std::string(1, static_cast<char>(ch)),
        });
    }
}

void InputParser::emit_alt_key(uint8_t ch, std::vector<Event>& events) {
    KeyEvent ev{
        .key = CharKey{static_cast<char32_t>(ch)},
        .mods = {.alt = true},
        .raw_sequence = std::move(buf_),
    };

    // Remap specific keys
    if (ch == '\r' || ch == '\n') {
        ev.key = SpecialKey::Enter;
    } else if (ch == 0x7f) {
        ev.key = SpecialKey::Backspace;
    } else if (ch < 0x20) {
        ev.key  = CharKey{static_cast<char32_t>(ch + 'a' - 1)};
        ev.mods.ctrl = true;
    }

    events.emplace_back(std::move(ev));
}

void InputParser::parse_csi(std::vector<Event>& events) {
    // Strip ESC [ prefix
    std::string_view seq(buf_);
    seq.remove_prefix(2); // skip \x1b[

    if (seq.empty()) return;

    uint8_t final_byte = static_cast<uint8_t>(seq.back());
    std::string_view params_str = seq.substr(0, seq.size() - 1);

    // Check for bracketed paste start
    if (final_byte == '~') {
        auto params = parse_params(params_str);
        if (!params.empty()) {
            int code = params[0];
            if (code == 200) {
                // Bracketed paste start
                state_ = State::BracketedPaste;
                buf_.clear();
                return;
            }
            // xterm modifyOtherKeys=2: CSI 27 ; <mods> ; <keycode> ~
            // The sentinel `27` (ESC code) marks this as a modified-key
            // report; the real codepoint is in params[2]. Used for keys
            // whose plain encoding lacks modifier info — Shift+Enter,
            // Ctrl+Enter, etc — on terminals that don't speak KKP.
            if (code == 27 && params.size() >= 3) {
                int mod_param = params[1];
                int keycode   = params[2];
                Modifiers mods;
                if (mod_param > 1) mods = Modifiers::from_param(mod_param);
                std::optional<Key> k;
                switch (keycode) {
                    case 9:   k = SpecialKey::Tab;       break;
                    case 13:  k = SpecialKey::Enter;     break;
                    case 27:  k = SpecialKey::Escape;    break;
                    case 127: k = SpecialKey::Backspace; break;
                    default:
                        if (keycode >= 32 && keycode < 0x110000)
                            k = CharKey{static_cast<char32_t>(keycode)};
                        break;
                }
                if (k) {
                    events.emplace_back(KeyEvent{
                        .key = *k,
                        .mods = mods,
                        .raw_sequence = std::move(buf_),
                    });
                }
                return;
            }
            // Tilde-terminated special keys
            handle_tilde_key(code, params.size() > 1 ? params[1] : 1, events);
            return;
        }
    }

    // Focus events: CSI I (focus in) and CSI O (focus out)
    if (final_byte == 'I') {
        events.emplace_back(FocusEvent{.focused = true});
        return;
    }
    if (final_byte == 'O') {
        events.emplace_back(FocusEvent{.focused = false});
        return;
    }

    // SGR mouse: CSI < Pb ; Px ; Py M/m
    if (!params_str.empty() && params_str[0] == '<'
        && (final_byte == 'M' || final_byte == 'm'))
    {
        parse_sgr_mouse(params_str.substr(1), final_byte, events);
        return;
    }

    // Arrow keys, Home, End with optional modifiers
    // CSI [modifier] A/B/C/D/H/F
    if ((final_byte >= 'A' && final_byte <= 'F') || final_byte == 'H') {
        auto params = parse_params(params_str);
        Modifiers mods;
        if (params.size() >= 2 && params[1] > 1) {
            mods = Modifiers::from_param(params[1]);
        }

        std::optional<SpecialKey> sk;
        switch (final_byte) {
            case 'A': sk = SpecialKey::Up;    break;
            case 'B': sk = SpecialKey::Down;  break;
            case 'C': sk = SpecialKey::Right; break;
            case 'D': sk = SpecialKey::Left;  break;
            case 'H': sk = SpecialKey::Home;  break;
            case 'F': sk = SpecialKey::End;   break;
        }

        if (sk) {
            events.emplace_back(KeyEvent{
                .key = *sk,
                .mods = mods,
                .raw_sequence = std::move(buf_),
            });
        }
        return;
    }

    // Shift-Tab: CSI Z
    if (final_byte == 'Z') {
        events.emplace_back(KeyEvent{
            .key = SpecialKey::BackTab,
            .mods = {.shift = true},
            .raw_sequence = std::move(buf_),
        });
        return;
    }

    // CSI u -- Kitty / Unicode keyboard protocol: ESC [ <codepoint> ; <mods> u
    // Encodes any Unicode codepoint with unambiguous modifier flags.
    if (final_byte == 'u') {
        auto params = parse_params(params_str);
        if (!params.empty()) {
            int codepoint = params[0];
            Modifiers mods;
            if (params.size() >= 2 && params[1] > 1) {
                mods = Modifiers::from_param(params[1]);
            }

            std::optional<Key> key;
            switch (codepoint) {
                case 9:   key = SpecialKey::Tab;       break;
                case 13:  key = SpecialKey::Enter;     break;
                case 27:  key = SpecialKey::Escape;    break;
                case 127: key = SpecialKey::Backspace; break;
                // Function keys encoded as codepoints >= 57344 (Kitty extension)
                case 57344: key = SpecialKey::F1;  break;
                case 57345: key = SpecialKey::F2;  break;
                case 57346: key = SpecialKey::F3;  break;
                case 57347: key = SpecialKey::F4;  break;
                case 57348: key = SpecialKey::F5;  break;
                case 57349: key = SpecialKey::F6;  break;
                case 57350: key = SpecialKey::F7;  break;
                case 57351: key = SpecialKey::F8;  break;
                case 57352: key = SpecialKey::F9;  break;
                case 57353: key = SpecialKey::F10; break;
                case 57354: key = SpecialKey::F11; break;
                case 57355: key = SpecialKey::F12; break;
                // Arrow keys / navigation
                case 57356: key = SpecialKey::Up;       break;
                case 57357: key = SpecialKey::Down;     break;
                case 57358: key = SpecialKey::Left;     break;
                case 57359: key = SpecialKey::Right;    break;
                case 57360: key = SpecialKey::Home;     break;
                case 57361: key = SpecialKey::End;      break;
                case 57362: key = SpecialKey::PageUp;   break;
                case 57363: key = SpecialKey::PageDown; break;
                case 57364: key = SpecialKey::Insert;   break;
                case 57365: key = SpecialKey::Delete;   break;
                default:
                    if (codepoint >= 32 && codepoint < 0x110000) {
                        key = CharKey{static_cast<char32_t>(codepoint)};
                    }
                    break;
            }

            if (key) {
                events.emplace_back(KeyEvent{
                    .key = *key,
                    .mods = mods,
                    .raw_sequence = std::move(buf_),
                });
            }
            return;
        }
    }

    // Fallback: emit as unknown key event with raw sequence
    events.emplace_back(KeyEvent{
        .key = CharKey{U'?'},
        .mods = {},
        .raw_sequence = std::move(buf_),
    });
}

void InputParser::handle_tilde_key(int code, int modifier_param, std::vector<Event>& events) {
    Modifiers mods;
    if (modifier_param > 1) {
        mods = Modifiers::from_param(modifier_param);
    }

    std::optional<SpecialKey> sk;
    switch (code) {
        case 1:  sk = SpecialKey::Home;   break;
        case 2:  sk = SpecialKey::Insert;  break;
        case 3:  sk = SpecialKey::Delete;  break;
        case 4:  sk = SpecialKey::End;     break;
        case 5:  sk = SpecialKey::PageUp;  break;
        case 6:  sk = SpecialKey::PageDown; break;
        case 11: sk = SpecialKey::F1;      break;
        case 12: sk = SpecialKey::F2;      break;
        case 13: sk = SpecialKey::F3;      break;
        case 14: sk = SpecialKey::F4;      break;
        case 15: sk = SpecialKey::F5;      break;
        case 17: sk = SpecialKey::F6;      break;
        case 18: sk = SpecialKey::F7;      break;
        case 19: sk = SpecialKey::F8;      break;
        case 20: sk = SpecialKey::F9;      break;
        case 21: sk = SpecialKey::F10;     break;
        case 23: sk = SpecialKey::F11;     break;
        case 24: sk = SpecialKey::F12;     break;
        case 201: return; // Bracketed paste end (handled elsewhere)
        default: return;
    }

    if (sk) {
        events.emplace_back(KeyEvent{
            .key = *sk,
            .mods = mods,
            .raw_sequence = std::move(buf_),
        });
    }
}

void InputParser::parse_sgr_mouse(std::string_view params_str, uint8_t final_byte,
                                   std::vector<Event>& events)
{
    auto params = parse_params(params_str);
    if (params.size() < 3) return;

    int cb = params[0];
    int px = params[1];
    int py = params[2];

    // Decode button and modifiers from cb
    Modifiers mods{
        .ctrl  = (cb & 16) != 0,
        .alt   = (cb & 8)  != 0,
        .shift = (cb & 4)  != 0,
    };

    int button_bits = cb & 0x43; // bits 0-1 and 6
    bool is_motion  = (cb & 32) != 0;

    MouseButton button   = MouseButton::None;
    MouseEventKind kind  = MouseEventKind::Press;

    if (final_byte == 'm') {
        kind = MouseEventKind::Release;
    } else if (is_motion) {
        kind = MouseEventKind::Move;
    }

    // Decode button
    if (button_bits == 0)       button = MouseButton::Left;
    else if (button_bits == 1)  button = MouseButton::Middle;
    else if (button_bits == 2)  button = MouseButton::Right;
    else if (button_bits == 3)  button = MouseButton::None; // release (X10)
    else if (button_bits == 64) button = MouseButton::ScrollUp;
    else if (button_bits == 65) button = MouseButton::ScrollDown;

    events.emplace_back(MouseEvent{
        .button = button,
        .kind   = kind,
        .x      = Columns{px},
        .y      = Rows{py},
        .mods   = mods,
    });
}

void InputParser::parse_ss3(uint8_t ch, std::vector<Event>& events) {
    std::optional<SpecialKey> sk;
    switch (ch) {
        case 'P': sk = SpecialKey::F1;   break;
        case 'Q': sk = SpecialKey::F2;   break;
        case 'R': sk = SpecialKey::F3;   break;
        case 'S': sk = SpecialKey::F4;   break;
        case 'A': sk = SpecialKey::Up;   break;
        case 'B': sk = SpecialKey::Down;  break;
        case 'C': sk = SpecialKey::Right; break;
        case 'D': sk = SpecialKey::Left;  break;
        case 'H': sk = SpecialKey::Home;  break;
        case 'F': sk = SpecialKey::End;   break;
        default:  break;
    }

    if (sk) {
        events.emplace_back(KeyEvent{
            .key = *sk,
            .mods = {},
            .raw_sequence = std::move(buf_),
        });
    }
}

void InputParser::parse_osc([[maybe_unused]] std::vector<Event>& events) {
    // OSC sequences from the terminal (e.g., clipboard responses)
    // are not yet handled. Silently discard.
}

auto InputParser::parse_params(std::string_view s) -> std::vector<int> {
    std::vector<int> result;
    if (s.empty()) return result;

    int current = 0;
    bool has_digit = false;

    for (char c : s) {
        if (c >= '0' && c <= '9') {
            current = current * 10 + (c - '0');
            has_digit = true;
        } else if (c == ';') {
            result.push_back(has_digit ? current : 0);
            current = 0;
            has_digit = false;
        }
        // Skip non-digit, non-semicolon characters (e.g., '?' private mode indicator)
    }

    result.push_back(has_digit ? current : 0);
    return result;
}

} // namespace maya
