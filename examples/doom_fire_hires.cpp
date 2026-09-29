// doom_fire_hires.cpp — the doom fire at blockishfire's exact fidelity.
//
// WHY THIS EXISTS
//
// examples/doom_fire.cpp draws with the upper half block '▀': one cell
// carries 2 pixels (top = fg, bottom = bg), and the field is sized to the
// terminal. blockishfire (github.com/yazgoo/blockishfire) runs a fixed
// 320x168 field and renders it through the `blockish` crate, which samples
// 8x8 = 64 source pixels per CELL and picks a glyph from a 52-entry table.
//
// Comparing those two measures the RESOLUTION CHOICE, not the frameworks:
// blockish does ~32x more sampling per cell, so of course it costs more.
// This file removes that difference. It ports blockish's renderer exactly
// -- same field size, same 64 samples per cell, same threshold rule, same
// glyph table, same nearest-match fallback -- and keeps maya's simulation
// and frame diffing. What is left when you run both is the difference
// between the two RUNTIMES, which is the thing worth measuring.
//
// THE ALGORITHM, from blockish-0.1.0/src/lib.rs (render_write_eol_with_write):
//
//   1. For each character cell, read the 8x8 patch of source pixels it
//      covers. Greyscale each as r+g+b, or 0 when alpha is 0.
//   2. Average those 64 greys.
//   3. Build a 64-bit mask, one bit per sub-pixel, set when that
//      sub-pixel's grey >= the cell average. Bit order is row-major from
//      the top-left, MSB first.
//   4. Look the mask up in a table of 52 (mask -> glyph, swap) entries.
//      On a miss, take the table entry with the smallest Hamming distance
//      (blockish stops early on distance 1).
//   5. Two colours come from the patch sorted by brightness: the lower
//      quartile is the dark one, the upper quartile the bright one. The
//      table entry says which is foreground.
//
// Step 3 is the part my first attempt got wrong: I approximated the glyph
// with a vertical fill fraction, which cannot express the quadrants,
// shades, or box-drawing entries, so edges came out blockier than the
// original. The table is ported verbatim below.
//
// Note blockish's own quality caveat, preserved here: it only writes every
// 8th pixel into the `sorted` array used for the quartiles (its comment
// says this "reduce[s] quality a lot" but keeps the sort fast). The port
// keeps that behaviour so the output matches rather than being quietly
// better.

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <random>
#include <vector>

#include <maya/maya.hpp>
#include <maya/host/run.hpp>
#include <maya/element/pixels.hpp>

using namespace maya;
using namespace std::chrono_literals;

// ── blockishfire's field and palette, verbatim ──────────────────────────
constexpr int kFireW = 320;
constexpr int kFireH = 168;

constexpr std::array<Rgb, 37> kPalette = {{
    {0x07,0x07,0x07},{0x1F,0x07,0x07},{0x2F,0x0F,0x07},{0x47,0x0F,0x07},
    {0x57,0x17,0x07},{0x67,0x1F,0x07},{0x77,0x1F,0x07},{0x8F,0x27,0x07},
    {0x9F,0x2F,0x07},{0xAF,0x3F,0x07},{0xBF,0x47,0x07},{0xC7,0x47,0x07},
    {0xDF,0x4F,0x07},{0xDF,0x57,0x07},{0xDF,0x57,0x07},{0xD7,0x5F,0x07},
    {0xD7,0x5F,0x07},{0xD7,0x67,0x0F},{0xCF,0x6F,0x0F},{0xCF,0x77,0x0F},
    {0xCF,0x7F,0x0F},{0xCF,0x87,0x17},{0xC7,0x87,0x17},{0xC7,0x8F,0x17},
    {0xC7,0x97,0x1F},{0xBF,0x9F,0x1F},{0xBF,0x9F,0x1F},{0xBF,0xA7,0x27},
    {0xBF,0xA7,0x27},{0xBF,0xAF,0x2F},{0xB7,0xAF,0x2F},{0xB7,0xB7,0x2F},
    {0xB7,0xB7,0x37},{0xCF,0xCF,0x6F},{0xDF,0xDF,0x9F},{0xEF,0xEF,0xC7},
    {0xFF,0xFF,0xFF},
}};

// ── blockish's glyph table ──────────────────────────────────────────────
// (mask, glyph, swap). `swap` true means the BRIGHT colour is the glyph's
// foreground; false means the entry was authored inverted, so the dark
// colour goes in front. Extracted mechanically from the Rust source, all
// 52 entries, in declaration order (which matters: the nearest-match scan
// below takes the first minimum, exactly as blockish's does).
struct BlockGlyph { std::uint64_t mask; char32_t ch; bool swap; };

