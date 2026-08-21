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
    DecColor color() {
        DecColor c{u8(), 0, 0, 0};
        if (c.kind == 1 || c.kind == 2) c.a = u8();
        else if (c.kind == 3) { c.a = u8(); c.b = u8(); c.c = u8(); }
        return c;
    }
};

// Strip the APC wrapper (ESC _ G … ESC \) and decode the payload.
DecFrame decode(const std::string& wire) {
    // find ESC _ G
    auto p = wire.find("\x1b_G");
    assert(p != std::string::npos);
    auto end = wire.find("\x1b\\", p);
    assert(end != std::string::npos);
    std::string payload = wire.substr(p + 3, end - (p + 3));

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

    assert(f.ver == 1);
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
