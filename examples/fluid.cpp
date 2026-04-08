// maya — 2D Fluid / Smoke Simulation
//
// Real-time Navier-Stokes (Jos Stam "Stable Fluids") rendered with
// half-block characters for double vertical resolution.
//
// Keys: q/Esc=quit  1-5=palette  space=pause  +/-=viscosity  r=reset
// Mouse: drag to inject density + velocity

#include <maya/maya.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace maya;

// ── Constants ────────────────────────────────────────────────────────────────

static constexpr float DT   = 0.1f;
static constexpr int   ITER = 10;

// ── Palette ──────────────────────────────────────────────────────────────────

struct RGB { uint8_t r, g, b; };

struct Palette {
    const char* name;
    RGB stops[5];   // density 0..4 mapped to these colors
};

static constexpr Palette g_palettes[] = {
    {"FIRE",    {{0,0,0}, {140,20,0}, {220,100,0}, {255,220,50}, {255,255,255}}},
    {"OCEAN",   {{0,0,0}, {0,20,100}, {0,80,140},  {0,200,220},  {255,255,255}}},
    {"NEON",    {{0,0,0}, {80,0,120}, {180,0,180},  {255,80,200}, {255,255,255}}},
    {"SMOKE",   {{0,0,0}, {40,40,40}, {100,100,100},{180,180,180},{255,255,255}}},
    {"RAINBOW", {{0,0,0}, {255,0,80}, {0,200,100},  {80,120,255}, {255,255,255}}},
};

// ── State ────────────────────────────────────────────────────────────────────

static int g_N = 0;                 // grid width
static int g_M = 0;                 // grid height (terminal rows * 2)
static int g_palette = 0;
static bool g_paused = false;
static float g_visc = 0.0001f;
static float g_diff = 0.0001f;

static std::vector<float> g_dens, g_dens0;
static std::vector<float> g_vx, g_vy, g_vx0, g_vy0;

// Mouse state
static bool g_mouse_down = false;
static int  g_mouse_x = -1, g_mouse_y = -1;
static int  g_prev_mx = -1, g_prev_my = -1;

// Style cache: [fg_idx][bg_idx] for half-block rendering
static constexpr int CSTEPS = 64;
static uint16_t g_styles[CSTEPS][CSTEPS];
static uint16_t g_bar_style;

// ── Helpers ──────────────────────────────────────────────────────────────────

static inline int IX(int x, int y) { return y * g_N + x; }

static inline RGB lerp_rgb(RGB a, RGB b, float t) {
    t = std::clamp(t, 0.f, 1.f);
    return {
        static_cast<uint8_t>(a.r + (b.r - a.r) * t),
        static_cast<uint8_t>(a.g + (b.g - a.g) * t),
        static_cast<uint8_t>(a.b + (b.b - a.b) * t),
    };
}

static RGB density_to_color(float d) {
    auto& pal = g_palettes[g_palette];
    d = std::clamp(d, 0.f, 5.f);
    float t = d / 5.f * 4.f;           // map [0,5] -> [0,4]
    int seg = std::min(static_cast<int>(t), 3);
    float frac = t - seg;
    return lerp_rgb(pal.stops[seg], pal.stops[seg + 1], frac);
}

// ── Fluid solver (Jos Stam) ─────────────────────────────────────────────────

static void set_bnd(int b, std::vector<float>& x) {
    int N = g_N, M = g_M;
    for (int i = 1; i < N - 1; ++i) {
        x[IX(i, 0)]     = b == 2 ? -x[IX(i, 1)]     : x[IX(i, 1)];
        x[IX(i, M - 1)] = b == 2 ? -x[IX(i, M - 2)] : x[IX(i, M - 2)];
    }
    for (int j = 1; j < M - 1; ++j) {
        x[IX(0, j)]     = b == 1 ? -x[IX(1, j)]     : x[IX(1, j)];
        x[IX(N - 1, j)] = b == 1 ? -x[IX(N - 2, j)] : x[IX(N - 2, j)];
    }
    x[IX(0, 0)]         = 0.5f * (x[IX(1, 0)]     + x[IX(0, 1)]);
    x[IX(0, M - 1)]     = 0.5f * (x[IX(1, M - 1)] + x[IX(0, M - 2)]);
    x[IX(N - 1, 0)]     = 0.5f * (x[IX(N - 2, 0)] + x[IX(N - 1, 1)]);
    x[IX(N - 1, M - 1)] = 0.5f * (x[IX(N - 2, M - 1)] + x[IX(N - 1, M - 2)]);
}