constexpr std::array<BlockGlyph, 52> kGlyphs = {{
    {0x000000ff00000000ull, U'\u2500', true},   // ─
    {0x00000000ff000000ull, U'\u2500', true},   // ─
    {0x000000ffff000000ull, U'\u2501', true},   // ━
    {0x1010101010101010ull, U'\u2502', true},   // │
    {0x0808080808080808ull, U'\u2502', true},   // │
    {0x1818181818181818ull, U'\u2503', true},   // ┃
    {0xffffff00ffffffffull, U'\u2500', false},  // ─
    {0xffffffff00ffffffull, U'\u2500', false},  // ─
    {0xffffff0000ffffffull, U'\u2501', false},  // ━
    {0xefefefefefefefefull, U'\u2502', false},  // │
    {0xf7f7f7f7f7f7f7f7ull, U'\u2502', false},  // │
    {0xe7e7e7e7e7e7e7e7ull, U'\u2503', false},  // ┃
    {0xffffffff00000000ull, U'\u2580', true},   // ▀
    {0x00000000000000ffull, U'\u2581', true},   // ▁
    {0x000000000000ffffull, U'\u2582', true},   // ▂
    {0x0000000000ffffffull, U'\u2583', true},   // ▃
    {0x00000000ffffffffull, U'\u2584', true},   // ▄
    {0x000000ffffffffffull, U'\u2585', true},   // ▅
    {0x0000ffffffffffffull, U'\u2586', true},   // ▆
    {0x00ffffffffffffffull, U'\u2587', true},   // ▇
    {0x00000000ffffffffull, U'\u2580', false},  // ▀
    {0xffffffffffffff00ull, U'\u2581', false},  // ▁
    {0xffffffffffff0000ull, U'\u2582', false},  // ▂
    {0xffffffffff000000ull, U'\u2583', false},  // ▃
    {0xffffffff00000000ull, U'\u2584', false},  // ▄
    {0xffffff0000000000ull, U'\u2585', false},  // ▅
    {0xffff000000000000ull, U'\u2586', false},  // ▆
    {0xff00000000000000ull, U'\u2587', false},  // ▇
    {0xffffffffffffffffull, U'\u2588', true},   // █
    {0xfefefefefefefefeull, U'\u2589', true},   // ▉
    {0xfcfcfcfcfcfcfcfcull, U'\u258A', true},   // ▊
    {0xf8f8f8f8f8f8f8f8ull, U'\u258B', true},   // ▋
    {0xf0f0f0f0f0f0f0f0ull, U'\u258C', true},   // ▌
    {0xe0e0e0e0e0e0e0e0ull, U'\u258D', true},   // ▍
    {0xc0c0c0c0c0c0c0c0ull, U'\u258E', true},   // ▎
    {0x8080808080808080ull, U'\u258F', true},   // ▏
    {0x0f0f0f0f0f0f0f0full, U'\u2590', true},   // ▐
    {0x8822882288228822ull, U'\u2591', true},   // ░
    {0xaa54aa54aa54aa54ull, U'\u2592', true},   // ▒
    {0x77dd77dd77dd77ddull, U'\u2593', true},   // ▓
    {0xff00000000000000ull, U'\u2594', true},   // ▔
    {0x0101010101010101ull, U'\u2595', true},   // ▕
    {0x00000000f0f0f0f0ull, U'\u2596', true},   // ▖
    {0x000000000f0f0f0full, U'\u2597', true},   // ▗
    {0xf0f0f0f000000000ull, U'\u2598', true},   // ▘
    {0xf0f0f0f0ffffffffull, U'\u2599', true},   // ▙
    {0xf0f0f0f00f0f0f0full, U'\u259A', true},   // ▚
    {0xfffffffff0f0f0f0ull, U'\u259B', true},   // ▛
    {0xffffffff0f0f0f0full, U'\u259C', true},   // ▜
    {0x0f0f0f0f00000000ull, U'\u259D', true},   // ▝
    {0x0f0f0f0ff0f0f0f0ull, U'\u259E', true},   // ▞
    {0x0f0f0f0fffffffffull, U'\u259F', true},   // ▟
}};

// Exact hit, else the smallest Hamming distance. blockish returns early on
// distance 1 (it cannot do better than that without an exact match), and
// keeps the FIRST minimum otherwise; both are reproduced.
[[nodiscard]] const BlockGlyph& closest(std::uint64_t mask) noexcept {
    const BlockGlyph* best = &kGlyphs[0];
    int best_d = 65;
    for (const auto& g : kGlyphs) {
        if (g.mask == mask) return g;
        const int d = std::popcount(g.mask ^ mask);
        if (d < best_d) { best_d = d; best = &g; if (d == 1) break; }
    }
    return *best;
}

// ── Messages / model ────────────────────────────────────────────────────
struct Tick {};
struct Resize { int cols, rows; };
struct Quit {};
using Msg = std::variant<Tick, Resize, Quit>;

struct Model {
    std::vector<std::uint8_t> fire =
        std::vector<std::uint8_t>(static_cast<std::size_t>(kFireW) * kFireH, 0);
    Glyphs       grid;
    int          cols = 0, rows = 0;
    std::mt19937 rng{std::random_device{}()};

    void ignite() {
        for (int x = 0; x < kFireW; ++x)
            fire[static_cast<std::size_t>(kFireH - 1) * kFireW + x] = 36;
    }
};

