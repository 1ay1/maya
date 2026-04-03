// examples/viz.cpp — Signals: live time series + 2D wave interference heatmap
//
//   Left  — three braille area charts (cpu / mem / net signals)
//   Right — 2D wave interference heatmap, five animated sources, hot-cold ▀ blocks
//   Bar   — live values, fps counter, key hints
//
//   Keys: q / ESC = quit   p = pause

#include <maya/maya.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

using namespace maya;

// ── Braille dot-to-bit table ─────────────────────────────────────────────────
//
// Unicode 8-dot braille U+2800–U+28FF encodes 8 dots as 8 bits:
//
//   col  0 (left)     col  1 (right)
//   row 0  bit 0x01   bit 0x10
//   row 1  bit 0x02   bit 0x20
//   row 2  bit 0x04   bit 0x40
//   row 3  bit 0x08   bit 0x80
//
// One braille char = 2 pixel-columns × 4 pixel-rows of sub-cell resolution.

static constexpr uint8_t kBit[4][2] = {
    {0x01, 0x10}, {0x02, 0x20}, {0x04, 0x40}, {0x08, 0x80},
};

// ── Signal data ───────────────────────────────────────────────────────────────

static constexpr int kHistLen = 320;

struct SigData {
    const char* name;
    uint8_t lr, lg, lb;   // bright line colour
    uint8_t dr, dg, db;   // deep fill colour (darker shade)
    std::deque<float> hist;
    float val   = 0.5f;
};

static SigData g_sig[3] = {
    { "CPU",  85, 200, 255,  12, 42,  90 },
    { "MEM", 185,  85, 255,  60, 12, 110 },
    { "NET",  65, 240, 120,  12, 80,  36 },
};

static float g_time   = 0.f;
static bool  g_paused = false;

// ── Styles ────────────────────────────────────────────────────────────────────

static constexpr int kGrad = 20;  // gradient depth levels per signal
static constexpr int kHeat = 28;  // heatmap palette steps (kHeat² combos pre-interned)

struct Styles {
    uint16_t sig[3][kGrad];      // [signal][depth 0=line..kGrad-1=deepest fill]
    uint16_t sig_label[3];
    uint16_t heat[kHeat][kHeat]; // ▀ (fg=top pixel, bg=bottom pixel)
    uint16_t axis, grid, divider;
    uint16_t heat_title;
    uint16_t bar_bg, bar_brand, bar_val[3], bar_hint, bar_fps;
};

static Styles g_sty{};

// Hot-cold gradient  blue → cyan → green → yellow → red
static Color heat_color(float t) {
    t = std::clamp(t, 0.f, 1.f);
    uint8_t r = 0, g = 0, b = 0;
    if (t < 0.25f) {
        float f = t * 4.f;
        r = 0; g = static_cast<uint8_t>(f * 190.f); b = 255;
    } else if (t < 0.5f) {
        float f = (t - 0.25f) * 4.f;
        r = 0; g = 190; b = static_cast<uint8_t>((1.f - f) * 255.f);
    } else if (t < 0.75f) {
        float f = (t - 0.5f) * 4.f;
        r = static_cast<uint8_t>(f * 255.f);
        g = static_cast<uint8_t>(190.f + f * 40.f);
        b = 0;
    } else {
        float f = (t - 0.75f) * 4.f;
        r = 255; g = static_cast<uint8_t>((1.f - f) * 230.f); b = 0;
    }
    return Color::rgb(r, g, b);
}

