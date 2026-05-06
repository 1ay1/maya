// StreamSink boundary-safety tests.
//
// Contract under test: for any byte sequence S split into chunks of any
// shape (including chunks that fall mid-UTF-8 or mid-CSI), feeding the
// chunks in order into a StreamSink and concatenating the results +
// final flush() yields a byte sequence equal to S.
//
// In addition, the safe prefix returned by every feed() must always end
// on a UTF-8 codepoint boundary and must not split a CSI / OSC sequence.

#include <cassert>
#include <print>
#include <random>
#include <string>
#include <string_view>

#include <maya/text/stream_sink.hpp>

namespace {

using maya::StreamSink;

// ── Property: feed-in-pieces equals feed-whole ──────────────────────────────

void test_chunked_equals_whole_ascii() {
    std::println("--- test_chunked_equals_whole_ascii ---");
    constexpr std::string_view src = "Hello, world! This is plain ASCII text.";
    StreamSink s;
    std::string acc;
    for (char c : src) acc += s.feed(std::string_view{&c, 1});
    acc += s.flush();
    assert(acc == src);
    assert(!s.has_pending());
}

void test_chunked_equals_whole_utf8_dense() {
    std::println("--- test_chunked_equals_whole_utf8_dense ---");
    // Em-dashes, arrows, checkmarks, hexagrams — the kind of multi-byte
    // glyphs that show up in real LLM markdown output.
    constexpr std::string_view src =
        "Patch \xe2\x9c\x93 — found it. Light \xe2\x86\x92 dark, \xe6\x9c\x88 \xe2\x9c\xa8";
    StreamSink s;
    std::string acc;
    // Feed one byte at a time — the worst case.
    for (std::size_t i = 0; i < src.size(); ++i) {
        acc += s.feed(src.substr(i, 1));
    }
    acc += s.flush();
    assert(acc == src);
}

void test_random_chunks_equal_whole() {
    std::println("--- test_random_chunks_equal_whole ---");
    // Mix of ASCII, multi-byte UTF-8, and ANSI escape sequences.
    const std::string src =
        std::string("\x1b[1;32mGreen\x1b[0m text with \xe2\x80\x94 dashes ") +
        "and a CSI \x1b[2J reset, plus emoji \xf0\x9f\x9a\x80 rocket.";

    std::mt19937 rng(0xDEADBEEF);
    std::uniform_int_distribution<int> chunk_size(1, 5);

    for (int trial = 0; trial < 64; ++trial) {
        StreamSink s;
        std::string acc;
        std::size_t i = 0;
        while (i < src.size()) {
            std::size_t n = std::min<std::size_t>(
                std::size_t(chunk_size(rng)), src.size() - i);
            acc += s.feed(std::string_view{src.data() + i, n});
            i += n;
        }
        acc += s.flush();
        assert(acc == src);
    }
}

// ── Property: feed() output never ends mid-codepoint ────────────────────────

bool ends_on_codepoint_boundary(std::string_view s) {
    if (s.empty()) return true;
    // The last byte must be either ASCII (<0x80) or a complete sequence.
    // Walk back from the end: if the last byte is a continuation byte
    // (10xxxxxx), it must be the trailing continuation of a complete
    // multi-byte sequence whose lead byte is properly placed.
    auto is_cont = [](unsigned char b) { return (b & 0xC0) == 0x80; };
    auto lead_len = [](unsigned char b) -> int {
        if (b < 0x80)             return 1;
        if ((b & 0xE0) == 0xC0)   return 2;
        if ((b & 0xF0) == 0xE0)   return 3;
        if ((b & 0xF8) == 0xF0)   return 4;
        return 0;  // invalid lead — treat as opaque
    };

    // Find the last lead byte.
    std::size_t i = s.size();
    while (i > 0 && is_cont(static_cast<unsigned char>(s[i - 1]))) --i;
    if (i == 0) return false;  // continuation byte with no lead — invalid
    --i;
    int len = lead_len(static_cast<unsigned char>(s[i]));
    if (len == 0) {
        // Invalid lead — we treat it as a 1-byte opaque emit, so the
        // boundary is fine if it's the actual last byte.
        return i + 1 == s.size();
    }
    return i + std::size_t(len) == s.size();
}

void test_feed_never_splits_codepoint() {
    std::println("--- test_feed_never_splits_codepoint ---");
    constexpr std::string_view src =
        "ASCII \xe2\x80\x94 mixed \xe2\x9c\x93 \xf0\x9f\x9a\x80 \xe6\x9c\x88 end";

    std::mt19937 rng(0xBADC0FFEu);
    std::uniform_int_distribution<int> chunk_size(1, 4);

    for (int trial = 0; trial < 256; ++trial) {
        StreamSink s;
        std::size_t i = 0;
        while (i < src.size()) {
            std::size_t n = std::min<std::size_t>(
                std::size_t(chunk_size(rng)), src.size() - i);
            std::string out = s.feed(src.substr(i, n));
            assert(ends_on_codepoint_boundary(out));
            i += n;
        }
        // Final flush may emit a partial codepoint at end-of-stream; that
        // is by design (better to surface than to swallow). Don't assert.
    }
}

// ── Property: CSI sequences are never split ────────────────────────────────

bool contains_split_csi(std::string_view s) {
    // A "split CSI" means we ended a feed() output with bytes that
    // started a CSI but didn't include its final byte (0x40-0x7E).
    bool in_csi = false;
    for (unsigned char b : s) {
        if (!in_csi) {
            // Check for ESC [
            // (we allow ESC alone — it's only "in-CSI" after seeing '[')
            // For a quick property check we just look for ESC [ pairs.
        }
    }
    // Simpler: walk and verify every ESC [ has a final byte.
    for (std::size_t i = 0; i + 1 < s.size(); ++i) {
        if (static_cast<unsigned char>(s[i]) == 0x1B && s[i + 1] == '[') {
            std::size_t j = i + 2;
            while (j < s.size()) {
                unsigned char b = static_cast<unsigned char>(s[j]);
                if (b >= 0x40 && b <= 0x7E) break;
                ++j;
            }
            if (j == s.size()) return true;  // unterminated CSI
        }
    }
    return false;
}

void test_feed_never_splits_csi() {
    std::println("--- test_feed_never_splits_csi ---");
    constexpr std::string_view src =
        "before \x1b[1;31mred text\x1b[0m middle "
        "\x1b[?2026hsync\x1b[?2026l after";

    std::mt19937 rng(0x12345678);
    std::uniform_int_distribution<int> chunk_size(1, 3);

    for (int trial = 0; trial < 128; ++trial) {
        StreamSink s;
        std::size_t i = 0;
        while (i < src.size()) {
            std::size_t n = std::min<std::size_t>(
                std::size_t(chunk_size(rng)), src.size() - i);
            std::string out = s.feed(src.substr(i, n));
            assert(!contains_split_csi(out));
            i += n;
        }
    }
}

// ── Reset clears state ──────────────────────────────────────────────────────

void test_reset_clears_pending() {
    std::println("--- test_reset_clears_pending ---");
    StreamSink s;
    // Feed a half-codepoint.
    auto out = s.feed(std::string_view("\xe2\x80", 2));
    assert(out.empty());
    assert(s.has_pending());
    s.reset();
    assert(!s.has_pending());
    // Feeding a complete codepoint after reset emits cleanly.
    out = s.feed(std::string_view("\xe2\x80\x94", 3));
    auto expected = std::string_view("\xe2\x80\x94", 3);
    assert(out == expected);
}

} // namespace

int main() {
    test_chunked_equals_whole_ascii();
    test_chunked_equals_whole_utf8_dense();
    test_random_chunks_equal_whole();
    test_feed_never_splits_codepoint();
    test_feed_never_splits_csi();
    test_reset_clears_pending();
    std::println("All StreamSink tests passed.");
    return 0;
}
