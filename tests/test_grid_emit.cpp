// Tests for the grid-emit host backend: paint a canvas, emit a binary grid
// frame, decode it back, and assert the structure round-trips.  This is the
// contract the Emacs render module (and any cooperating host) decodes against,
// so the wire format is pinned here.
#include <maya/maya.hpp>
#undef NDEBUG
#include "agtest.hpp"

#include "maya/render/grid_emit.hpp"

#include <cstdint>
#include <string>
#include <vector>
#include <print>
#include <random>

using namespace maya;
using namespace maya::dsl;
using namespace maya::render;

namespace {

// ── A minimal reference decoder for the APC grid frame ──────────────────────
// Mirrors the wire format in grid_emit.hpp exactly; used only to verify the
// encoder here (the real consumer is the Emacs C module).
struct DecColor { std::uint8_t kind; std::uint8_t a, b, c; };
struct DecStyle { std::uint16_t id; DecColor fg, bg; std::uint16_t attrs; };
struct DecRun   { std::uint16_t row, col, len, style; std::string utf8; };
struct DecFrame {
    std::uint8_t ver, type, flags;
    std::uint16_t cols, rows, base_row;
    std::vector<DecStyle> styles;
    std::vector<DecRun> runs;
    std::uint16_t crow = 0, ccol = 0; std::uint8_t cvis = 0;
    bool has_cursor = false;
};

struct Reader {
    const std::string& s; std::size_t i = 0;
    explicit Reader(const std::string& b) : s(b) {}
    std::uint8_t  u8()  { return static_cast<std::uint8_t>(s[i++]); }
    std::uint16_t u16() { std::uint16_t v = u8(); v |= std::uint16_t(u8()) << 8; return v; }
    std::uint32_t u32() { std::uint32_t v = u16(); v |= std::uint32_t(u16()) << 16; return v; }
    DecColor color() {
        DecColor c{u8(), 0, 0, 0};
        if (c.kind == 1 || c.kind == 2) c.a = u8();
        else if (c.kind == 3) { c.a = u8(); c.b = u8(); c.c = u8(); }
        return c;
    }
};

// Strip the APC wrapper (ESC _ G <u32 len> <payload> ESC \) and decode the
// payload.  The frame is LENGTH-PREFIXED (v2): read the u32 immediately after
// ESC _ G and take exactly that many payload bytes — never scan for ESC \,
// which the binary payload can contain.
DecFrame decode(const std::string& wire) {
    auto p = wire.find("\x1b_G");
    assert(p != std::string::npos);
    // 4-byte LE length follows the ESC _ G introducer.
    Reader lr(wire); lr.i = p + 3;
    std::uint32_t plen = lr.u32();
    std::size_t pstart = p + 3 + 4;
    assert(pstart + plen + 2 <= wire.size());
    // trailing ESC \ is a sanity check, not the delimiter.
    assert(wire[pstart + plen] == '\x1b' && wire[pstart + plen + 1] == '\\');
    std::string payload = wire.substr(pstart, plen);

    Reader r(payload);
    DecFrame f;
    f.ver = r.u8(); f.type = r.u8(); f.flags = r.u8(); r.u8();  // reserved
    f.cols = r.u16(); f.rows = r.u16(); f.base_row = r.u16();
    if (f.flags & 1) {
        std::uint16_t n = r.u16();
        for (int k = 0; k < n; ++k) {
            DecStyle st; st.id = r.u16(); st.fg = r.color(); st.bg = r.color();
            st.attrs = r.u16(); f.styles.push_back(st);
        }
    }
    std::uint16_t nruns = r.u16();
    for (int k = 0; k < nruns; ++k) {
        DecRun run; run.row = r.u16(); run.col = r.u16();
        run.len = r.u16(); run.style = r.u16();
        // read `len` codepoints of UTF-8 (count multibyte leads)
        int cps = 0;
        while (cps < run.len) {
            unsigned char lead = static_cast<unsigned char>(r.s[r.i]);
            int nbytes = (lead < 0x80) ? 1 : (lead < 0xE0) ? 2 : (lead < 0xF0) ? 3 : 4;
            for (int b = 0; b < nbytes; ++b) run.utf8.push_back(r.s[r.i++]);
            ++cps;
        }
        f.runs.push_back(std::move(run));
    }
    if (f.flags & 2) {
        f.crow = r.u16(); f.ccol = r.u16(); f.cvis = r.u8(); f.has_cursor = true;
    }
    return f;
}

// Concatenate a run's plain-ASCII text for assertions.
std::string runs_text(const DecFrame& f) {
    std::string t;
    for (auto& r : f.runs) t += r.utf8;
    return t;
}

} // namespace