static void build_styles(StylePool& pool) {
    g_sty.axis      = pool.intern(Style{}.with_fg(Color::rgb(48, 58, 82)));
    g_sty.grid      = pool.intern(Style{}.with_fg(Color::rgb(22, 26, 40)).with_dim());
    g_sty.divider   = pool.intern(Style{}.with_fg(Color::rgb(42, 52, 76)));
    g_sty.heat_title= pool.intern(Style{}.with_bold().with_fg(Color::rgb(145, 160, 210)));
    g_sty.bar_bg    = pool.intern(Style{}.with_bg(Color::rgb(9, 11, 18)));
    g_sty.bar_brand = pool.intern(Style{}.with_bold()
                                         .with_fg(Color::rgb(155, 170, 220))
                                         .with_bg(Color::rgb(9, 11, 18)));
    g_sty.bar_hint  = pool.intern(Style{}.with_fg(Color::rgb(55, 70, 108))
                                         .with_bg(Color::rgb(9, 11, 18)));
    g_sty.bar_fps   = pool.intern(Style{}.with_fg(Color::rgb(70, 85, 130))
                                         .with_bg(Color::rgb(9, 11, 18)));

    for (int i = 0; i < 3; ++i) {
        const auto& s = g_sig[i];
        g_sty.sig_label[i] = pool.intern(
            Style{}.with_bold().with_fg(Color::rgb(s.lr, s.lg, s.lb)));
        g_sty.bar_val[i] = pool.intern(
            Style{}.with_bold().with_fg(Color::rgb(s.lr, s.lg, s.lb))
                   .with_bg(Color::rgb(9, 11, 18)));

        for (int d = 0; d < kGrad; ++d) {
            float t  = static_cast<float>(d) / (kGrad - 1);
            auto  r  = static_cast<uint8_t>(s.lr * (1.f - t) + s.dr * t);
            auto  gg = static_cast<uint8_t>(s.lg * (1.f - t) + s.dg * t);
            auto  b  = static_cast<uint8_t>(s.lb * (1.f - t) + s.db * t);
            r  = std::max(r,  static_cast<uint8_t>(8));
            gg = std::max(gg, static_cast<uint8_t>(8));
            b  = std::max(b,  static_cast<uint8_t>(8));
            Style st = Style{}.with_fg(Color::rgb(r, gg, b));
            if (d >= kGrad * 2 / 3) st = st.with_dim();
            g_sty.sig[i][d] = pool.intern(st);
        }
    }

    // Pre-intern all kHeat × kHeat ▀ foreground/background style combos
    const float kN = static_cast<float>(kHeat - 1);
    for (int fi = 0; fi < kHeat; ++fi) {
        for (int bi = 0; bi < kHeat; ++bi) {
            g_sty.heat[fi][bi] = pool.intern(
                Style{}.with_fg(heat_color(fi / kN))
                       .with_bg(heat_color(bi / kN)));
        }
    }
}

// ── Simulation ────────────────────────────────────────────────────────────────

static void tick(float dt) {
    if (g_paused) return;
    g_time += dt;

    // Three independent oscillators with slow target drift
    static float ph[3]  = {0.f, 1.8f, 3.9f};
    static float tgt[3] = {0.5f, 0.6f, 0.28f};
    static const float rate[3] = {1.35f, 0.65f, 2.1f};

    for (int i = 0; i < 3; ++i) {
        ph[i]  += dt * rate[i];
        tgt[i]  = std::clamp(0.15f + 0.7f * (0.5f + 0.5f * sinf(ph[i])), 0.04f, 0.96f);
        g_sig[i].val = g_sig[i].val * 0.87f + tgt[i] * 0.13f;
        g_sig[i].hist.push_back(g_sig[i].val);
        if (static_cast<int>(g_sig[i].hist.size()) > kHistLen)
            g_sig[i].hist.pop_front();
    }
}

// ── Braille area chart ────────────────────────────────────────────────────────
//
// Plots one signal as a filled area chart in the canvas region [x0,x1) × [y0,y1).
// Each terminal cell holds one braille char (2 px wide × 4 px tall).
// The area below the curve is filled with braille dots; the gradient fades
// from bright at the curve line to dim at the bottom.

static void paint_chart(Canvas& canvas, int si, int x0, int y0, int x1, int y1)
{
    const int CW = x1 - x0;   // chars wide
    const int CH = y1 - y0;   // chars tall
    if (CW <= 0 || CH <= 0) return;
    const int PW = CW * 2;    // pixel columns (2 dots per char-col)
    const int PH = CH * 4;    // pixel rows    (4 dots per char-row)

    const auto& hist  = g_sig[si].hist;
    const int   hlen  = static_cast<int>(hist.size());

    std::vector<uint8_t> bits(CW * CH, 0);
    std::vector<int>     curve_py(PW, -1);  // curve pixel-row per pixel-column

    for (int px = 0; px < PW; ++px) {
        const int di = hlen - PW + px;
        if (di < 0 || di >= hlen) continue;

        const float v  = std::clamp(hist[di], 0.f, 1.f);
        const int   py = static_cast<int>((1.f - v) * (PH - 1)); // 0=top
        curve_py[px] = py;

        // Fill all dots from the curve down to the bottom
        for (int row = py; row < PH; ++row) {
            const int cy = row / 4, dr = row % 4;
            const int cx = px  / 2, dc = px  % 2;
            if (cy < CH) bits[cy * CW + cx] |= kBit[dr][dc];
        }
    }

    for (int cy = 0; cy < CH; ++cy) {
        for (int cx = 0; cx < CW; ++cx) {
            const uint8_t mask = bits[cy * CW + cx];
            if (!mask) continue;

            // Nearest curve_py within this char's two pixel-columns
            int min_curve = cy * 4 + 4;
            for (int dc = 0; dc < 2; ++dc) {
                const int px = cx * 2 + dc;
                if (px < PW && curve_py[px] >= 0)
                    min_curve = std::min(min_curve, curve_py[px]);
            }

            // Depth: pixel rows between this char's top and the nearest curve
            const int   depth = std::max(0, cy * 4 - min_curve);
            const float t     = std::min(static_cast<float>(depth) / static_cast<float>(PH), 1.f);
            const int   grad  = static_cast<int>(t * t * (kGrad - 1));

            canvas.set(x0 + cx, y0 + cy,
                       static_cast<char32_t>(0x2800u + mask),
                       g_sty.sig[si][grad]);
        }
    }
}

