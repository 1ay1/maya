// tests/test_diff_fidelity.cpp — the frame diff must paint exactly the target.
//
// A property test for the encoder. Random frames (a small palette so style
// ids repeat, runs so there are gaps of every length, half-blocks and
// ASCII, occasional wide chars) are diffed frame after frame into a tiny VT
// model that understands exactly what diff() may emit: CUP, CUF, SGR
// (full + differential, 38/48 in 5 and 2 forms, 0/39/49 resets, attribute
// toggles), EL, UTF-8. After EVERY frame the model's screen must equal the
// target canvas as the terminal would show it: same glyph, same resolved
// look. Any shortcut that desynchronises the terminal (a wrong cursor move,
// a skipped cell that did change, a stale pen) fails here, at the frame and
// cell where it happened.

#include <maya/maya.hpp>
#include <maya/render/diff.hpp>

#include "agtest.hpp"

#include <cstdint>
#include <optional>
#include <print>
#include <random>
#include <string>
#include <vector>

using namespace maya;

namespace {

struct Pen {
    std::optional<std::uint32_t> fg, bg;   // packed colour key; nullopt = default
    std::uint8_t attrs = 0;                // 1 bold 2 italic 4 underline 8 inverse 16 strike
    bool operator==(const Pen&) const = default;
};

struct VCell { char32_t ch = U' '; Pen pen; bool operator==(const VCell&) const = default; };

struct Vt {
    int w, h, cx = 0, cy = 0;
    Pen pen;
    std::vector<VCell> cells;
    Vt(int w_, int h_) : w(w_), h(h_), cells(std::size_t(w_ * h_)) {}

    static std::uint32_t key(int kind, int a, int b = 0, int c = 0) {
        return std::uint32_t(kind) << 24 | std::uint32_t(a & 0xFF) << 16
             | std::uint32_t(b & 0xFF) << 8 | std::uint32_t(c & 0xFF);
    }
    void sgr(const std::vector<int>& ps) {
        for (std::size_t i = 0; i < ps.size(); ++i) {
            const int p = ps[i];
            if (p == 0) pen = {};
            else if (p == 1) pen.attrs |= 1;   else if (p == 22) pen.attrs &= ~1;
            else if (p == 3) pen.attrs |= 2;   else if (p == 23) pen.attrs &= ~2;
            else if (p == 4) pen.attrs |= 4;   else if (p == 24) pen.attrs &= ~4;
            else if (p == 7) pen.attrs |= 8;   else if (p == 27) pen.attrs &= ~8;
            else if (p == 9) pen.attrs |= 16;  else if (p == 29) pen.attrs &= ~16;
            else if (p == 39) pen.fg.reset();
            else if (p == 49) pen.bg.reset();
            else if ((p >= 30 && p <= 37) || (p >= 90 && p <= 97)) pen.fg = key(1, p);
            else if ((p >= 40 && p <= 47) || (p >= 100 && p <= 107)) pen.bg = key(1, p - 10);
            else if (p == 38 || p == 48) {
                std::uint32_t k = 0;
                if (i + 2 < ps.size() && ps[i + 1] == 5) { k = key(2, ps[i + 2]); i += 2; }
                else if (i + 4 < ps.size() && ps[i + 1] == 2) {
                    k = key(3, ps[i + 2], ps[i + 3], ps[i + 4]); i += 4;
                }
                (p == 38 ? pen.fg : pen.bg) = k;
            }
        }
    }
    // xterm semantics for wide glyphs: a wide char occupies [x, x+1], the
    // right half stored as '\0'. Writing anything into either half of an
    // existing wide char blanks the OTHER half (it can't survive split).
    void clobber(int x) {
        VCell& c = cells[std::size_t(cy * w + x)];
        if (c.ch == U'\0' && x > 0)                     // right half: blank the lead
            cells[std::size_t(cy * w + x - 1)] = {U' ', cells[std::size_t(cy * w + x - 1)].pen};
        else if (is_wide(c.ch) && x + 1 < w)             // lead: blank the right half
            cells[std::size_t(cy * w + x + 1)] = {U' ', cells[std::size_t(cy * w + x + 1)].pen};
    }
    static bool is_wide(char32_t ch) { return ch == U'\u4E2D'; }
    void put(char32_t ch) {
        if (cx >= w) return;                    // DECAWM off in maya's frames
        const bool wide = is_wide(ch);
        if (wide && cx + 1 >= w) return;        // no room: terminals drop it
        clobber(cx);
        if (wide) clobber(cx + 1);
        cells[std::size_t(cy * w + cx)] = {ch, pen};
        ++cx;
        if (wide) { cells[std::size_t(cy * w + cx)] = {U'\0', pen}; ++cx; }
    }
    void feed(const std::string& s) {
        std::size_t i = 0;
        while (i < s.size()) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c == 0x1b && i + 1 < s.size() && s[i + 1] == '[') {
                std::size_t j = i + 2; std::string params;
                while (j < s.size() && ((s[j] >= '0' && s[j] <= '9') || s[j] == ';' || s[j] == '?'))
                    params += s[j++];
                const char fin = s[j]; i = j + 1;
                std::vector<int> ps; { int v = 0; bool any = false;
                    for (char ch : params) {
                        if (ch == ';') { ps.push_back(any ? v : 0); v = 0; any = false; }
                        else if (ch != '?') { v = v * 10 + (ch - '0'); any = true; }
                    }
                    ps.push_back(any ? v : 0); }
                const bool empty = params.empty();
                switch (fin) {
                    case 'H': cy = (ps.size() > 0 && ps[0] ? ps[0] : 1) - 1;
                              cx = (ps.size() > 1 && ps[1] ? ps[1] : 1) - 1; break;
                    case 'C': cx = std::min(w - 1, cx + (empty || !ps[0] ? 1 : ps[0])); break;
                    case 'K': for (int x = cx; x < w; ++x) cells[std::size_t(cy * w + x)] = {U' ', pen}; break;
                    case 'm': sgr(empty ? std::vector<int>{0} : ps); break;
                    default: break;               // ?2026h/l, ?25l etc.
                }
                continue;
            }
            if (c == '\r') { cx = 0; ++i; continue; }
            if (c == '\n') { ++cy; ++i; continue; }
            // UTF-8 decode
            char32_t cp; int n;
            if (c < 0x80) { cp = c; n = 1; }
            else if ((c >> 5) == 6) { cp = c & 0x1F; n = 2; }
            else if ((c >> 4) == 14) { cp = c & 0x0F; n = 3; }
            else { cp = c & 0x07; n = 4; }
            for (int k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
            i += std::size_t(n);
            put(cp);
        }
    }
};