static void diffuse(int b, std::vector<float>& x, std::vector<float>& x0, float diff) {
    float a = DT * diff * (g_N - 2) * (g_M - 2);
    float c = 1.f + 4.f * a;
    for (int k = 0; k < ITER; ++k) {
        for (int j = 1; j < g_M - 1; ++j)
            for (int i = 1; i < g_N - 1; ++i)
                x[IX(i, j)] = (x0[IX(i, j)] + a * (
                    x[IX(i - 1, j)] + x[IX(i + 1, j)] +
                    x[IX(i, j - 1)] + x[IX(i, j + 1)])) / c;
        set_bnd(b, x);
    }
}

static void advect(int b, std::vector<float>& d, std::vector<float>& d0,
                   std::vector<float>& vx, std::vector<float>& vy) {
    float dt0x = DT * (g_N - 2);
    float dt0y = DT * (g_M - 2);
    for (int j = 1; j < g_M - 1; ++j) {
        for (int i = 1; i < g_N - 1; ++i) {
            float x = i - dt0x * vx[IX(i, j)];
            float y = j - dt0y * vy[IX(i, j)];
            x = std::clamp(x, 0.5f, g_N - 1.5f);
            y = std::clamp(y, 0.5f, g_M - 1.5f);
            int i0 = static_cast<int>(x), j0 = static_cast<int>(y);
            int i1 = i0 + 1, j1 = j0 + 1;
            float s1 = x - i0, s0 = 1.f - s1;
            float t1 = y - j0, t0 = 1.f - t1;
            d[IX(i, j)] = s0 * (t0 * d0[IX(i0, j0)] + t1 * d0[IX(i0, j1)])
                        + s1 * (t0 * d0[IX(i1, j0)] + t1 * d0[IX(i1, j1)]);
        }
    }
    set_bnd(b, d);
}

static void project(std::vector<float>& vx, std::vector<float>& vy,
                    std::vector<float>& p, std::vector<float>& div) {
    float hx = 1.f / (g_N - 2);
    float hy = 1.f / (g_M - 2);
    for (int j = 1; j < g_M - 1; ++j)
        for (int i = 1; i < g_N - 1; ++i) {
            div[IX(i, j)] = -0.5f * (
                hx * (vx[IX(i + 1, j)] - vx[IX(i - 1, j)]) +
                hy * (vy[IX(i, j + 1)] - vy[IX(i, j - 1)]));
            p[IX(i, j)] = 0.f;
        }
    set_bnd(0, div);
    set_bnd(0, p);

    for (int k = 0; k < ITER; ++k) {
        for (int j = 1; j < g_M - 1; ++j)
            for (int i = 1; i < g_N - 1; ++i)
                p[IX(i, j)] = (div[IX(i, j)] +
                    p[IX(i - 1, j)] + p[IX(i + 1, j)] +
                    p[IX(i, j - 1)] + p[IX(i, j + 1)]) / 4.f;
        set_bnd(0, p);
    }

    for (int j = 1; j < g_M - 1; ++j)
        for (int i = 1; i < g_N - 1; ++i) {
            vx[IX(i, j)] -= 0.5f * (p[IX(i + 1, j)] - p[IX(i - 1, j)]) * (g_N - 2);
            vy[IX(i, j)] -= 0.5f * (p[IX(i, j + 1)] - p[IX(i, j - 1)]) * (g_M - 2);
        }
    set_bnd(1, vx);
    set_bnd(2, vy);
}