// ── 2D wave interference heatmap ─────────────────────────────────────────────
//
// Five animated point sources radiating sinusoidal waves that interfere.
// Each terminal row maps to TWO pixel rows via ▀ (U+2580, upper half block):
//   fg colour = top pixel value,  bg colour = bottom pixel value.
// This gives 2× vertical resolution with a vivid continuous colour field.

static void paint_heatmap(Canvas& canvas, int x0, int y0, int x1, int y1)
{
    const int CW = x1 - x0;
    const int CH = y1 - y0;
    if (CW <= 0 || CH <= 0) return;

    const float W  = static_cast<float>(CW);
    const float PH = static_cast<float>(CH * 2);
    const float kN = static_cast<float>(kHeat - 1);

    struct Src { float x, y, k, w; };

    // Four corner sources + one roaming central source
    const Src srcs[5] = {
        {0.12f * W, 0.14f * PH, 0.29f, 2.15f},
        {0.88f * W, 0.18f * PH, 0.23f, 1.85f},
        {0.14f * W, 0.86f * PH, 0.27f, 2.55f},
        {0.86f * W, 0.84f * PH, 0.25f, 1.95f},
        {W  * (0.5f + 0.22f * sinf(g_time * 0.37f)),
         PH * (0.5f + 0.19f * cosf(g_time * 0.28f)),
         0.34f, 3.1f},
    };

    // Aspect-ratio correction: terminal cells are ~2× taller than wide
    const float ar = (W / PH) * 1.6f;

    for (int cy = 0; cy < CH; ++cy) {
        for (int cx = 0; cx < CW; ++cx) {
            float v[2];
            for (int off = 0; off < 2; ++off) {
                const float px = static_cast<float>(cx) + 0.5f;
                const float py = static_cast<float>(cy * 2 + off) + 0.5f;
                float s = 0.f;
                for (const auto& src : srcs) {
                    const float dx = px - src.x;
                    const float dy = (py - src.y) * ar;
                    s += sinf(src.k * sqrtf(dx*dx + dy*dy) - src.w * g_time);
                }
                v[off] = std::clamp(s / 5.f * 0.62f + 0.5f, 0.f, 1.f);
            }
            const int fi = std::clamp(static_cast<int>(v[0] * kN), 0, kHeat - 1);
            const int bi = std::clamp(static_cast<int>(v[1] * kN), 0, kHeat - 1);
            canvas.set(x0 + cx, y0 + cy, U'\u2580', g_sty.heat[fi][bi]);
        }
    }
}

// ── Drawing helpers ───────────────────────────────────────────────────────────

// Write ASCII text and return the x position after the last character.
static int wstr(Canvas& c, int x, int y, const char* s, uint16_t sid) {
    const auto len = std::strlen(s);
    c.write_text(x, y, {s, len}, sid);
    return x + static_cast<int>(len);
}

// ── Main paint ────────────────────────────────────────────────────────────────

