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
#include <unordered_map>
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
    std::uint32_t varint() {
        std::uint32_t v = 0; int shift = 0; std::uint8_t b;
        do { b = u8(); v |= std::uint32_t(b & 0x7F) << shift; shift += 7; }
        while (b & 0x80);
        return v;
    }
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
    const bool varint_runs = (f.flags & 0x20) != 0;   // kFlagVarintRuns
    for (int k = 0; k < nruns; ++k) {
        DecRun run;
        if (varint_runs) {
            run.row = static_cast<std::uint16_t>(r.varint());
            run.col = static_cast<std::uint16_t>(r.varint());
            run.len = static_cast<std::uint16_t>(r.varint());
            run.style = static_cast<std::uint16_t>(r.varint());
        } else {
            run.row = r.u16(); run.col = r.u16();
            run.len = r.u16(); run.style = r.u16();
        }
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

TEST_CASE("grid emit: diff with col_lo emits only the changed suffix") {
    std::println("--- grid_emit diff column-range ---");
    // During the streaming glide only a row's TAIL columns change each frame.
    // Passing per-row first-changed columns (changed_cols) must make the Diff
    // emit runs starting at that column — the leading columns stay untouched on
    // the host (Diff is an (row,col) overlay), so they are absent from the wire.
    StylePool pool;
    Canvas c(24, 1, &pool);
    c.clear();
    std::uint16_t s = pool.intern(Style{});
    c.write_text(0, 0, "stable prefix TAILGREW", s);

    // Only columns >= 14 ("TAILGREW") changed this frame.
    std::string wire;
    std::vector<int> changed{0};
    std::vector<int> changed_cols{14};
    emit_diff(c, pool, changed, /*base_row=*/0, /*cursor=*/nullptr, wire,
              &changed_cols);
    DecFrame f = decode(wire);

    assert(f.type == static_cast<std::uint8_t>(GridFrameType::Diff));
    assert(!f.runs.empty());
    // No run may start before the changed column — the prefix is never re-sent.
    for (const auto& r : f.runs)
        assert(r.col >= 14 && "col_lo must clip leading columns off the wire");
    // The first run starts exactly at the changed column, and the emitted
    // suffix reconstructs the changed text (space-trimmed for the trailing
    // blanks the fixed-width canvas carries).
    assert(f.runs.front().col == 14);
    std::string suffix = runs_text(f);
    // "TAILGREW" begins the suffix (rest is blank fill to width 24).
    assert(suffix.rfind("TAILGREW", 0) == 0 &&
           "suffix must start with the changed text");

    // Contrast: WITHOUT col_lo the whole row is emitted (starts at col 0).
    std::string wire_full;
    emit_diff(c, pool, changed, /*base_row=*/0, /*cursor=*/nullptr, wire_full);
    DecFrame ff = decode(wire_full);
    assert(ff.runs.front().col == 0 && "null changed_cols keeps full-row behaviour");
    // The column-clipped frame is strictly smaller on the wire.
    assert(wire.size() < wire_full.size() &&
           "column-range diff must shrink the frame");
    std::println("PASS (suffix wire %zu B < full-row %zu B)",
                 wire.size(), wire_full.size());
}

TEST_CASE("grid emit: col_lo on a wide-glyph trailing half backs up to the lead") {
    std::println("--- grid_emit diff col_lo wide-glyph guard ---");
    // If the first-changed column lands on the TRAILING half of a wide glyph,
    // the emitter must back up to the glyph's lead cell so the host never gets
    // a dangling continuation / half a glyph.
    StylePool pool;
    Canvas c(10, 1, &pool);
    c.clear();
    std::uint16_t s = pool.intern(Style{});
    // "AB" then a wide glyph at columns 2-3, then "CD".
    c.write_text(0, 0, "AB\xe4\xb8\x96" "CD", s);   // U+4E16 (世) is width-2

    // Pretend the change starts at column 3 = the wide glyph's TRAILING half.
    std::string wire;
    std::vector<int> changed{0};
    std::vector<int> changed_cols{3};
    emit_diff(c, pool, changed, /*base_row=*/0, /*cursor=*/nullptr, wire,
              &changed_cols);
    DecFrame f = decode(wire);
    assert(!f.runs.empty());
    // Must start at column 2 (the wide glyph's LEAD), not 3.
    assert(f.runs.front().col == 2 &&
           "col_lo on a wide trailing half must snap back to the lead cell");
    std::println("PASS (snapped col_lo 3 -> lead col %u)", f.runs.front().col);
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

// ── v3: cross-frame style dictionary (StyleAckSet) ─────────────────────
TEST_CASE("grid emit: style dictionary omits re-sends, host resolves via cache") {
    std::println("--- grid_emit v3 style dictionary ---");
    StylePool pool;
    Canvas c(24, 3, &pool);

    const std::uint16_t red  = pool.intern(Style{}.with_fg(Color::red()));
    const std::uint16_t blue = pool.intern(Style{}.with_fg(Color::blue()));

    // Host-side style cache: id → definition, exactly what a cooperating host
    // maintains. A run's style is resolved from here; a partial frame only
    // TOPS UP the cache with styles it hasn't sent before.
    std::unordered_map<std::uint16_t, DecStyle> host_cache;
    auto ingest = [&](const DecFrame& f) {
        for (const auto& st : f.styles) host_cache[st.id] = st;
    };
    // Assert every run references a style the host can resolve (present in the
    // cache after ingesting this frame's partial table).
    auto all_runs_resolvable = [&](const DecFrame& f) {
        for (const auto& r : f.runs)
            if (host_cache.find(r.style) == host_cache.end()) return false;
        return true;
    };

    StyleAckSet ack;

    // Frame 1: paint red "aaa" on row 0. First sight of `red` → its definition
    // MUST be present (partial table can't omit an unacked style).
    c.clear();
    c.write_text(0, 0, "aaa", red);
    std::string w1;
    emit_diff(c, pool, {0}, 0, nullptr, w1, nullptr, &ack);
    DecFrame f1 = decode(w1);
    ingest(f1);
    assert(all_runs_resolvable(f1) && "frame 1 red must be resolvable");
    bool f1_has_red = false;
    for (auto& st : f1.styles) if (st.id == red) f1_has_red = true;
    assert(f1_has_red && "first sight of a style must send its definition");

    // Frame 2: same red style on row 1 (append). red is ALREADY acked, so its
    // definition MUST be omitted (partial-table bit set) — yet the run still
    // references id `red`, which the host resolves from its cache.
    c.write_text(0, 1, "bbb", red);
    std::string w2;
    emit_diff(c, pool, {1}, 0, nullptr, w2, nullptr, &ack);
    DecFrame f2 = decode(w2);
    assert((f2.flags & kFlagPartialStyleTable) &&
           "re-used style must produce a PARTIAL table (bit4)");
    bool f2_has_red = false;
    for (auto& st : f2.styles) if (st.id == red) f2_has_red = true;
    assert(!f2_has_red && "an acked style's definition must be omitted");
    ingest(f2);
    assert(all_runs_resolvable(f2) &&
           "host must still resolve the re-used red from its cache");

    // Frame 3: introduce a NEW style (blue) on row 2. blue is unacked so its
    // definition IS sent; red (also on-screen if repainted) stays omitted.
    c.write_text(0, 2, "ccc", blue);
    std::string w3;
    emit_diff(c, pool, {2}, 0, nullptr, w3, nullptr, &ack);
    DecFrame f3 = decode(w3);
    bool f3_has_blue = false;
    for (auto& st : f3.styles) if (st.id == blue) f3_has_blue = true;
    assert(f3_has_blue && "a newly-seen style must send its definition");
    ingest(f3);
    assert(all_runs_resolvable(f3));

    // A Full frame RESETS the dictionary: the host re-states, so every style
    // it references must be re-sent (cache is invalidated).
    host_cache.clear();
    std::string wf;
    emit_full(c, pool, 0, nullptr, wf, &ack);
    DecFrame ff = decode(wf);
    ingest(ff);
    assert(!(ff.flags & kFlagPartialStyleTable) &&
           "a Full frame must send a COMPLETE style table (no partial bit)");
    assert(all_runs_resolvable(ff) &&
           "after a Full re-state every referenced style is present again");

    std::println("PASS (def sent once, re-used styles omitted, Full resets)");
}

TEST_CASE("grid emit: style dictionary shrinks the wire on a repeated style") {
    std::println("--- grid_emit v3 wire savings ---");
    StylePool pool;
    Canvas c(40, 1, &pool);
    const std::uint16_t s = pool.intern(Style{}.with_fg(Color::red())
                                              .with_bg(Color::blue()));

    // Emit the SAME styled row many times, once WITHOUT the dictionary (v2:
    // full style table every frame) and once WITH it (v3: definition sent on
    // frame 1, omitted thereafter). The v3 total must be strictly smaller.
    auto total = [&](bool use_ack) {
        StyleAckSet ack;
        std::size_t bytes = 0;
        for (int frame = 0; frame < 50; ++frame) {
            c.clear();
            c.write_text(0, 0, "styled row content here", s);
            std::string w;
            emit_diff(c, pool, {0}, 0, nullptr, w, nullptr,
                      use_ack ? &ack : nullptr);
            bytes += w.size();
        }
        return bytes;
    };
    const std::size_t v2 = total(/*use_ack=*/false);
    const std::size_t v3 = total(/*use_ack=*/true);
    std::println("  v2 (full table/frame) = {} B   v3 (dictionary) = {} B "
                 "({:.2f}x less)", v2, v3, double(v2) / double(v3));
    assert(v3 < v2 && "the dictionary must reduce total wire on a repeated style");
}

// ── v3: varint run headers (StyleAckSet::varint_runs) ─────────────────
TEST_CASE("grid emit: varint run headers round-trip cell-for-cell") {
    std::println("--- grid_emit v3 varint runs ---");
    StylePool pool;
    Canvas c(30, 4, &pool);
    const std::uint16_t red = pool.intern(Style{}.with_fg(Color::red()));
    c.clear();
    c.write_text(2, 0, "hello", red);
    c.write_text(0, 1, "world wide row of text", 0);
    c.write_text(5, 3, "tail", red);

    std::vector<int> rows{0, 1, 3};
    // Fixed-header frame (v2) and varint frame (v3) of the SAME content.
    std::string fixed, var;
    emit_diff(c, pool, rows, 0, nullptr, fixed, nullptr, nullptr);
    StyleAckSet ack; ack.varint_runs = true;
    // Pre-ack the style so the two frames differ ONLY in run encoding, not in
    // the style table (isolates the varint effect).
    { std::string warm; emit_diff(c, pool, rows, 0, nullptr, warm, nullptr, &ack); }
    var.clear();
    emit_diff(c, pool, rows, 0, nullptr, var, nullptr, &ack);

    DecFrame ff = decode(fixed);
    DecFrame vf = decode(var);
    assert(!(ff.flags & 0x20) && "fixed frame must NOT set the varint bit");
    assert((vf.flags & 0x20) && "varint frame must set bit5");

    // The runs must decode to IDENTICAL (row,col,len,style,text) tuples — the
    // encoding differs, the content does not.
    assert(ff.runs.size() == vf.runs.size() && "run count must match");
    for (std::size_t k = 0; k < ff.runs.size(); ++k) {
        assert(ff.runs[k].row  == vf.runs[k].row);
        assert(ff.runs[k].col  == vf.runs[k].col);
        assert(ff.runs[k].len  == vf.runs[k].len);
        assert(ff.runs[k].style== vf.runs[k].style);
        assert(ff.runs[k].utf8 == vf.runs[k].utf8);
    }
    std::println("PASS (varint runs decode identically to fixed u16 runs)");
}

TEST_CASE("grid emit: varint headers shrink the run section") {
    std::println("--- grid_emit v3 varint savings ---");
    StylePool pool;
    Canvas c(80, 20, &pool);
    // Many small runs: one styled word per row (small row/col/len/style — the
    // regime varints win in).
    const std::uint16_t s = pool.intern(Style{}.with_fg(Color::green()));
    c.clear();
    std::vector<int> rows;
    for (int y = 0; y < 20; ++y) { c.write_text(y % 10, y, "word", s); rows.push_back(y); }

    std::string fixed; emit_diff(c, pool, rows, 0, nullptr, fixed, nullptr, nullptr);
    StyleAckSet ack; ack.varint_runs = true;
    { std::string warm; emit_diff(c, pool, rows, 0, nullptr, warm, nullptr, &ack); }
    std::string var; emit_diff(c, pool, rows, 0, nullptr, var, nullptr, &ack);

    std::println("  fixed u16x4 = {} B   varint = {} B ({:.2f}x less)",
                 fixed.size(), var.size(),
                 double(fixed.size()) / double(var.size()));
    // Varint must be strictly smaller here (all coords < 128 → 1 byte each,
    // vs 2 bytes each fixed) AND still decode identically.
    assert(var.size() < fixed.size() &&
           "varint run headers must shrink the wire on small coordinates");
    DecFrame vf = decode(var);
    assert(!vf.runs.empty() && "word-runs must survive the varint round-trip");
}

// ── v3: interior-span run splitting — OVERLAY correctness across frames ───
//
// This is the load-bearing test for the biggest grid win. Interior splitting
// SKIPS unchanged cells, trusting the host to keep them (overlay semantics).
// If that trust is ever misplaced the screen corrupts. So: simulate a real
// cooperating host — a full cell grid the host maintains — feed it a sequence
// of interior-split diff frames built against the host's OWN prior cells, and
// assert the host's reconstructed grid matches the source canvas CELL-FOR-CELL
// after every frame. A single wrong skip fails this.
TEST_CASE("grid emit: interior-split diffs reconstruct the grid cell-for-cell") {
    std::println("--- grid_emit v3 interior-split overlay ---");
    const int W = 40, H = 12;
    StylePool pool;
    const std::uint16_t plain = 0;
    const std::uint16_t red   = pool.intern(Style{}.with_fg(Color::red()));
    const std::uint16_t bold  = pool.intern(Style{}.with_fg(Color::blue()));

    // Host-side grid: packed cell per (row,col). Starts blank (all 0 = space).
    // A run overlays len cells starting at (row,col); characters are decoded
    // from utf8, style from the run. We don't need exact packing — we compare
    // the host grid the SAME way against a reference built from the canvas.
    struct HostCell { char32_t ch; std::uint16_t style; };
    std::vector<HostCell> host(static_cast<std::size_t>(W) * H, {U' ', 0});
    std::unordered_map<std::uint16_t, DecStyle> host_styles;

    auto apply = [&](const DecFrame& f) {
        for (const auto& st : f.styles) host_styles[st.id] = st;
        for (const auto& r : f.runs) {
            // Decode the run's utf8 into codepoints and overlay them.
            std::size_t bi = 0; int col = r.col;
            for (int k = 0; k < r.len && col < W; ++k) {
                unsigned char lead = static_cast<unsigned char>(r.utf8[bi]);
                int nb = (lead < 0x80) ? 1 : (lead < 0xE0) ? 2 : (lead < 0xF0) ? 3 : 4;
                char32_t cp;
                if (nb == 1) cp = lead;
                else {
                    cp = lead & (0x7F >> nb);
                    for (int b = 1; b < nb; ++b)
                        cp = (cp << 6) | (static_cast<unsigned char>(r.utf8[bi + b]) & 0x3F);
                }
                bi += nb;
                host[static_cast<std::size_t>(r.row) * W + col] =
                    {cp == 0 ? U' ' : cp, r.style};
                ++col;
            }
        }
    };

    // Build a reference host grid directly from a canvas (the ground truth).
    auto reference_of = [&](const Canvas& c) {
        std::vector<HostCell> ref(static_cast<std::size_t>(W) * H, {U' ', 0});
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                Cell cc = c.get(x, y);
                ref[static_cast<std::size_t>(y) * W + x] =
                    {cc.character == 0 ? U' ' : cc.character, cc.style_id};
            }
        return ref;
    };

    StyleAckSet ack; ack.varint_runs = true;
    std::vector<std::uint64_t> prev;  // host's prior packed cells
    int prev_rows = 0;

    // A scripted sequence that EXERCISES interior gaps: text with unchanged
    // middles, style flips mid-row, edits that touch scattered columns.
    auto frame = [&](int fi) {
        Canvas c(W, H, &pool);
        c.clear();
        c.write_text(0, 0, "the quick brown fox jumps", plain);
        // row 1: a word in the MIDDLE changes each frame (interior gap on
        // both sides — the exact case suffix-emit wastes).
        c.write_text(0, 1, "stable left ", plain);
        c.write_text(12, 1, (fi % 2 ? "AAAA" : "BBBB"), red);
        c.write_text(16, 1, " stable right", plain);
        // row 2: style flip on an unchanged word.
        c.write_text(0, 2, "mixed ", plain);
        c.write_text(6, 2, "styled", fi % 3 ? bold : red);
        c.write_text(12, 2, " tail", plain);
        // a growing tail row that appends (fresh growth rows too).
        c.write_text(0, 3 + (fi % 6), "grown line", plain);
        return c;
    };

    for (int fi = 0; fi < 8; ++fi) {
        Canvas c = frame(fi);
        // Compute changed rows + first-changed cols vs the host's prior frame.
        std::vector<int> rows, cols;
        for (int y = 0; y < H; ++y) {
            int fd = -1;
            for (int x = 0; x < W; ++x) {
                std::uint64_t pv = (y < prev_rows)
                    ? prev[static_cast<std::size_t>(y) * W + x] : 0;
                if (c.get_packed(x, y) != pv) { fd = x; break; }
            }
            if (fd >= 0) { rows.push_back(y); cols.push_back(fd); }
        }
        std::string wire;
        if (!rows.empty()) {
            emit_diff(c, pool, rows, 0, nullptr, wire, &cols, &ack,
                      prev.empty() ? nullptr : prev.data(), W, prev_rows);
            apply(decode(wire));
        }
        // Snapshot the host's new belief.
        prev.assign(static_cast<std::size_t>(W) * H, 0);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                prev[static_cast<std::size_t>(y) * W + x] = c.get_packed(x, y);
        prev_rows = H;

        // THE INVARIANT: host grid == canvas, cell-for-cell.
        auto ref = reference_of(c);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const auto& h = host[static_cast<std::size_t>(y) * W + x];
                const auto& r = ref[static_cast<std::size_t>(y) * W + x];
                if (h.ch != r.ch) {
                    std::println("  MISMATCH frame={} ({},{}) host U+{:04X} "
                                 "ref U+{:04X}", fi, x, y,
                                 (std::uint32_t)h.ch, (std::uint32_t)r.ch);
                }
                assert(h.ch == r.ch &&
                       "interior-split overlay must reconstruct every cell");
            }
    }
    std::println("PASS (8 frames, interior gaps + style flips + growth, "
                 "overlay exact)");
}