static void step() {
    // Velocity step
    std::swap(g_vx, g_vx0);
    diffuse(1, g_vx, g_vx0, g_visc);
    std::swap(g_vy, g_vy0);
    diffuse(2, g_vy, g_vy0, g_visc);
    project(g_vx, g_vy, g_vx0, g_vy0);

    std::swap(g_vx, g_vx0);
    std::swap(g_vy, g_vy0);
    advect(1, g_vx, g_vx0, g_vx0, g_vy0);
    advect(2, g_vy, g_vy0, g_vx0, g_vy0);
    project(g_vx, g_vy, g_vx0, g_vy0);

    // Density step
    std::swap(g_dens, g_dens0);
    diffuse(0, g_dens, g_dens0, g_diff);
    std::swap(g_dens, g_dens0);
    advect(0, g_dens, g_dens0, g_vx, g_vy);

    // Clamp density
    for (auto& d : g_dens) d = std::clamp(d, 0.f, 5.f);
}

// ── Add sources from mouse ──────────────────────────────────────────────────

static void add_source() {
    if (!g_mouse_down || g_mouse_x < 0) return;
    int cx = g_mouse_x;
    int cy = g_mouse_y;
    if (cx < 1 || cx >= g_N - 1 || cy < 1 || cy >= g_M - 1) return;

    // Density burst
    int radius = 3;
    for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx) {
            int nx = cx + dx, ny = cy + dy;
            if (nx < 1 || nx >= g_N - 1 || ny < 1 || ny >= g_M - 1) continue;
            float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy));
            if (dist > radius) continue;
            float strength = (1.f - dist / radius) * 2.f;
            g_dens[IX(nx, ny)] += strength;
        }

    // Velocity from drag direction
    if (g_prev_mx >= 0) {
        float dvx = static_cast<float>(cx - g_prev_mx) * 5.f;
        float dvy = static_cast<float>(cy - g_prev_my) * 5.f;
        for (int dy = -radius; dy <= radius; ++dy)
            for (int dx = -radius; dx <= radius; ++dx) {
                int nx = cx + dx, ny = cy + dy;
                if (nx < 1 || nx >= g_N - 1 || ny < 1 || ny >= g_M - 1) continue;
                g_vx[IX(nx, ny)] += dvx;
                g_vy[IX(nx, ny)] += dvy;
            }
    }
    g_prev_mx = cx;
    g_prev_my = cy;
}

// ── Resize / reset ──────────────────────────────────────────────────────────

static void resize_grid(int W, int H) {
    g_N = std::max(4, W);
    g_M = std::max(4, H * 2);       // double vertical res for half-blocks
    int sz = g_N * g_M;
    g_dens.assign(sz, 0.f);  g_dens0.assign(sz, 0.f);
    g_vx.assign(sz, 0.f);    g_vy.assign(sz, 0.f);
    g_vx0.assign(sz, 0.f);   g_vy0.assign(sz, 0.f);
}

// ── Style interning ─────────────────────────────────────────────────────────