// ── Simulation: blockishfire's rule, unchanged ──────────────────────────
void propagate(Model& m) {
    std::uniform_int_distribution<int> r3(0, 3);
    for (int x = 0; x < kFireW; ++x) {
        for (int y = 1; y < kFireH; ++y) {
            const std::size_t src = static_cast<std::size_t>(y) * kFireW + x;
            const std::uint8_t v = m.fire[src];
            if (v > 0) {
                const int r = r3(m.rng);
                const std::size_t dst = src - static_cast<std::size_t>(r) + 1;
                if (dst >= static_cast<std::size_t>(kFireW))
                    m.fire[dst - kFireW] =
                        static_cast<std::uint8_t>(v - static_cast<std::uint8_t>(r & 1));
            } else {
                m.fire[src - kFireW] = 0;
            }
        }
    }
}

// ── Render: blockish's cell encoder ─────────────────────────────────────
void render(Model& m) {
    if (m.cols <= 0 || m.rows <= 0) return;

    for (int cy = 0; cy < m.rows; ++cy) {
        for (int cx = 0; cx < m.cols; ++cx) {
            // The 8x8 patch of the field this cell samples. blockishfire
            // maps the terminal onto the whole field, so each cell covers
            // fireW/cols x fireH/rows source pixels; we take 8x8 samples
            // inside that, which is what blockish's dx/dy loops do.
            int greys[64];
            Rgb  cols8[8];          // every 8th sample, for the quartiles
            int  ncol = 0, sum = 0;

            for (int dy = 0; dy < 8; ++dy) {
                for (int dx = 0; dx < 8; ++dx) {
                    const int sx = std::clamp((cx * 8 + dx) * kFireW / (m.cols * 8), 0, kFireW - 1);
                    const int sy = std::clamp((cy * 8 + dy) * kFireH / (m.rows * 8), 0, kFireH - 1);
                    const int heat = m.fire[static_cast<std::size_t>(sy) * kFireW + sx];
                    const Rgb c = kPalette[static_cast<std::size_t>(std::clamp(heat, 0, 36))];
                    // blockishfire treats the darkest palette entry as
                    // transparent (alpha 0), which greyscales to 0.
                    const int grey = (c.r == 0x07 && c.g == 0x07 && c.b == 0x07)
                                   ? 0 : (c.r + c.g + c.b);
                    const int i = dy * 8 + dx;
                    greys[i] = grey;
                    sum += grey;
                    if (i % 8 == dy && ncol < 8) cols8[ncol++] = c;   // blockish's sampling
                }
            }

            const int avg = sum / 64;

            // One bit per sub-pixel, MSB first, set when at or above the
            // cell average. This is blockish's `group`.
            std::uint64_t mask = 0;
            for (int i = 0; i < 64; ++i)
                mask = (mask << 1) | (greys[i] >= avg ? 1ull : 0ull);

            // Quartiles of the sampled colours give the two colours.
            std::sort(cols8, cols8 + ncol, [](const Rgb& a, const Rgb& b) {
                return a.r + a.g + a.b < b.r + b.g + b.b;
            });
            const Rgb dark   = ncol ? cols8[ncol / 4]           : kPalette[0];
            const Rgb bright = ncol ? cols8[(3 * ncol) / 4]     : kPalette[0];

            const BlockGlyph& g = closest(mask);
            const Rgb fg = g.swap ? bright : dark;
            const Rgb bg = g.swap ? dark   : bright;

            m.grid(cx, cy) = Glyph{g.ch, fg, bg, false};
        }
    }
}

struct DoomFireHires {
    using Model = ::Model;
    using Msg   = ::Msg;
    using Cmd   = jaal::Cmd<Msg>;
    using Sub   = jaal::Sub<Msg, on_key, on_resize>;

    static Cmd init(Model& m) { m.ignite(); return {}; }

    static Cmd update(Model& m, Resize r) {
        m.cols = std::max(1, r.cols);
        m.rows = std::max(1, r.rows);
        m.grid = Glyphs(m.cols, m.rows);
        render(m);
        return {};
    }

    static Cmd update(Model& m, Tick) {
        if (m.cols == 0) return {};
        propagate(m);
        render(m);
        return {};
    }

    static Cmd update(Model&, Quit) { return Cmd::quit(0); }

    static Element view(const Model& m) {
        if (m.cols == 0) return dsl::text("");
        return glyphs(m.grid);
    }

    static Sub subscribe(const Model&) {
        return Sub::batch(
            Sub::every(16ms, Tick{}),
            Sub::on(on_resize{}, [](const ResizeEvent& r) -> std::optional<Msg> {
                return Resize{r.width.value, r.height.value};
            }),
            Sub::on(on_key{}, [](const KeyEvent& k) -> std::optional<Msg> {
                if (const auto* c = std::get_if<CharKey>(&k.key))
                    if (c->codepoint == U'q') return Quit{};
                if (std::holds_alternative<SpecialKey>(k.key)
                    && std::get<SpecialKey>(k.key) == SpecialKey::Escape) return Quit{};
                return std::nullopt;
            }));
    }
};

int main() {
    return run<DoomFireHires>({.title = "doom fire (blockish fidelity)", .fps = 0});
}
