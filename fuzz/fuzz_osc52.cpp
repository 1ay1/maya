// fuzz_osc52.cpp — libFuzzer harness focused on the OSC-52 base64 path.
//
// The OSC-52 clipboard reply carries base64 that decode_base64() turns into
// raw bytes (possibly a binary image) and surfaces as a PasteEvent. That
// payload originates on the user's local terminal but crosses an SSH pty,
// so it is attacker-influenceable. decode_base64 is private, so we drive it
// through feed() by wrapping the fuzz bytes in a well-formed OSC-52 frame.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <maya/terminal/input.hpp>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    maya::InputParser parser;

    // ESC ] 52 ; c ; <fuzz bytes> ST   — the exact shape of a clipboard read
    // response. The body is whatever the fuzzer produced (valid b64, invalid
    // symbols, embedded '=', whitespace, partial codepoints, etc).
    std::string frame = "\x1b]52;c;";
    frame.append(reinterpret_cast<const char*>(data), size);
    frame += "\x1b\\";  // ST terminator

    (void)parser.feed(frame);

    // Also feed it BEL-terminated to hit the other terminator branch.
    maya::InputParser parser2;
    std::string frame2 = "\x1b]52;p;";
    frame2.append(reinterpret_cast<const char*>(data), size);
    frame2 += "\x07";   // BEL terminator
    (void)parser2.feed(frame2);

    // OSC 5522 (kitty clipboard protocol) — the multi-packet read reply.
    // Drive the metadata/payload splitter and the DATA accumulator with
    // fuzz bytes in both positions, inside a live transfer (status=OK
    // first) so the accumulation paths are reachable.
    maya::InputParser parser3;
    std::string s;
    s = "\x1b]5522;type=read:status=OK\x1b\\";
    s += "\x1b]5522;";
    s.append(reinterpret_cast<const char*>(data), size);
    s += "\x1b\\";
    s += "\x1b]5522;type=read:status=DATA:mime=";
    s.append(reinterpret_cast<const char*>(data), size);
    s += ";";
    s.append(reinterpret_cast<const char*>(data), size);
    s += "\x1b\\";
    s += "\x1b]5522;type=read:status=DONE\x1b\\";
    (void)parser3.feed(s);

    return 0;
}