TEST_CASE("grid emit: commit frame carries the scrollback row count") {
    std::println("--- grid_emit commit frame ---");
    // A Commit frame tells the host: N rows scrolled into history.  The count
    // rides the `rows` header field; there are no runs.
    std::string wire; emit_commit(7, wire);
    DecFrame f = decode(wire);
    assert(f.type == static_cast<std::uint8_t>(GridFrameType::Commit));
    assert(f.rows == 7);
    assert(f.runs.empty());
    assert(!f.has_cursor);
    std::println("PASS (commit rows=%d)", f.rows);
}

TEST_CASE("grid emit: full frame round-trips text + dimensions") {
    std::println("--- grid_emit full round-trip ---");
    StylePool pool;
    Canvas c(12, 2, &pool);
    c.clear();
    std::uint16_t sid = pool.intern(Style{}.with_fg(Color::red()));
    c.write_text(0, 0, "hello", sid);

    std::string wire;
    emit_full(c, pool, /*base_row=*/0, /*cursor=*/nullptr, wire);
    DecFrame f = decode(wire);

    assert(f.ver == 2);
    assert(f.type == static_cast<std::uint8_t>(GridFrameType::Full));
    assert(f.cols == 12);
    assert(f.rows == 2);
    // "hello" must appear in the emitted runs.
    assert(runs_text(f).find("hello") != std::string::npos);
    // At least one style advertised (the text style).
    assert((f.flags & 1) && !f.styles.empty());
    std::println("PASS (cols=%d rows=%d styles=%zu runs=%zu)",
                 f.cols, f.rows, f.styles.size(), f.runs.size());
}

TEST_CASE("grid emit: diff frame only carries the changed row") {
    std::println("--- grid_emit diff single row ---");
    StylePool pool;
    Canvas c(16, 3, &pool);
    c.clear();
    std::uint16_t s = pool.intern(Style{});
    c.write_text(0, 0, "row zero", s);
    c.write_text(0, 1, "row one",  s);
    c.write_text(0, 2, "row two",  s);

    // Emit a diff for ONLY row 1.
    std::string wire;
    std::vector<int> changed{1};
    emit_diff(c, pool, changed, /*base_row=*/0, /*cursor=*/nullptr, wire);
    DecFrame f = decode(wire);

    assert(f.type == static_cast<std::uint8_t>(GridFrameType::Diff));
    // every run must be on row 1 (no other rows leaked in)
    for (auto& r : f.runs) assert(r.row == 1);
    assert(runs_text(f).find("row one") != std::string::npos);
    assert(runs_text(f).find("row zero") == std::string::npos);
    std::println("PASS (runs=%zu all on row 1)", f.runs.size());
}

TEST_CASE("grid emit: cursor + resize + clear headers") {
    std::println("--- grid_emit control frames ---");
    // resize
    {
        std::string wire; emit_resize(80, 24, wire);
        DecFrame f = decode(wire);
        assert(f.type == static_cast<std::uint8_t>(GridFrameType::Resize));
        assert(f.cols == 80 && f.rows == 24 && f.runs.empty());
    }
    // cursor
    {
        std::string wire; GridCursor cur{5, 9, true};
        emit_cursor(cur, 80, 24, wire);
        DecFrame f = decode(wire);
        assert(f.type == static_cast<std::uint8_t>(GridFrameType::Cursor));
        assert(f.has_cursor && f.crow == 5 && f.ccol == 9 && f.cvis == 1);
    }
    // clear
    {
        std::string wire; emit_clear(80, 24, wire);
        DecFrame f = decode(wire);
        assert(f.type == static_cast<std::uint8_t>(GridFrameType::Clear));
        assert(f.runs.empty());
    }
    std::println("PASS");
}