// What the terminal should show for a canvas cell: the pen the pool's full
// SGR for that style sets, fed through the same model.
Pen pen_of(const StylePool& pool, std::uint16_t id) {
    Vt v(1, 1);
    v.feed(std::string(pool.sgr(id)));
    return v.pen;
}

void check_frames(int w, int h, int frames, std::uint32_t seed, int palette) {
    StylePool pool;
    std::mt19937 rng(seed);
    std::vector<std::uint16_t> ids;
    for (int i = 0; i < palette; ++i) {
        const int r = int(rng() % 256), g = int(rng() % 256), b = int(rng() % 256);
        Style s = Style{}.with_fg(Color::rgb(r, g, b));
        if (rng() % 2) s = s.with_bg(Color::rgb(int(rng() % 256), int(rng() % 256), int(rng() % 256)));
        if (rng() % 7 == 0) s = s.with_bold();
        ids.push_back(pool.intern(s));
    }
    ids.push_back(0);
    const char32_t glyphs[] = {U'\u2580', U'\u2584', U' ', U'a', U'#', U'\u2588'};

    Canvas prev(w, h, &pool), cur(w, h, &pool);
    Vt vt(w, h);
    // Frame 0 is a full paint, as a host does on start.
    {
        std::string out;
        serialize(prev, pool, out);
        vt.cx = vt.cy = 0;
        vt.feed("\x1b[H" + out);
    }
    for (int f = 0; f < frames; ++f) {
        cur = Canvas(w, h, &pool);
        // Copy most of the previous frame, then change runs: gaps of every
        // length between changed cells, which is what the encoder's skip
        // logic has to get right.
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const Cell c = prev.get(x, y);
                cur.set(x, y, c.character, c.style_id, c.width);
            }
        const int changes = int(rng() % std::uint32_t(w * h / 2 + 1));
        for (int k = 0; k < changes; ++k) {
            const int x = int(rng() % std::uint32_t(w)), y = int(rng() % std::uint32_t(h));
            const int run = 1 + int(rng() % 6);
            const auto id = ids[rng() % ids.size()];
            if (rng() % 9 == 0 && x + 1 < w) {
                // A wide char, written the way the renderer writes one: the
                // lead (width 1) and its placeholder (width 2). Canvas::set
                // does not do the second half itself.
                cur.set(x, y, U'\u4E2D', id, 1);
                cur.set(x + 1, y, U' ', id, 2);
                continue;
            }
            const auto g = glyphs[rng() % std::size(glyphs)];
            for (int d = 0; d < run && x + d < w; ++d) cur.set(x + d, y, g, id);
        }
        // Repair split wide chars, as the renderer's painter does: a lead
        // whose placeholder was overwritten, or a placeholder whose lead
        // was, becomes a blank. The encoder only ever sees well-formed rows.
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const Cell c = cur.get(x, y);
                if (c.width == 1 && (x + 1 >= w || cur.get(x + 1, y).width != 2))
                    cur.set(x, y, U' ', c.style_id);
                else if (c.width == 2 && (x == 0 || cur.get(x - 1, y).width != 1))
                    cur.set(x, y, U' ', c.style_id);
            }
        std::string out;
        diff(prev, cur, pool, out);
        vt.feed(out);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const Cell want = cur.get(x, y);
                if (want.width == 2) continue;        // placeholder: covered by its lead
                const VCell& got = vt.cells[std::size_t(y * w + x)];
                const Pen want_pen = pen_of(pool, want.style_id);
                if (got.ch != want.character || !(got.pen == want_pen)) {
                    std::println("MISMATCH seed={} frame={} cell=({},{}) want U+{:04X} got U+{:04X} pen_ok={}",
                                 seed, f, x, y, std::uint32_t(want.character),
                                 std::uint32_t(got.ch), got.pen == want_pen);
                    assert(false);
                }
            }
        std::swap(prev, cur);
    }
}

}  // namespace

TEST_CASE("diff fidelity: incremental frames paint exactly the target") {
    std::println("--- test_diff_fidelity ---");
    for (std::uint32_t seed = 1; seed <= 12; ++seed) {
        check_frames(40, 8, 60, seed, 3);       // few styles: lots of same-pen runs
        check_frames(97, 5, 40, seed + 100, 40); // many styles: frequent SGR
    }
    std::println("PASS\n");
}
