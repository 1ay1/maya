// fuzz_input.cpp — libFuzzer harness for the terminal input parser.
//
// InputParser::feed() consumes raw bytes from a tty: CSI/SS3/OSC escape
// sequences, SGR mouse, bracketed paste, UTF-8, and the OSC-52 base64
// clipboard reply (which decode_base64 runs on attacker-influenced bytes
// when an app is on the far end of an SSH pty). This is maya's single most
// exposed parse surface — fuzz it hard.
//
//   clang++ -fsanitize=fuzzer,address,undefined fuzz_input.cpp -lmaya ...

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <maya/terminal/input.hpp>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Split the input so we also exercise the parser's PARTIAL-sequence
    // buffering: the first byte picks a chunk size, the rest is fed in two
    // pieces across two feed() calls on the SAME parser instance. A
    // sequence that straddles the boundary must still parse (or be safely
    // held) — that's where state-machine bugs hide.
    maya::InputParser parser;

    if (size == 0) {
        (void)parser.feed({});
        (void)parser.flush_timeout();
        return 0;
    }

    std::string_view all{reinterpret_cast<const char*>(data), size};

    const std::size_t split = (static_cast<std::size_t>(data[0]) * all.size()) / 256;
    (void)parser.feed(all.substr(0, split));
    (void)parser.flush_timeout();
    (void)parser.feed(all.substr(split));
    (void)parser.flush_timeout();

    // Reset must always return to a clean ground state.
    parser.reset();
    return 0;
}