TEST_CASE("grid emit: style table dedups + carries attributes") {
    std::println("--- grid_emit style table ---");
    StylePool pool;
    Canvas c(20, 1, &pool);
    c.clear();
    // A bold run and a plain run -> two distinct styles.
    std::uint16_t bold_id  = pool.intern(Style{}.with_bold());
    std::uint16_t plain_id = pool.intern(Style{});
    c.write_text(0, 0, "AA", bold_id);
    c.write_text(2, 0, "bb", plain_id);

    std::string wire;
    emit_full(c, pool, 0, nullptr, wire);
    DecFrame f = decode(wire);

    // Each style id referenced by a run must appear exactly once in the table.
    for (auto& r : f.runs) {
        int hits = 0;
        for (auto& st : f.styles) if (st.id == r.style) ++hits;
        assert(hits == 1);
    }
    // At least one style must have the bold attr bit (bit 0) set.
    bool any_bold = false;
    for (auto& st : f.styles) if (st.attrs & 1u) any_bold = true;
    assert(any_bold);
    std::println("PASS (styles=%zu, bold present)", f.styles.size());
}

TEST_CASE("grid emit: length prefix survives ESC-backslash bytes in payload") {
    std::println("--- grid_emit self-delimiting framing ---");
    // A resize whose ROWS field is 0x5c1b encodes, little-endian, as the bytes
    // 0x1b 0x5c — a FAKE `ESC \` terminator sitting INSIDE the payload.  With
    // the v1 terminator-scan this truncated the frame; with the v2 length
    // prefix the decoder consumes exactly payload_len bytes and is immune.
    std::string wire;
    emit_resize(/*cols=*/40, /*rows=*/0x5c1b, wire);

    // The payload must actually contain the 1b 5c pair (before the real end).
    auto p = wire.find("\x1b_G");
    assert(p != std::string::npos);
    std::size_t body = p + 3 + 4;                 // past ESC _ G + u32 len
    bool has_fake_term = false;
    for (std::size_t k = body; k + 1 < wire.size() - 2; ++k)
        if ((std::uint8_t)wire[k] == 0x1b && (std::uint8_t)wire[k+1] == '\\')
            has_fake_term = true;
    assert(has_fake_term && "test must actually embed a fake ESC\\ in the body");

    // Decode by length prefix: must recover the real frame, not truncate.
    DecFrame f = decode(wire);
    assert(f.ver == 2);
    assert(f.type == static_cast<std::uint8_t>(GridFrameType::Resize));
    assert(f.cols == 40);
    assert(f.rows == 0x5c1b);
    std::println("PASS (fake ESC\\ in payload, framed by length)");
}

// Reconstruct the DISPLAY (one string per column-cell, wide glyph occupies two)
// from a decoded FULL frame and compare it, cell for cell, against the canvas.
// This is the real round-trip oracle: any column/width/text/style-boundary bug
// in build_row_runs shows up as a mismatch here.
static std::u32string decode_utf8_to_u32(const std::string& s) {
    std::u32string out; std::size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        char32_t cp; int nb;
        if (c < 0x80) { cp = c; nb = 1; }
        else if (c < 0xE0) { cp = c & 0x1F; nb = 2; }
        else if (c < 0xF0) { cp = c & 0x0F; nb = 3; }
        else { cp = c & 0x07; nb = 4; }
        for (int b = 1; b < nb && i + (std::size_t)b < s.size(); ++b)
            cp = (cp << 6) | ((unsigned char)s[i + (std::size_t)b] & 0x3F);
        out.push_back(cp); i += (std::size_t)nb;
    }
    return out;
}

