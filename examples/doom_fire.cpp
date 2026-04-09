// maya -- Doom PSX fire effect (enhanced)
//
// The classic fire propagation algorithm with enhancements:
//   - 3 color palettes: classic, inferno (purple→blue), toxic (green)
//   - Ember particles that float up and fade
//   - Heat intensity control (+/-)
//   - Half-block rendering for 2x vertical resolution
//
// Keys: q/Esc=quit  space=toggle  ←/→=wind  +/-=intensity  1-3=palette

#include <maya/internal.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace maya;

// -- Constants ---------------------------------------------------------------

static constexpr int MAX_HEAT = 48;
static constexpr int NUM_PALETTES = 3;

// -- State -------------------------------------------------------------------

static std::mt19937 g_rng{42};
static std::vector<uint8_t> g_fire;
static int g_w = 0, g_h = 0;
static bool g_source = true;
static int g_wind = 0;
static int g_palette = 0;
static int g_intensity = 3;  // 1-5 decay rate

// Ember particles
struct Ember {
    float x, y, vx, vy;
    int heat;
    float life;
};
static std::vector<Ember> g_embers;
static float g_time = 0.f;

// Style IDs: half-block styles (fg=top, bg=bottom), plus status bar
static constexpr int Q = 6;   // quantization levels per channel (6³×6³ = 46656 < 65535)
static uint16_t g_fire_styles[Q * Q * Q][Q * Q * Q];  // [fg_idx][bg_idx]
static uint16_t g_bar_bg;
static uint16_t g_bar_dim;
static uint16_t g_bar_accent;

// -- Palettes ----------------------------------------------------------------

struct Palette {
    const char* name;
    // Returns color for a heat level 0..MAX_HEAT
    Color (*color_fn)(int h);
};

static Color classic_color(int h) {
    h = std::clamp(h, 0, MAX_HEAT);
    float t = static_cast<float>(h) / MAX_HEAT;
    if (t < 0.15f) {
        float u = t / 0.15f;
        return Color::rgb(static_cast<uint8_t>(u * 180), 0, 0);
    }
    if (t < 0.4f) {
        float u = (t - 0.15f) / 0.25f;
        return Color::rgb(static_cast<uint8_t>(180 + u * 75), static_cast<uint8_t>(u * 100), 0);
    }
    if (t < 0.7f) {
        float u = (t - 0.4f) / 0.3f;
        return Color::rgb(255, static_cast<uint8_t>(100 + u * 155), 0);
    }
    float u = (t - 0.7f) / 0.3f;
    return Color::rgb(255, 255, static_cast<uint8_t>(u * 255));
}

static Color inferno_color(int h) {
    h = std::clamp(h, 0, MAX_HEAT);
    float t = static_cast<float>(h) / MAX_HEAT;
    // Black → deep purple → magenta → orange → white
    if (t < 0.2f) {
        float u = t / 0.2f;
        return Color::rgb(static_cast<uint8_t>(u * 60), 0, static_cast<uint8_t>(u * 100));
    }
    if (t < 0.45f) {
        float u = (t - 0.2f) / 0.25f;
        return Color::rgb(static_cast<uint8_t>(60 + u * 160), 0, static_cast<uint8_t>(100 + u * 60));
    }
    if (t < 0.7f) {
        float u = (t - 0.45f) / 0.25f;
        return Color::rgb(static_cast<uint8_t>(220 + u * 35), static_cast<uint8_t>(u * 140),
                          static_cast<uint8_t>(160 - u * 160));
    }
    float u = (t - 0.7f) / 0.3f;
    return Color::rgb(255, static_cast<uint8_t>(140 + u * 115), static_cast<uint8_t>(u * 200));
}

static Color toxic_color(int h) {
    h = std::clamp(h, 0, MAX_HEAT);
    float t = static_cast<float>(h) / MAX_HEAT;
    // Black → dark green → bright green → yellow → white
    if (t < 0.2f) {
        float u = t / 0.2f;
        return Color::rgb(0, static_cast<uint8_t>(u * 60), 0);
    }
    if (t < 0.5f) {
        float u = (t - 0.2f) / 0.3f;
        return Color::rgb(0, static_cast<uint8_t>(60 + u * 195), static_cast<uint8_t>(u * 30));
    }
    if (t < 0.75f) {
        float u = (t - 0.5f) / 0.25f;
        return Color::rgb(static_cast<uint8_t>(u * 200), 255, static_cast<uint8_t>(30 - u * 30));
    }
    float u = (t - 0.75f) / 0.25f;
    return Color::rgb(static_cast<uint8_t>(200 + u * 55), 255, static_cast<uint8_t>(u * 255));
}

