#pragma once
// maya::render::diff - Frame differ: direct ANSI output, zero intermediate allocations
//
// Compares two canvases cell-by-cell within the damage region and writes
// a compact ANSI byte stream that transforms the front buffer's terminal
// state into the back buffer's visual state.
//
// Key design: instead of building a vector<RenderOp> and then serializing,
// we write ANSI sequences directly into the caller's string buffer. This
// eliminates every heap allocation that the old RenderOp path incurred —
// one per changed cell for cursor positioning, one per style change, one
// per character batch. On a typical 80x24 frame with 10% changed cells,
// that's ~192 fewer allocations per render cycle.
//
// Performance critical paths (inner loop, per-cell):
//   - Style transitions:  pre-cached SGR string from StylePool — single memcpy,
//                          zero branches, zero Style field comparisons.
//   - UTF-8 encoding:     batch write (one append() per character, not per byte).
//   - Cursor positioning:  built in a stack buffer, one append() per CUP sequence.
//   - Cell comparison:     single 64-bit integer test (packed cell representation).
//   - Row skip:            SIMD bulk_eq (AVX-512: 8 cells/cycle, AVX2: 4, SSE2: 2).

#include <cstdint>
#include <string>

#include "canvas.hpp"
#include "../core/simd.hpp"
#include "../platform/detect.hpp"
#include "../terminal/ansi.hpp"

namespace maya {

// ============================================================================
// UTF-8 encoding — batch write, one append() per character
// ============================================================================

namespace detail {

/// Encode a Unicode code point, appending its UTF-8 bytes to `out`.
/// Uses a stack buffer + single append() instead of per-byte operator+=.
MAYA_FORCEINLINE void encode_utf8(char32_t cp, std::string& out) {
    if (cp < 0x80) [[likely]] {
        out += static_cast<char>(cp);
    } else {
        char buf[4];
        int len;
        if (cp < 0x800) {
            buf[0] = static_cast<char>(0xC0 | (cp >> 6));
            buf[1] = static_cast<char>(0x80 | (cp & 0x3F));
            len = 2;
        } else if (cp < 0x10000) [[likely]] {
            buf[0] = static_cast<char>(0xE0 | (cp >> 12));
            buf[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            buf[2] = static_cast<char>(0x80 | (cp & 0x3F));
            len = 3;
        } else {
            buf[0] = static_cast<char>(0xF0 | (cp >> 18));
            buf[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            buf[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            buf[3] = static_cast<char>(0x80 | (cp & 0x3F));
            len = 4;
        }
        out.append(buf, static_cast<std::size_t>(len));
    }
}

// ============================================================================
// Fast cursor positioning — one append() per CUP sequence
// ============================================================================

MAYA_FORCEINLINE char* write_uint_pos(char* p, unsigned n) noexcept {
    if (n < 10) {
        *p++ = static_cast<char>('0' + n);
    } else if (n < 100) {
        *p++ = static_cast<char>('0' + n / 10);
        *p++ = static_cast<char>('0' + n % 10);
    } else if (n < 1000) {
        *p++ = static_cast<char>('0' + n / 100);
        *p++ = static_cast<char>('0' + (n / 10) % 10);
        *p++ = static_cast<char>('0' + n % 10);
    } else {
        *p++ = static_cast<char>('0' + n / 1000);
        *p++ = static_cast<char>('0' + (n / 100) % 10);
        *p++ = static_cast<char>('0' + (n / 10) % 10);
        *p++ = static_cast<char>('0' + n % 10);
    }
    return p;
}

MAYA_FORCEINLINE void write_cup(std::string& out, int col, int row) {
    char buf[16]; // max: \x1b[9999;9999H = 15 chars
    char* p = buf;
    *p++ = '\x1b'; *p++ = '[';
    p = write_uint_pos(p, static_cast<unsigned>(row));
    *p++ = ';';
    p = write_uint_pos(p, static_cast<unsigned>(col));
    *p++ = 'H';
    out.append(buf, static_cast<std::size_t>(p - buf));
}

} // namespace detail

// ============================================================================
// diff - Compare two canvases; write ANSI update stream directly to `out`
// ============================================================================

void diff(
    const Canvas& old_canvas,
    const Canvas& new_canvas,
    const StylePool& pool,
    std::string& out);

} // namespace maya