static void paint(Canvas& canvas, int W, int H,
                  int frame_count, double fps)
{
    const int bar_y  = H - 1;
    const int main_h = H - 1;

    // Panel split: left ≈60% for series, right ≈40% for heatmap
    const int div_x = std::max(28, W * 6 / 10);
    const int rp_x  = div_x + 1;

    // Left label column width (name + value)
    const int lw = 6;

    // Each of the 3 signal panels gets roughly equal height
    const int sh = main_h / 3;

    // ── three signal panels ───────────────────────────────────────────────────

    for (int i = 0; i < 3; ++i) {
        const int y0 = i * sh;
        const int y1 = (i == 2) ? main_h : y0 + sh;

        // Horizontal separator between panels (skip top edge)
        if (i > 0) {
            for (int x = 0; x < div_x; ++x)
                canvas.set(x, y0, U'\u2500', g_sty.grid);     // ─
            // Junction with vertical divider
            canvas.set(div_x, y0, U'\u2524', g_sty.divider);  // ┤
        }

        // Vertical left border
        for (int y = y0; y < y1; ++y)
            canvas.set(0, y, U'\u2502', g_sty.axis);           // │

        // Label area (vertically centred in panel)
        const int cy = (y0 + y1) / 2;
        canvas.write_text(1, cy - 1, std::string_view(g_sig[i].name, 3), g_sty.sig_label[i]);
        char pct[6];
        std::snprintf(pct, sizeof pct, "%3.0f%%", g_sig[i].val * 100.f);
        canvas.write_text(1, cy, {pct, 4}, g_sty.sig[i][0]);

        // Braille area chart
        paint_chart(canvas, i, lw, y0, div_x, y1);
    }

    // ── vertical divider ─────────────────────────────────────────────────────

    for (int y = 0; y < main_h; ++y)
        canvas.set(div_x, y, U'\u2502', g_sty.divider);  // │

    // ── heatmap panel ─────────────────────────────────────────────────────────

    // Centred title above heatmap
    static constexpr std::string_view kTitle = " INTERFERENCE ";
    const int title_x = rp_x + std::max(0, (W - rp_x - static_cast<int>(kTitle.size())) / 2);
    canvas.write_text(title_x, 0, kTitle, g_sty.heat_title);

    paint_heatmap(canvas, rp_x, 1, W, main_h);

    // ── status bar ────────────────────────────────────────────────────────────

    for (int x = 0; x < W; ++x)
        canvas.set(x, bar_y, U' ', g_sty.bar_bg);

    int bx = 1;
    bx = wstr(canvas, bx, bar_y, " SIGNALS ", g_sty.bar_brand);
    bx = wstr(canvas, bx, bar_y, "  ",        g_sty.bar_bg);

    for (int i = 0; i < 3; ++i) {
        bx = wstr(canvas, bx, bar_y, g_sig[i].name, g_sty.sig_label[i]);
        char buf[8];
        std::snprintf(buf, sizeof buf, " %3.0f%%  ", g_sig[i].val * 100.f);
        bx = wstr(canvas, bx, bar_y, buf, g_sty.bar_val[i]);
    }

    // Right-aligned: fps + key hints
    char fps_buf[24];
    std::snprintf(fps_buf, sizeof fps_buf, "%.0f fps  ", fps);
    const char* hints    = "[p] pause  [q] quit ";
    const int   rlen     = static_cast<int>(std::strlen(fps_buf) + std::strlen(hints)) + 1;
    int         rx       = W - rlen;
    if (rx > bx) {
        rx = wstr(canvas, rx, bar_y, fps_buf, g_sty.bar_fps);
        wstr(canvas, rx, bar_y, hints, g_sty.bar_hint);
    }

    (void)frame_count;
}

// ── main ──────────────────────────────────────────────────────────────────────

int main()
{
    using Clock = std::chrono::steady_clock;
    auto  last  = Clock::now();

    // FPS tracking
    int    frames    = 0;
    double fps       = 60.0;
    auto   fps_clock = Clock::now();

    // Warm up history with smooth initial data so charts aren't empty on launch
    for (int i = 0; i < 3; ++i) {
        float ph = static_cast<float>(i) * 2.09f;
        float v  = g_sig[i].val;
        for (int j = 0; j < kHistLen / 2; ++j) {
            ph += 0.09f;
            v   = v * 0.9f + (0.15f + 0.7f * (0.5f + 0.5f * sinf(ph))) * 0.1f;
            g_sig[i].hist.push_back(v);
        }
        g_sig[i].val = v;
    }

    auto result = canvas_run(
        CanvasConfig{.fps = 60, .title = "signals · maya"},

        [](StylePool& pool, int, int) {
            build_styles(pool);
        },

        [](const Event& ev) -> bool {
            if (const auto* ke = std::get_if<KeyEvent>(&ev)) {
                if (const auto* ck = std::get_if<CharKey>(&ke->key)) {
                    switch (ck->codepoint) {
                        case 'q': case 'Q': return false;
                        case 'p': case 'P': g_paused = !g_paused; break;
                    }
                }
                if (const auto* sk = std::get_if<SpecialKey>(&ke->key))
                    if (*sk == SpecialKey::Escape) return false;
            }
            return true;
        },

        [&](Canvas& canvas, int W, int H) {
            const auto now = Clock::now();
            const float dt = std::min(
                std::chrono::duration<float>(now - last).count(), 0.1f);
            last = now;

            ++frames;
            const double el = std::chrono::duration<double>(now - fps_clock).count();
            if (el >= 0.5) {
                fps = frames / el;
                frames = 0;
                fps_clock = now;
            }

            tick(dt);
            paint(canvas, W, H, frames, fps);
        }
    );

    if (!result) {
        std::fprintf(stderr, "maya: %s\n", result.error().message.c_str());
        return 1;
    }
}