static const Palette g_palettes[NUM_PALETTES] = {
    {"CLASSIC", classic_color},
    {"INFERNO", inferno_color},
    {"TOXIC",   toxic_color},
};

// -- Quantization for half-block rendering ------------------------------------

static int to_idx(uint8_t r, uint8_t g, uint8_t b) {
    return (r * Q / 256) * Q * Q + (g * Q / 256) * Q + (b * Q / 256);
}

static uint8_t to8(int level) {
    return static_cast<uint8_t>(level * 255 / (Q - 1));
}

// -- Resize ------------------------------------------------------------------

static void rebuild(StylePool& pool, int w, int h) {
    g_w = w;
    g_h = h;
    int fire_h = h * 2;  // half-block = 2x resolution
    g_fire.assign(static_cast<size_t>(w * fire_h), 0);

    if (g_source) {
        for (int x = 0; x < w; ++x)
            g_fire[static_cast<size_t>((fire_h - 1) * w + x)] = MAX_HEAT;
    }

    // Intern all fg/bg color combinations
    for (int fi = 0; fi < Q * Q * Q; ++fi) {
        int fr = fi / (Q * Q), fg = (fi / Q) % Q, fb = fi % Q;
        for (int bi = 0; bi < Q * Q * Q; ++bi) {
            int br = bi / (Q * Q), bg = (bi / Q) % Q, bb = bi % Q;
            g_fire_styles[fi][bi] = pool.intern(
                Style{}.with_fg(Color::rgb(to8(fr), to8(fg), to8(fb)))
                       .with_bg(Color::rgb(to8(br), to8(bg), to8(bb))));
        }
    }

    g_bar_bg     = pool.intern(Style{}.with_bg(Color::rgb(20, 20, 20)).with_fg(Color::rgb(120, 120, 120)));
    g_bar_dim    = pool.intern(Style{}.with_bg(Color::rgb(20, 20, 20)).with_fg(Color::rgb(100, 100, 110)));
    g_bar_accent = pool.intern(Style{}.with_bg(Color::rgb(20, 20, 20)).with_fg(Color::rgb(255, 140, 40)).with_bold());
}

// -- Event -------------------------------------------------------------------

static bool handle(const Event& ev) {
    if (key(ev, 'q') || key(ev, SpecialKey::Escape)) return false;

    on(ev, ' ', [] {
        g_source = !g_source;
        int fire_h = g_h * 2;
        if (!g_source) {
            for (int x = 0; x < g_w; ++x)
                g_fire[static_cast<size_t>((fire_h - 1) * g_w + x)] = 0;
        } else {
            for (int x = 0; x < g_w; ++x)
                g_fire[static_cast<size_t>((fire_h - 1) * g_w + x)] = MAX_HEAT;
        }
    });

    on(ev, SpecialKey::Right, [] { g_wind = std::min(g_wind + 1, 5); });
    on(ev, SpecialKey::Left,  [] { g_wind = std::max(g_wind - 1, -5); });
    on(ev, '+', [] { g_intensity = std::min(g_intensity + 1, 5); });
    on(ev, '=', [] { g_intensity = std::min(g_intensity + 1, 5); });
    on(ev, '-', [] { g_intensity = std::max(g_intensity - 1, 1); });
    on(ev, '1', [] { g_palette = 0; });
    on(ev, '2', [] { g_palette = 1; });
    on(ev, '3', [] { g_palette = 2; });

    return true;
}

// -- Paint -------------------------------------------------------------------