static void rebuild_styles(StylePool& pool) {
    // Build NxN palette for half-block rendering (fg=top, bg=bottom)
    for (int fi = 0; fi < CSTEPS; ++fi) {
        float fd = static_cast<float>(fi) / (CSTEPS - 1) * 5.f;
        RGB fc = density_to_color(fd);
        for (int bi = 0; bi < CSTEPS; ++bi) {
            float bd = static_cast<float>(bi) / (CSTEPS - 1) * 5.f;
            RGB bc = density_to_color(bd);
            g_styles[fi][bi] = pool.intern(
                Style{}.with_fg(Color::rgb(fc.r, fc.g, fc.b))
                       .with_bg(Color::rgb(bc.r, bc.g, bc.b)));
        }
    }
    g_bar_style = pool.intern(
        Style{}.with_fg(Color::rgb(180, 180, 180))
               .with_bg(Color::rgb(30, 30, 30)));
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main() {
    int prev_pal = -1;

    auto on_resize = [&](StylePool& pool, int W, int H) {
        resize_grid(W, H - 1);          // reserve 1 row for status bar
        prev_pal = g_palette;
        rebuild_styles(pool);
    };

    auto on_event = [&](const Event& ev) -> bool {
        if (key(ev, 'q') || key(ev, SpecialKey::Escape)) return false;
        on(ev, ' ', [] { g_paused = !g_paused; });
        on(ev, 'r', [&] { resize_grid(g_N, (g_M / 2)); });
        on(ev, '+', [] { g_visc = std::min(g_visc * 2.f, 0.01f); });
        on(ev, '=', [] { g_visc = std::min(g_visc * 2.f, 0.01f); });
        on(ev, '-', [] { g_visc = std::max(g_visc * 0.5f, 0.00001f); });
        on(ev, '1', [] { g_palette = 0; });
        on(ev, '2', [] { g_palette = 1; });
        on(ev, '3', [] { g_palette = 2; });
        on(ev, '4', [] { g_palette = 3; });
        on(ev, '5', [] { g_palette = 4; });

        // Mouse handling
        if (mouse_clicked(ev)) {
            g_mouse_down = true;
            if (auto p = mouse_pos(ev)) {
                g_mouse_x = p->col;
                g_mouse_y = p->row * 2;     // map to fluid grid (2x vertical)
                g_prev_mx = g_mouse_x;
                g_prev_my = g_mouse_y;
            }
        }
        if (mouse_released(ev)) {
            g_mouse_down = false;
            g_prev_mx = g_prev_my = -1;
        }
        if (mouse_moved(ev) && g_mouse_down) {
            if (auto p = mouse_pos(ev)) {
                g_mouse_x = p->col;
                g_mouse_y = p->row * 2;
            }
        }
        return true;
    };

    auto on_paint = [&](Canvas& canvas, int W, int H) {
        // Palette changed — need to re-intern styles
        if (g_palette != prev_pal) {
            // We can't re-intern here; styles will update on next resize.
            // Instead, just recompute colors live (the style cache is from
            // the current palette at init time). Force a pseudo-resize by
            // marking prev_pal; the on_resize callback handles it.
        }

        if (!g_paused) {
            add_source();
            step();
        }

        int bar_y = H - 1;
        int fluid_h = H - 1;          // terminal rows for the fluid display

        // Paint fluid with half-blocks: each terminal row = 2 fluid rows
        for (int ty = 0; ty < fluid_h; ++ty) {
            int fy_top = ty * 2;
            int fy_bot = ty * 2 + 1;
            for (int x = 0; x < W && x < g_N; ++x) {
                float d_top = (fy_top < g_M) ? g_dens[IX(x, fy_top)] : 0.f;
                float d_bot = (fy_bot < g_M) ? g_dens[IX(x, fy_bot)] : 0.f;
                int fi = static_cast<int>(std::clamp(d_top / 5.f, 0.f, 1.f) * (CSTEPS - 1));
                int bi = static_cast<int>(std::clamp(d_bot / 5.f, 0.f, 1.f) * (CSTEPS - 1));
                canvas.set(x, ty, U'\u2580', g_styles[fi][bi]);  // ▀
            }
        }

        // Status bar
        for (int x = 0; x < W; ++x)
            canvas.set(x, bar_y, U' ', g_bar_style);

        const char* status = " FLUID \xe2\x94\x82 drag=add \xe2\x94\x82 [1-5] palette \xe2\x94\x82 [r] reset \xe2\x94\x82 [+/-] visc \xe2\x94\x82 [spc] pause \xe2\x94\x82 [q] quit";
        canvas.write_text(0, bar_y, status, g_bar_style);

        // Show palette name on the right
        auto& pal = g_palettes[g_palette];
        int name_len = static_cast<int>(std::strlen(pal.name));
        if (W > name_len + 2)
            canvas.write_text(W - name_len - 1, bar_y, pal.name, g_bar_style);
    };

    // We need to re-intern styles when palette changes. We'll use a wrapper
    // that tracks palette changes via the resize callback mechanism.
    int last_W = 0, last_H = 0;
    StylePool* g_pool_ptr = nullptr;

    auto resize_wrapper = [&](StylePool& pool, int W, int H) {
        last_W = W;
        last_H = H;
        g_pool_ptr = &pool;
        on_resize(pool, W, H);
    };

    auto paint_wrapper = [&](Canvas& canvas, int W, int H) {
        // Re-intern styles if palette changed
        if (g_palette != prev_pal && g_pool_ptr) {
            prev_pal = g_palette;
            rebuild_styles(*g_pool_ptr);
        }
        on_paint(canvas, W, H);
    };

    (void)canvas_run(
        CanvasConfig{.fps = 30, .mouse = true, .mode = Mode::Fullscreen, .title = "fluid"},
        resize_wrapper,
        on_event,
        paint_wrapper
    );
}
