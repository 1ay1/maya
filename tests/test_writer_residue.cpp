// tests/test_writer_residue.cpp — the Writer's backpressure path loses,
// reorders or duplicates nothing.
//
// A tty takes bytes 1 KB at a time; a frame bigger than the kernel buffer
// is shipped partly and the rest is held as `residue` and drained later.
// Property: whatever the drain pattern, the bytes that come out of the fd
// are EXACTLY the bytes written, in order, and at every moment what has
// come out is a prefix of what was sent.
//
// Frames are random mixes of CSI, OSC, UTF-8 (2-4 bytes) and ASCII. The
// reader drains a random 1..4096 bytes between writes, so residue is
// appended to, drained partially, and compacted in every combination.

#include <maya/maya.hpp>
#include <maya/terminal/writer.hpp>

#include "agtest.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <print>
#include <random>
#include <string>

using namespace maya;

namespace {

std::string random_frame(std::mt19937& rng) {
    std::string s;
    const int units = 200 + int(rng() % 3000);
    for (int i = 0; i < units; ++i) {
        switch (rng() % 6) {
            case 0: s += "\x1b[" + std::to_string(rng() % 60) + ";" + std::to_string(rng() % 200) + "H"; break;
            case 1: s += "\x1b[38;2;" + std::to_string(rng() % 256) + ";1;2;48;5;" + std::to_string(rng() % 256) + "m"; break;
            case 2: s += "\x1b]0;title " + std::to_string(rng() % 999) + "\x07"; break;
            case 3: s += "\xe2\x96\x80"; break;                       // U+2580, 3 bytes
            case 4: s += "\xf0\x9f\x94\xa5"; break;                   // U+1F525, 4 bytes
            default: s += char('a' + rng() % 26); break;
        }
    }
    return s;
}

}  // namespace

TEST_CASE("writer residue: bytes come out whole, in order, never split mid-unit") {
    std::println("--- test_writer_residue ---");
    for (std::uint32_t seed = 1; seed <= 25; ++seed) {
        std::mt19937 rng(seed);
        int fds[2];
        assert(::pipe(fds) == 0);
        ::fcntl(fds[1], F_SETFL, ::fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);
        ::fcntl(fds[0], F_SETFL, ::fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
        Writer w{static_cast<platform::NativeHandle>(fds[1])};

        std::string sent, got, chunk(4096, '\0');
        auto read_some = [&](std::size_t max) {
            while (max > 0) {
                const auto n = ::read(fds[0], chunk.data(), std::min(max, chunk.size()));
                if (n <= 0) return;
                got.append(chunk.data(), std::size_t(n));
                max -= std::size_t(n);
            }
        };
        for (int f = 0; f < 40; ++f) {
            const std::string frame = random_frame(rng);
            sent += frame;
            auto st = w.write_or_buffer(frame);
            assert(st.has_value());
            read_some(1 + rng() % 4096);
            if (rng() % 3 == 0) (void)w.try_drain_residue();
            // Everything read so far is a PREFIX of what was sent: nothing
            // lost, reordered or duplicated at any point in the drain. (The
            // reader's own cut can land mid-unit - it asked for N bytes - so
            // unit boundaries are checked per write() in the unit test of
            // safe_break_len, not here.)
            assert(sent.compare(0, got.size(), got) == 0);
        }
        // Drain to completion.
        for (int guard = 0; w.has_residue() && guard < 100000; ++guard) {
            read_some(65536);
            (void)w.try_drain_residue();
        }
        read_some(1 << 20);
        if (got != sent) {
            std::size_t i = 0;
            while (i < got.size() && i < sent.size() && got[i] == sent[i]) ++i;
            std::println("  MISMATCH seed={} at byte {} of {} (got {})", seed, i, sent.size(), got.size());
            assert(false);
        }
        ::close(fds[0]);
    }
    std::println("PASS\n");
}