static void paint(Canvas& canvas, int w, int h) {
    if (w != g_w || h != g_h) return;
    g_time += 1.f / 60.f;

    int fire_h = h * 2;  // pixel height in half-blocks

    // Propagate fire upward with intensity-dependent decay
    std::uniform_int_distribution<int> decay_dist(0, g_intensity);
    std::uniform_int_distribution<int> wind_dist(0, std::abs(g_wind));
    std::uniform_int_distribution<int> spread_dist(-1, 1);

    for (int y = 0; y < fire_h - 1; ++y) {
        for (int x = 0; x < w; ++x) {
            int src_y = y + 1;
            int wind_offset = (g_wind == 0) ? 0
                : (g_wind > 0 ? wind_dist(g_rng) : -wind_dist(g_rng));
            int src_x = std::clamp(x + wind_offset + spread_dist(g_rng), 0, w - 1);

            int decay = decay_dist(g_rng);
            int heat = g_fire[static_cast<size_t>(src_y * w + src_x)] - decay;
            if (heat < 0) heat = 0;

            g_fire[static_cast<size_t>(y * w + x)] = static_cast<uint8_t>(heat);
        }
    }

    // Maintain source row
    if (g_source) {
        for (int x = 0; x < w; ++x)
            g_fire[static_cast<size_t>((fire_h - 1) * w + x)] = MAX_HEAT;
    }

    // Spawn ember particles from high-heat areas
    if (g_source && g_rng() % 3 == 0) {
        int ex = static_cast<int>(g_rng() % static_cast<unsigned>(w));
        int ey_row = fire_h / 3 + static_cast<int>(g_rng() % static_cast<unsigned>(fire_h / 3));
        int heat = g_fire[static_cast<size_t>(ey_row * w + ex)];
        if (heat > MAX_HEAT / 2) {
            float fex = static_cast<float>(ex);
            float fey = static_cast<float>(ey_row) / 2.f;
            float vx = (static_cast<float>(g_rng() % 100) - 50.f) / 200.f + g_wind * 0.05f;
            float vy = -(static_cast<float>(g_rng() % 100) + 30.f) / 200.f;
            g_embers.push_back({fex, fey, vx, vy, heat, 1.0f});
        }
    }

    // Update embers
    for (auto& e : g_embers) {
        e.x += e.vx;
        e.y += e.vy;
        e.vy -= 0.003f;  // slight upward acceleration (buoyancy)
        e.life -= 0.02f;
        e.heat = static_cast<int>(static_cast<float>(e.heat) * 0.97f);
    }
    std::erase_if(g_embers, [](const Ember& e) {
        return e.life <= 0.f || e.heat <= 1;
    });

    // Render fire with half-block characters
    auto color_fn = g_palettes[g_palette].color_fn;
    int canvas_h = h - 1;  // reserve last row for status bar

    for (int cy = 0; cy < canvas_h; ++cy) {
        for (int cx = 0; cx < w; ++cx) {
            int py_top = cy * 2;
            int py_bot = cy * 2 + 1;
            int heat_top = g_fire[static_cast<size_t>(py_top * w + cx)];
            int heat_bot = g_fire[static_cast<size_t>(py_bot * w + cx)];

            Color c_top = color_fn(heat_top);
            Color c_bot = color_fn(heat_bot);

            int fi = to_idx(c_top.r(), c_top.g(), c_top.b());
            int bi = to_idx(c_bot.r(), c_bot.g(), c_bot.b());
            canvas.set(cx, cy, U'\u2580', g_fire_styles[fi][bi]);
        }
    }

    // Render embers on top
    for (const auto& e : g_embers) {
        int ex = static_cast<int>(e.x);
        int ey = static_cast<int>(e.y);
        if (ex >= 0 && ex < w && ey >= 0 && ey < canvas_h) {
            Color c = color_fn(e.heat);
            int fi = to_idx(c.r(), c.g(), c.b());
            // Ember appears as a bright dot using the upper half-block
            canvas.set(ex, ey, U'\u2580', g_fire_styles[fi][fi]);
        }
    }

    // Status bar
    int bar_y = h - 1;
    for (int x = 0; x < w; ++x)
        canvas.set(x, bar_y, U' ', g_bar_bg);

    const char* bar = "DOOM FIRE \xe2\x94\x82 [1-3] palette \xe2\x94\x82 [+/-] heat \xe2\x94\x82 [\xe2\x86\x90\xe2\x86\x92] wind \xe2\x94\x82 [space] toggle \xe2\x94\x82 [q] quit";
    canvas.write_text(1, bar_y, bar, g_bar_dim);
    canvas.write_text(1, bar_y, "DOOM FIRE", g_bar_accent);

    // Right side: palette name + stats
    char rbuf[64];
    std::snprintf(rbuf, sizeof(rbuf), "%s h:%d w:%+d e:%d",
        g_palettes[g_palette].name, g_intensity, g_wind,
        static_cast<int>(g_embers.size()));
    int rlen = static_cast<int>(std::strlen(rbuf));
    if (w > rlen + 2)
        canvas.write_text(w - rlen - 1, bar_y, rbuf, g_bar_accent);
}

// -- Main --------------------------------------------------------------------

int main() {
    (void)canvas_run(
        CanvasConfig{.fps = 60, .auto_clear = false, .title = "doom fire"},
        rebuild,
        handle,
        paint
    );
}
