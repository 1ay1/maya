// fuzz_safe_break.cpp — libFuzzer harness for Writer::safe_break_len().
//
// safe_break_len() finds the longest prefix of a byte buffer that does NOT
// end inside a CSI / OSC / DCS / UTF-8 sequence, so the non-blocking writer
// can ship a partial frame to a backed-up tty without splitting an escape
// (which corrupts scrollback). It walks arbitrary bytes — feed it garbage
// and assert it never returns an out-of-range or sequence-splitting answer.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <maya/terminal/writer.hpp>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string_view sv{reinterpret_cast<const char*>(data), size};
    const std::size_t n = maya::detail::safe_break_len(sv);

    // Invariant 1: the safe break is always within bounds.
    if (n > size) __builtin_trap();

    // Invariant 2: idempotence — re-running on the safe prefix returns the
    // whole prefix (a safe-ending prefix has no further unsafe tail).
    if (n > 0) {
        const std::size_t m = maya::detail::safe_break_len(sv.substr(0, n));
        if (m != n) __builtin_trap();
    }

    return 0;
}
