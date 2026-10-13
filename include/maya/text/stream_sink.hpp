#pragma once
// maya::StreamSink — boundary-safe input adapter for streaming bytes.
//
// Streaming consumers (StreamingMarkdown, log viewers, anything that
// receives bytes incrementally from an SSE / WebSocket / subprocess pipe)
// must never see a partial multi-byte UTF-8 codepoint or a half-written
// ANSI escape sequence.  When that happens, downstream layers cannot
// classify cell widths reliably — the unicode_width tables don't know
// what to do with a lone `\xe2` lead byte, the renderer guesses 1 cell,
// the next frame's complete codepoint occupies 2 cells, and the cells
// after it shift sideways → "ghosting" and "row overlap".
//
// StreamSink absorbs that risk at the boundary.  Callers feed bytes in
// arbitrary chunks; StreamSink emits only the safely-completable prefix
// and holds anything mid-sequence in an internal carry buffer until its
// continuation arrives.  The output of feed() is guaranteed to:
//
//   1. End on a UTF-8 codepoint boundary.
//   2. Not split a CSI / OSC / DCS escape sequence.
//
// The framework's streaming widgets route every incoming chunk through a
// private StreamSink before handing the bytes to the parser / cell
// model, so widget users get this for free regardless of how their
// upstream chunks the stream.
//
// Usage (typical, inside a streaming widget):
//
//   class MyWidget {
//       maya::StreamSink sink_;
//       std::string      content_;
//   public:
//       void feed(std::string_view bytes) {
//           content_ += sink_.feed(bytes);   // append safely-completed prefix
//           // ... re-render / re-parse content_ ...
//       }
//       void finish() {
//           content_ += sink_.flush();        // drain any held tail at EOF
//       }
//   };
//
// Thread-safety: not safe for concurrent feed().  One owner per sink.

#include <cstdint>
#include <string>
#include <string_view>

namespace maya {

class StreamSink {
public:
    StreamSink() = default;

    // Feed bytes; return the safely-completable prefix.  Bytes that would
    // split a multi-byte UTF-8 codepoint or an in-progress escape
    // sequence are held in the carry buffer and re-emitted on the next
    // feed/flush once their continuation arrives.
    //
    // Cost: O(bytes.size()).  No allocations beyond the returned string
    // and (rarely) carry buffer growth on multi-byte sequence boundaries.
    [[nodiscard]] std::string feed(std::string_view bytes);

    // Drain the carry buffer.  Returns whatever's pending without waiting
    // for completion.  Use only at end-of-stream — the returned bytes
    // may include a half-decoded codepoint or an unterminated escape,
    // so downstream needs to be tolerant of those (e.g., display them
    // as the U+FFFD replacement character).
    [[nodiscard]] std::string flush();

    // True if any bytes are currently held in the carry buffer.
    [[nodiscard]] bool has_pending() const noexcept { return !carry_.empty(); }

    // Reset state for a new stream.  Discards any pending bytes.
    void reset() noexcept { carry_.clear(); }

private:
    std::string carry_;   // bytes held back from the last feed()
};

// ── Inline implementation ─────────────────────────────────────────────────
// Header-only because the class is tiny and value-typed; no link unit needed.

namespace detail {

// UTF-8 sequence length from a leading byte. 1 for ASCII / invalid (we
// emit invalid bytes as opaque single bytes — the renderer will display
// them as U+FFFD in-place rather than blocking the stream).
[[nodiscard]] constexpr int utf8_lead_len(unsigned char b) noexcept {
    if (b < 0x80)             return 1;
    if ((b & 0xE0) == 0xC0)   return 2;
    if ((b & 0xF0) == 0xE0)   return 3;
    if ((b & 0xF8) == 0xF0)   return 4;
    return 1;   // continuation byte standing alone, or 0xF8+ — emit as-is
}

[[nodiscard]] constexpr bool utf8_is_continuation(unsigned char b) noexcept {
    return (b & 0xC0) == 0x80;
}

} // namespace detail

inline std::string StreamSink::feed(std::string_view bytes) {
    // Combine carry + new bytes into one walkable buffer.  In the common
    // case (no carry, ASCII input) this is one append + one move; the
    // codepoint scan finds no incomplete tail and the carry stays empty.
    std::string buf = std::move(carry_);
    carry_.clear();
    buf.append(bytes.data(), bytes.size());

    // Scan forward, advancing one unit (codepoint or escape sequence)
    // at a time.  Track three states:
    //   plain  — outside any escape sequence
    //   esc    — saw ESC (0x1B), waiting for the next byte to disambiguate
    //   csi    — inside ESC [ … final-byte (0x40-0x7E)
    //   osc    — inside ESC ] … BEL (0x07) or ST (ESC \)
    // For our purposes (preventing mid-sequence splits), tracking the
    // sequence shape is enough; we don't interpret the contents.

    enum class State : std::uint8_t { Plain, Esc, Csi, Osc };
    State st = State::Plain;
    std::size_t safe_end = 0;   // byte offset up to which it's safe to emit

    std::size_t i = 0;
    while (i < buf.size()) {
        unsigned char b = static_cast<unsigned char>(buf[i]);

        switch (st) {
            case State::Plain: {
                if (b == 0x1B) {
                    st = State::Esc;
                    ++i;
                    continue;
                }
                // UTF-8 codepoint advance.
                int len = detail::utf8_lead_len(b);
                // Skip stray continuation bytes (malformed input) as
                // single-byte units rather than blocking the stream.
                if (detail::utf8_is_continuation(b)) {
                    ++i;
                    safe_end = i;
                    continue;
                }
                if (i + std::size_t(len) > buf.size()) {
                    // Incomplete codepoint — hold the rest.
                    goto done_scan;
                }
                i += std::size_t(len);
                safe_end = i;
                continue;
            }

            case State::Esc: {
                // Disambiguate the second byte of the escape sequence.
                if (b == '[')      { st = State::Csi; ++i; continue; }
                if (b == ']')      { st = State::Osc; ++i; continue; }
                // Single-byte ESC sequence (ESC + char).  We only know
                // we've completed once we see this byte.
                ++i;
                safe_end = i;
                st = State::Plain;
                continue;
            }

            case State::Csi: {
                // CSI: parameters in 0x30-0x3F, intermediates 0x20-0x2F,
                // final byte in 0x40-0x7E ends the sequence.
                ++i;
                if (b >= 0x40 && b <= 0x7E) {
                    safe_end = i;
                    st = State::Plain;
                }
                continue;
            }

            case State::Osc: {
                // OSC: ends on BEL (0x07) or ST (ESC \).  We treat ESC
                // here as "may begin ST" but for simplicity require the
                // backslash to actually complete it.
                if (b == 0x07) {
                    ++i;
                    safe_end = i;
                    st = State::Plain;
                    continue;
                }
                if (b == 0x1B) {
                    // Possible ST = ESC \ — peek at the next byte.
                    if (i + 1 < buf.size() &&
                        static_cast<unsigned char>(buf[i + 1]) == '\\')
                    {
                        i += 2;
                        safe_end = i;
                        st = State::Plain;
                        continue;
                    }
                    // No follower yet — hold.
                    goto done_scan;
                }
                ++i;
                continue;
            }
        }
    }
done_scan:

    std::string out;
    out.assign(buf, 0, safe_end);
    if (safe_end < buf.size()) {
        carry_.assign(buf, safe_end, buf.size() - safe_end);
    }
    return out;
}

inline std::string StreamSink::flush() {
    std::string out = std::move(carry_);
    carry_.clear();
    return out;
}

} // namespace maya