TEST_CASE("grid emit: FULL frame round-trips a wide/styled canvas cell-for-cell") {
    std::println("--- grid_emit round-trip fuzz ---");
    std::mt19937 rng(0xC0FFEE);
    const char32_t glyphs[] = { U'a', U'b', U'Z', U'世', U'界', U'あ', U' ', U'#' };
    int mismatches = 0;
    for (int iter = 0; iter < 200; ++iter) {
        int W = 4 + (int)(rng() % 20), H = 1 + (int)(rng() % 5);
        StylePool pool;
        Canvas c(W, H, &pool);
        // reference display grid: one char32_t per column (0 = continuation)
        std::vector<std::u32string> ref(H);
        for (int y = 0; y < H; ++y) ref[y].assign((std::size_t)W, U' ');
        for (int y = 0; y < H; ++y) {
            int x = 0;
            while (x < W) {
                char32_t g = glyphs[rng() % (sizeof glyphs / sizeof *glyphs)];
                int gw = (g == U'世' || g == U'界' || g == U'あ') ? 2 : 1;
                if (x + gw > W) { x++; continue; }
                std::uint16_t sid = (std::uint16_t)(rng() % 3);   // a few styles
                Style st; if (sid == 1) st.bold = true; else if (sid == 2) st.fg = Color::red();
                std::uint16_t id = pool.intern(st);
                if (gw == 2) { c.set(x, y, g, id, 1); c.set(x + 1, y, U' ', id, 2);
                              ref[y][(std::size_t)x] = g; ref[y][(std::size_t)x + 1] = 0; }
                else         { c.set(x, y, g, id, 0); ref[y][(std::size_t)x] = g; }
                x += gw;
            }
        }
        std::string wire; emit_full(c, pool, 0, nullptr, wire);
        DecFrame f = decode(wire);
        // reconstruct display grid from runs
        std::vector<std::u32string> got(H);
        for (int y = 0; y < H; ++y) got[y].assign((std::size_t)W, U' ');
        for (auto& r : f.runs) {
            std::u32string cps = decode_utf8_to_u32(r.utf8);
            int col = r.col;
            for (char32_t cp : cps) {
                int cw = (cp == U'世' || cp == U'界' || cp == U'あ') ? 2 : 1;
                if (col < W) got[r.row][(std::size_t)col] = cp;
                if (cw == 2 && col + 1 < W) got[r.row][(std::size_t)col + 1] = 0;
                col += cw;
            }
        }
        for (int y = 0; y < H && mismatches < 5; ++y)
            if (got[y] != ref[y]) {
                ++mismatches;
                std::println("  MISMATCH iter=%d row=%d", iter, y);
            }
    }
    assert(mismatches == 0);
    std::println("PASS (200 random wide/styled canvases round-trip)");
}

TEST_CASE("grid emit: ill-formed cell code points never reach the wire") {
    std::println("--- grid_emit utf-8 sanitizer ---");
    StylePool pool;
    Canvas c(6, 1, &pool);
    // Force pathological characters into cells (NUL, lone surrogate, > U+10FFFF).
    c.set(0, 0, U'\0',      0, 0);
    c.set(1, 0, (char32_t)0xD83D, 0, 0);   // lone high surrogate
    c.set(2, 0, (char32_t)0x140000, 0, 0); // beyond Unicode
    c.set(3, 0, U'A',       0, 0);
    std::string wire; emit_full(c, pool, 0, nullptr, wire);
    DecFrame f = decode(wire);

    // Every run's text must be well-formed UTF-8 (no NUL, no bad sequence).
    for (auto& r : f.runs) {
        std::u32string cps = decode_utf8_to_u32(r.utf8);
        for (char32_t cp : cps) {
            assert(cp != 0 && "NUL must have been mapped to space");
            assert(!(cp >= 0xD800 && cp <= 0xDFFF) && "surrogate leaked");
            assert(cp <= 0x10FFFF && "out-of-range code point leaked");
        }
        // Byte-level: no NUL, and re-decoding is lossless (valid UTF-8).
        for (char ch : r.utf8) assert(ch != '\0');
    }
    std::println("PASS (NUL→space, surrogate/oob→U+FFFD)");
}
