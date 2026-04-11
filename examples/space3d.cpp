// maya — TERRAIN: 3D flight over raymarched terrain
//
// Per-pixel raymarched heightmap terrain with water reflections, ambient
// occlusion, soft shadows, procedural erosion-like texturing, golden hour
// sky with atmospheric scattering, and multi-threaded rendering.
//
// Keys: WASD/arrows=steer  space=ascend  c=descend  b=boost  q/Esc=quit

#include <maya/internal.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using namespace maya;

// ── Math ────────────────────────────────────────────────────────────────────

static constexpr float PI  = 3.14159265f;
static constexpr float TAU = 6.28318530f;

static float clampf(float x, float lo, float hi) { return std::fmin(std::fmax(x, lo), hi); }
static float lerp(float a, float b, float t) { return a + (b - a) * t; }
static float smoothstep(float e0, float e1, float x) {
    float t = clampf((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

struct v3 {
    float x, y, z;
    v3 operator+(v3 b) const { return {x+b.x, y+b.y, z+b.z}; }
    v3 operator-(v3 b) const { return {x-b.x, y-b.y, z-b.z}; }
    v3 operator*(float s) const { return {x*s, y*s, z*s}; }
    v3 operator*(v3 b) const { return {x*b.x, y*b.y, z*b.z}; }
    v3 operator-() const { return {-x,-y,-z}; }
};
static float dot(v3 a, v3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static float len(v3 v) { return std::sqrt(dot(v,v)); }
static v3 normalize(v3 v) { float l = len(v); return v * (1.f/(l+1e-9f)); }
static v3 reflect(v3 i, v3 n) { return i - n * (2.f * dot(i, n)); }

struct Col3 { float r, g, b; };
static Col3 operator+(Col3 a, Col3 b) { return {a.r+b.r, a.g+b.g, a.b+b.b}; }
static Col3 operator*(Col3 a, float s) { return {a.r*s, a.g*s, a.b*s}; }
static Col3 operator*(Col3 a, Col3 b) { return {a.r*b.r, a.g*b.g, a.b*b.b}; }
static Col3 col_lerp(Col3 a, Col3 b, float t) { return {lerp(a.r,b.r,t), lerp(a.g,b.g,t), lerp(a.b,b.b,t)}; }
static Col3 col_clamp(Col3 c) { return {clampf(c.r,0,1), clampf(c.g,0,1), clampf(c.b,0,1)}; }

// ── Noise ───────────────────────────────────────────────────────────────────

static float hash(float x, float y) {
    float h = std::sin(x * 127.1f + y * 311.7f) * 43758.5453f;
    return h - std::floor(h);
}

static float value_noise(float x, float y) {
    float ix = std::floor(x), iy = std::floor(y);
    float fx = x - ix, fy = y - iy;
    fx = fx * fx * (3.f - 2.f * fx);
    fy = fy * fy * (3.f - 2.f * fy);
    float a = hash(ix, iy), b = hash(ix+1, iy);
    float c = hash(ix, iy+1), d = hash(ix+1, iy+1);
    return lerp(lerp(a,b,fx), lerp(c,d,fx), fy);
}

static float fbm(float x, float y, int octaves) {
    float sum = 0.f, amp = 0.5f, freq = 1.f;
    for (int i = 0; i < octaves; ++i) {
        sum += value_noise(x * freq, y * freq) * amp;
        freq *= 2.03f;
        amp *= 0.48f;
    }
    return sum;
}

// Ridged noise — gives sharp mountain crests like real erosion
static float ridged_noise(float x, float y, int octaves) {
    float sum = 0.f, amp = 0.6f, freq = 1.f, prev = 1.f;
    for (int i = 0; i < octaves; ++i) {
        float n = value_noise(x * freq, y * freq);
        n = 1.f - std::fabs(n * 2.f - 1.f); // fold to ridge
        n = n * n;                            // sharpen
        sum += n * amp * prev;
        prev = n;
        freq *= 2.1f;
        amp *= 0.5f;
    }
    return sum;
}

// ── Terrain ─────────────────────────────────────────────────────────────────

static float terrain_height(float x, float z) {
    // Continental-scale: broad rolling base
    float base = fbm(x * 0.003f, z * 0.003f, 4) * 40.f - 10.f;

    // Mountain ranges: ridged noise for sharp crests
    float mountain_mask = smoothstep(0.35f, 0.65f, value_noise(x * 0.002f, z * 0.002f));
    float ridges = ridged_noise(x * 0.008f + 3.7f, z * 0.008f + 1.3f, 5) * 55.f;
    float h = base + ridges * mountain_mask;

    // Medium detail: eroded gullies
    float gully = value_noise(x * 0.025f, z * 0.025f);
    gully = smoothstep(0.4f, 0.6f, gully);
    h += (gully - 0.5f) * 6.f;

    // Fine detail
    h += fbm(x * 0.05f, z * 0.05f, 3) * 2.5f;

    // Flatten near water for beaches/lowlands
    if (h < 3.f) h = lerp(h, -2.f, smoothstep(3.f, -5.f, h));

    return h;
}

// Terrain normal — fine epsilon for sharp normals
static v3 terrain_normal(float x, float z) {
    constexpr float e = 0.15f;
    float hc = terrain_height(x, z);
    float hx = terrain_height(x + e, z);
    float hz = terrain_height(x, z + e);
    return normalize(v3{-(hx - hc) / e, 1.f, -(hz - hc) / e});
}

// ── Sky model ───────────────────────────────────────────────────────────────

// Golden hour sun — higher and warmer
static v3 g_sun_dir = normalize(v3{0.5f, 0.28f, -0.7f});
static Col3 g_sun_color = {1.6f, 1.15f, 0.7f};

static Col3 sky_color(v3 rd) {
    float y = rd.y;

    // Realistic atmosphere: Rayleigh-like scattering
    // Deep blue overhead, warm golden at horizon
    Col3 zenith   = {0.15f, 0.22f, 0.55f};
    Col3 mid_sky  = {0.30f, 0.38f, 0.68f};
    Col3 horizon  = {0.75f, 0.52f, 0.30f};
    Col3 low_hori = {0.90f, 0.50f, 0.18f};

    Col3 sky;
    if (y > 0.5f)       sky = col_lerp(mid_sky, zenith, (y - 0.5f) / 0.5f);
    else if (y > 0.12f) sky = col_lerp(horizon, mid_sky, (y - 0.12f) / 0.38f);
    else if (y > 0.f)   sky = col_lerp(low_hori, horizon, y / 0.12f);
    else                 sky = low_hori;

    // Mie scattering: large warm glow around sun
    float sun_dot = clampf(dot(rd, g_sun_dir), 0.f, 1.f);
    float mie = std::pow(sun_dot, 5.f);
    sky = sky + Col3{mie * 0.5f, mie * 0.30f, mie * 0.08f};

    // Tighter Mie peak
    float mie2 = std::pow(sun_dot, 24.f);
    sky = sky + Col3{mie2 * 0.4f, mie2 * 0.22f, mie2 * 0.05f};

    // Sun disk (layered for natural falloff)
    float disk = std::pow(sun_dot, 800.f);
    sky = sky + Col3{disk * 4.f, disk * 3.2f, disk * 2.f};
    float corona = std::pow(sun_dot, 128.f);
    sky = sky + Col3{corona * 0.6f, corona * 0.35f, corona * 0.12f};

    // Stars in deep blue overhead
    if (y > 0.35f) {
        float su = std::atan2(rd.x, rd.z) * 80.f;
        float sv = rd.y * 100.f;
        float star = hash(std::floor(su), std::floor(sv));
        if (star > 0.988f) {
            float bright = (star - 0.988f) * 70.f * smoothstep(0.35f, 0.65f, y);
            sky = sky + Col3{bright, bright, bright * 0.9f};
        }
    }

    // Thin high clouds
    if (y > 0.02f && y < 0.4f) {
        float cu = std::atan2(rd.x, rd.z) * 2.5f;
        float cv = y * 8.f;
        float cloud = fbm(cu + 0.3f, cv + 1.7f, 3);
        cloud = smoothstep(0.42f, 0.62f, cloud);
        Col3 cloud_col = col_lerp(Col3{0.9f, 0.75f, 0.55f}, Col3{1.f, 0.9f, 0.8f}, y * 3.f);
        sky = col_lerp(sky, cloud_col, cloud * 0.25f);
    }

    return col_clamp(sky);
}

// ── Atmosphere / fog ────────────────────────────────────────────────────────

static Col3 apply_fog(Col3 color, float dist, v3 rd) {
    // Exponential height fog: thicker at low altitude
    float fog_amount = 1.f - std::exp(-dist * 0.0008f);
    fog_amount = clampf(fog_amount, 0.f, 1.f);

    // Inscattered light: fog is lit by sun
    float sun_factor = clampf(dot(rd, g_sun_dir) * 0.5f + 0.5f, 0.f, 1.f);
    sun_factor = sun_factor * sun_factor;

    Col3 fog_col = col_lerp(
        Col3{0.42f, 0.45f, 0.58f},   // shadow fog (blue-ish)
        Col3{0.72f, 0.52f, 0.30f},   // sunlit fog (golden)
        sun_factor
    );

    // Extra inscattering near sun direction
    float inscatter = std::pow(clampf(dot(rd, g_sun_dir), 0, 1), 8.f);
    fog_col = fog_col + Col3{inscatter * 0.15f, inscatter * 0.08f, inscatter * 0.02f};

    return col_lerp(color, fog_col, fog_amount);
}

// ── Style interning ─────────────────────────────────────────────────────────

static constexpr int Q = 6;
static uint16_t g_styles[Q*Q*Q][Q*Q*Q];

static int to_idx(uint8_t r, uint8_t g, uint8_t b) {
    return (r*Q/256)*Q*Q + (g*Q/256)*Q + (b*Q/256);
}
static uint8_t to8(int level) { return static_cast<uint8_t>(level * 255 / (Q-1)); }

static uint16_t g_bar_bg, g_bar_dim, g_bar_accent, g_bar_alt, g_bar_speed;

// ── Pixel buffer ────────────────────────────────────────────────────────────

static int g_w = 0, g_h = 0;
struct Pixel { uint8_t r, g, b; };
static std::vector<Pixel> g_pixels;
static int g_pixel_w = 0, g_pixel_h = 0;

static void px_set_col(int x, int y, Col3 c) {
    if (x >= 0 && x < g_pixel_w && y >= 0 && y < g_pixel_h) {
        c = col_clamp(c);
        g_pixels[static_cast<size_t>(y * g_pixel_w + x)] = {
            static_cast<uint8_t>(c.r*255.f),
            static_cast<uint8_t>(c.g*255.f),
            static_cast<uint8_t>(c.b*255.f)};
    }
}

static Pixel px_get(int x, int y) {
    if (x >= 0 && x < g_pixel_w && y >= 0 && y < g_pixel_h)
        return g_pixels[static_cast<size_t>(y * g_pixel_w + x)];
    return {0,0,0};
}

// ── Rebuild ─────────────────────────────────────────────────────────────────

static void rebuild(StylePool& pool, int w, int h) {
    g_w = w; g_h = h;
    for (int fi = 0; fi < Q*Q*Q; ++fi) {
        int fr = fi/(Q*Q), fg = (fi/Q)%Q, fb = fi%Q;
        for (int bi = 0; bi < Q*Q*Q; ++bi) {
            int br = bi/(Q*Q), bg = (bi/Q)%Q, bb = bi%Q;
            g_styles[fi][bi] = pool.intern(
                Style{}.with_fg(Color::rgb(to8(fr), to8(fg), to8(fb)))
                       .with_bg(Color::rgb(to8(br), to8(bg), to8(bb))));
        }
    }
    g_bar_bg    = pool.intern(Style{}.with_bg(Color::rgb(10,8,15)).with_fg(Color::rgb(100,90,110)));
    g_bar_dim   = pool.intern(Style{}.with_bg(Color::rgb(10,8,15)).with_fg(Color::rgb(70,65,80)));
    g_bar_accent= pool.intern(Style{}.with_bg(Color::rgb(10,8,15)).with_fg(Color::rgb(255,180,60)).with_bold());
    g_bar_alt   = pool.intern(Style{}.with_bg(Color::rgb(10,8,15)).with_fg(Color::rgb(80,200,255)).with_bold());
    g_bar_speed = pool.intern(Style{}.with_bg(Color::rgb(10,8,15)).with_fg(Color::rgb(120,255,120)).with_bold());

    g_pixel_w = w;
    g_pixel_h = (h - 1) * 2;
    g_pixels.assign(static_cast<size_t>(g_pixel_w * g_pixel_h), Pixel{0,0,0});
}

// ── Composite ───────────────────────────────────────────────────────────────

static void composite(Canvas& canvas, int canvas_h) {
    for (int cy = 0; cy < canvas_h; ++cy) {
        int y_top = cy*2, y_bot = cy*2+1;
        for (int cx = 0; cx < g_pixel_w; ++cx) {
            Pixel top = px_get(cx, y_top);
            Pixel bot = px_get(cx, y_bot);
            canvas.set(cx, cy, U'\u2580',
                       g_styles[to_idx(top.r,top.g,top.b)][to_idx(bot.r,bot.g,bot.b)]);
        }
    }
}

// ── Camera / player state ───────────────────────────────────────────────────

static constexpr float WATER_LEVEL = 0.0f;
static constexpr float MIN_HEIGHT  = 4.f;
static constexpr float CAM_PITCH_RANGE = 0.35f;

static v3    g_cam_pos   = {0, 35, 0};
static float g_cam_yaw   = 0.f;
static float g_cam_pitch = -0.06f;
static float g_speed     = 40.f;
static float g_boost     = 0.f;
static int   g_frame     = 0;
static float g_time      = 0.f;
static float g_dist      = 0.f;
static int   g_score     = 0;

static constexpr float BASE_SPEED  = 40.f;
static constexpr float BOOST_SPEED = 120.f;
static constexpr float STEER_RATE  = 1.8f;
static constexpr float PITCH_RATE  = 0.8f;
static constexpr float VERT_RATE   = 18.f;

// Input
static constexpr int HOLD_FRAMES = 5;
enum KeyAction { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_ASCEND, K_DESCEND, K_BOOST, K_COUNT };
static int g_key_last[K_COUNT] = {-100,-100,-100,-100,-100,-100,-100};

// Rings
struct Ring { v3 pos; float radius; bool collected; };
static std::vector<Ring> g_rings;

// ── Ring spawning ───────────────────────────────────────────────────────────

static void spawn_rings_ahead() {
    float ahead_z = g_cam_pos.z - 200.f;
    for (int i = 0; i < 5; ++i) {
        float rz = ahead_z - i * 60.f;
        bool exists = false;
        for (auto& r : g_rings) {
            if (std::fabs(r.pos.z - rz) < 30.f) { exists = true; break; }
        }
        if (exists) continue;
        float rx = g_cam_pos.x + (hash(rz * 0.1f, 0.f) - 0.5f) * 80.f;
        float th = terrain_height(rx, rz);
        float ry = std::fmax(th, WATER_LEVEL) + 12.f + hash(rz * 0.1f, 1.f) * 15.f;
        g_rings.push_back({v3{rx, ry, rz}, 5.f, false});
    }
    g_rings.erase(std::remove_if(g_rings.begin(), g_rings.end(),
        [&](const Ring& r) { return r.pos.z > g_cam_pos.z + 50.f; }), g_rings.end());
}

// ── Reset / tick ────────────────────────────────────────────────────────────

static void reset_game() {
    g_cam_pos = {0, 35, 0};
    g_cam_yaw = 0.f; g_cam_pitch = -0.06f;
    g_speed = BASE_SPEED; g_boost = 0.f;
    g_frame = 0; g_time = 0.f; g_dist = 0.f; g_score = 0;
    g_rings.clear();
}

static void game_tick(float dt) {
    g_frame++;
    g_time += dt;

    auto held = [](KeyAction k) { return (g_frame - g_key_last[k]) < HOLD_FRAMES; };
    float turn  = (held(K_RIGHT) ? 1.f : 0.f) - (held(K_LEFT)  ? 1.f : 0.f);
    float pitch = (held(K_UP)    ? 1.f : 0.f) - (held(K_DOWN)  ? 1.f : 0.f);
    float vert  = (held(K_ASCEND)? 1.f : 0.f) - (held(K_DESCEND)? 1.f : 0.f);
    bool boosting = held(K_BOOST);

    g_cam_yaw += turn * STEER_RATE * dt;
    g_cam_pitch += pitch * PITCH_RATE * dt;
    g_cam_pitch = clampf(g_cam_pitch, -CAM_PITCH_RANGE, CAM_PITCH_RANGE);

    float target_speed = boosting ? BOOST_SPEED : BASE_SPEED;
    g_speed = lerp(g_speed, target_speed, 3.f * dt);
    g_boost = lerp(g_boost, boosting ? 1.f : 0.f, 4.f * dt);

    float fwd_x = std::sin(g_cam_yaw), fwd_z = -std::cos(g_cam_yaw);
    g_cam_pos.x += fwd_x * g_speed * dt;
    g_cam_pos.z += fwd_z * g_speed * dt;
    g_cam_pos.y += vert * VERT_RATE * dt;

    float ground = terrain_height(g_cam_pos.x, g_cam_pos.z);
    float min_y = std::fmax(ground, WATER_LEVEL) + MIN_HEIGHT;
    if (g_cam_pos.y < min_y) g_cam_pos.y = lerp(g_cam_pos.y, min_y, 8.f * dt);
    g_cam_pos.y = clampf(g_cam_pos.y, min_y, 120.f);

    g_dist += g_speed * dt;

    spawn_rings_ahead();
    for (auto& r : g_rings) {
        if (r.collected) continue;
        if (len(g_cam_pos - r.pos) < r.radius + 3.f) { r.collected = true; g_score += 100; }
    }
}

// ── Events ──────────────────────────────────────────────────────────────────

static bool handle(const Event& ev) {
    if (key(ev, 'q') || key(ev, SpecialKey::Escape)) return false;
    on(ev, 'r', [] { reset_game(); });

    on(ev, 'w',                [] { g_key_last[K_UP]      = g_frame; g_key_last[K_DOWN]    = -100; });
    on(ev, 's',                [] { g_key_last[K_DOWN]    = g_frame; g_key_last[K_UP]      = -100; });
    on(ev, 'a',                [] { g_key_last[K_LEFT]    = g_frame; g_key_last[K_RIGHT]   = -100; });
    on(ev, 'd',                [] { g_key_last[K_RIGHT]   = g_frame; g_key_last[K_LEFT]    = -100; });
    on(ev, SpecialKey::Up,     [] { g_key_last[K_UP]      = g_frame; g_key_last[K_DOWN]    = -100; });
    on(ev, SpecialKey::Down,   [] { g_key_last[K_DOWN]    = g_frame; g_key_last[K_UP]      = -100; });
    on(ev, SpecialKey::Left,   [] { g_key_last[K_LEFT]    = g_frame; g_key_last[K_RIGHT]   = -100; });
    on(ev, SpecialKey::Right,  [] { g_key_last[K_RIGHT]   = g_frame; g_key_last[K_LEFT]    = -100; });
    on(ev, ' ',                [] { g_key_last[K_ASCEND]  = g_frame; });
    on(ev, 'c',                [] { g_key_last[K_DESCEND] = g_frame; });
    on(ev, 'b',                [] { g_key_last[K_BOOST]   = g_frame; });

    return true;
}

// ── Soft shadow ─────────────────────────────────────────────────────────────

// March toward sun from surface point; return 0 (fully shadowed) to 1 (lit)
static float soft_shadow(v3 pos) {
    float res = 1.f;
    float t = 1.0f;
    for (int i = 0; i < 32 && t < 80.f; ++i) {
        v3 p = pos + g_sun_dir * t;
        float h = terrain_height(p.x, p.z);
        float diff = p.y - h;
        if (diff < 0.1f) return 0.f;
        // Soft penumbra
        res = std::fmin(res, 8.f * diff / t);
        t += clampf(diff * 0.5f, 0.5f, 6.f);
    }
    return clampf(res, 0.f, 1.f);
}

// ── Ambient occlusion ───────────────────────────────────────────────────────

static float ambient_occlusion(v3 pos, v3 normal) {
    float ao = 0.f;
    float scale = 1.f;
    for (int i = 1; i <= 4; ++i) {
        float dist = static_cast<float>(i) * 1.5f;
        v3 p = pos + normal * dist;
        float h = terrain_height(p.x, p.z);
        float diff = p.y - h;
        ao += (dist - clampf(diff, 0.f, dist)) * scale;
        scale *= 0.55f;
    }
    return clampf(1.f - ao * 0.15f, 0.f, 1.f);
}

// ── Terrain shading ─────────────────────────────────────────────────────────

static Col3 terrain_shade(v3 pos, v3 normal, v3 rd) {
    float h = pos.y;
    float slope = 1.f - normal.y;

    // ── Material: altitude + slope-based biome ──

    Col3 c;
    if (h < 0.8f) {
        // Wet sand near water
        float wet = smoothstep(0.8f, -0.5f, h);
        c = col_lerp(Col3{0.60f, 0.52f, 0.36f}, Col3{0.35f, 0.30f, 0.22f}, wet);
    } else if (h < 6.f) {
        // Lush grass lowlands
        float t = (h - 0.8f) / 5.2f;
        Col3 grass1 = {0.22f, 0.38f, 0.12f};
        Col3 grass2 = {0.18f, 0.32f, 0.09f};
        c = col_lerp(grass1, grass2, t);
        // Patches of different grass
        float patch = value_noise(pos.x * 0.08f, pos.z * 0.08f);
        Col3 alt_grass = {0.28f, 0.40f, 0.15f};
        c = col_lerp(c, alt_grass, smoothstep(0.4f, 0.6f, patch) * 0.4f);
    } else if (h < 18.f) {
        // Forest — dark greens with brown undergrowth
        float t = (h - 6.f) / 12.f;
        Col3 forest = {0.14f, 0.25f, 0.08f};
        Col3 sparse = {0.24f, 0.28f, 0.14f};
        c = col_lerp(forest, sparse, t);
        // Tree-scale variation
        float trees = value_noise(pos.x * 0.2f, pos.z * 0.2f);
        c = c * (0.75f + trees * 0.5f);
    } else if (h < 32.f) {
        // Alpine: rock + sparse vegetation
        float t = (h - 18.f) / 14.f;
        Col3 rock_low  = {0.38f, 0.34f, 0.28f};
        Col3 rock_high = {0.50f, 0.47f, 0.43f};
        c = col_lerp(rock_low, rock_high, t);
        // Lichen patches at lower end
        if (t < 0.4f) {
            float lichen = value_noise(pos.x * 0.15f + 7.f, pos.z * 0.15f + 3.f);
            if (lichen > 0.5f) c = col_lerp(c, Col3{0.30f, 0.35f, 0.18f}, (lichen - 0.5f) * 0.6f);
        }
    } else {
        // Snow: bright with blue shadows
        float t = clampf((h - 32.f) / 12.f, 0, 1);
        c = col_lerp(Col3{0.52f, 0.50f, 0.48f}, Col3{0.92f, 0.93f, 0.96f}, t);
    }

    // Steep cliffs override to exposed rock
    if (slope > 0.25f) {
        float rock_t = smoothstep(0.25f, 0.55f, slope);
        float rock_var = value_noise(pos.x * 0.12f + 5.f, pos.z * 0.12f + 9.f);
        Col3 cliff = {0.38f + rock_var * 0.08f, 0.34f + rock_var * 0.06f, 0.28f + rock_var * 0.05f};
        // Stratified layers in cliff face
        float layers = std::sin(pos.y * 1.2f + value_noise(pos.x * 0.05f, pos.z * 0.05f) * 3.f);
        cliff = cliff * (0.85f + layers * 0.15f);
        c = col_lerp(c, cliff, rock_t);
    }

    // Fine-grain surface noise (dirt, pebbles, grass blades)
    float micro = value_noise(pos.x * 0.5f, pos.z * 0.5f);
    c = c * (0.9f + micro * 0.2f);

    // ── Lighting ──

    float ndotl = clampf(dot(normal, g_sun_dir), 0.f, 1.f);

    // Compute shadow
    float shadow = soft_shadow(pos + normal * 0.3f);

    // Ambient occlusion
    float ao = ambient_occlusion(pos, normal);

    // Sky ambient: hemisphere sampling approximation
    Col3 sky_ambient = {0.20f, 0.25f, 0.40f};    // blue sky from above
    Col3 ground_bounce = {0.06f, 0.05f, 0.03f};   // warm bounce from ground
    float sky_factor = normal.y * 0.5f + 0.5f;
    Col3 ambient = col_lerp(ground_bounce, sky_ambient, sky_factor) * ao;

    // Direct sun
    Col3 direct = g_sun_color * (ndotl * shadow);

    Col3 lit = c * (ambient + direct);

    // Specular for wet surfaces (near water) and snow
    if (h < 2.f || h > 35.f) {
        v3 half_v = normalize(g_sun_dir - rd);
        float spec = std::pow(clampf(dot(normal, half_v), 0, 1), h < 2.f ? 32.f : 16.f);
        spec *= shadow;
        float spec_strength = h < 2.f ? 0.2f : 0.08f;
        lit = lit + Col3{spec * spec_strength, spec * spec_strength * 0.9f, spec * spec_strength * 0.7f};
    }

    return col_clamp(lit);
}

// ── Water shading ───────────────────────────────────────────────────────────

static Col3 water_shade(v3 pos, v3 rd, float dist) {
    // Multi-octave animated waves
    float t = g_time;
    float w1 = std::sin(pos.x * 0.25f + t * 1.0f) * std::cos(pos.z * 0.18f + t * 0.7f);
    float w2 = std::sin(pos.x * 0.6f - t * 0.5f + 1.f) * std::cos(pos.z * 0.45f + t * 1.3f);
    float w3 = std::sin(pos.x * 1.1f + t * 0.8f + 3.f) * std::cos(pos.z * 0.9f - t * 0.4f);
    float wave = w1 * 0.5f + w2 * 0.3f + w3 * 0.2f;

    v3 water_n = normalize(v3{
        wave * 0.12f + std::cos(pos.x * 0.4f + t * 0.9f) * 0.04f,
        1.f,
        wave * 0.10f + std::sin(pos.z * 0.35f + t * 0.6f) * 0.04f
    });

    // Fresnel (Schlick)
    float cos_i = clampf(std::fabs(dot(rd, water_n)), 0, 1);
    float fresnel = 0.02f + 0.98f * std::pow(1.f - cos_i, 5.f);

    // Reflected sky
    v3 refl = reflect(rd, water_n);
    if (refl.y < 0.01f) refl.y = 0.01f;
    refl = normalize(refl);
    Col3 refl_col = sky_color(refl);

    // Deep water tint (varies with depth/distance)
    Col3 deep = {0.01f, 0.04f, 0.09f};
    float shallow = smoothstep(20.f, 2.f, dist);
    Col3 shallow_col = {0.04f, 0.12f, 0.15f};
    Col3 water_body = col_lerp(deep, shallow_col, shallow);

    // Sun specular (sharp)
    float spec = std::pow(clampf(dot(refl, g_sun_dir), 0, 1), 512.f);
    Col3 sun_spec = Col3{1.5f, 1.2f, 0.8f} * spec;

    // Broad specular
    float broad = std::pow(clampf(dot(refl, g_sun_dir), 0, 1), 12.f);
    Col3 broad_col = Col3{0.45f, 0.32f, 0.15f} * broad * 0.25f;

    Col3 water = col_lerp(water_body, refl_col, fresnel) + sun_spec + broad_col;

    // Sun glitter path
    float sun_path = std::pow(clampf(dot(normalize(v3{rd.x, 0, rd.z}),
                                         normalize(v3{g_sun_dir.x, 0, g_sun_dir.z})), 0, 1), 3.f);
    float glitter = sun_path * (0.5f + 0.5f * wave);
    water = water + Col3{glitter * 0.25f, glitter * 0.12f, glitter * 0.04f};

    return col_clamp(water);
}

// ── Render one pixel ────────────────────────────────────────────────────────

static void render_pixel(int px, int py, int pw, int ph) {
    float u = (2.f * px - pw) / static_cast<float>(ph);
    float v_coord = (ph - 2.f * py) / static_cast<float>(ph);

    // Camera basis
    float cy = std::cos(g_cam_yaw), sy = std::sin(g_cam_yaw);
    float cp = std::cos(g_cam_pitch), sp = std::sin(g_cam_pitch);
    v3 fwd   = {sy * cp, sp, -cy * cp};
    v3 right = {cy, 0, sy};
    v3 up    = {-sy * sp, cp, cy * sp};

    float focal = 1.0f;
    v3 rd = normalize(fwd * focal + right * u + up * v_coord);

    // ── Raymarch terrain ──

    float max_dist = 600.f;
    float dt_step = 0.6f;
    float t = 0.3f;

    bool hit_terrain = false;
    v3 hit_pos = {};
    float hit_t = max_dist;

    // Analytic water plane
    float water_t = max_dist;
    if (rd.y < -0.001f) {
        water_t = (g_cam_pos.y - WATER_LEVEL) / (-rd.y);
        if (water_t < 0) water_t = max_dist;
    }

    for (int step = 0; step < 200 && t < max_dist; ++step) {
        v3 p = g_cam_pos + rd * t;
        float h = terrain_height(p.x, p.z);

        if (p.y < h) {
            // Binary search refinement (8 iterations for precision)
            float lo = t - dt_step, hi = t;
            for (int r = 0; r < 8; ++r) {
                float mid = (lo + hi) * 0.5f;
                v3 mp = g_cam_pos + rd * mid;
                if (mp.y < terrain_height(mp.x, mp.z)) hi = mid;
                else lo = mid;
            }
            hit_t = (lo + hi) * 0.5f;
            hit_pos = g_cam_pos + rd * hit_t;
            hit_terrain = true;
            break;
        }

        float above = p.y - h;
        dt_step = clampf(above * 0.3f, 0.3f, 6.f);
        t += dt_step;
    }

    // ── Shade ──

    Col3 color;

    if (hit_terrain && hit_t < water_t) {
        v3 n = terrain_normal(hit_pos.x, hit_pos.z);
        color = terrain_shade(hit_pos, n, rd);
        color = apply_fog(color, hit_t, rd);
    } else if (water_t < max_dist && water_t < hit_t) {
        v3 wp = g_cam_pos + rd * water_t;
        color = water_shade(wp, rd, water_t);
        color = apply_fog(color, water_t, rd);
    } else {
        color = sky_color(rd);
    }

    // ── Rings ──
    for (auto& ring : g_rings) {
        if (ring.collected) continue;
        v3 to_ring = ring.pos - g_cam_pos;
        float along = dot(to_ring, fwd);
        if (along < 2.f || along > 300.f) continue;

        float rx = dot(to_ring, right);
        float ry = dot(to_ring, up);
        float screen_x = rx / along * focal;
        float screen_y = ry / along * focal;

        float du = u - screen_x, dv = v_coord - screen_y;
        float screen_r = ring.radius / along * focal;
        float d = std::sqrt(du*du + dv*dv);
        float ring_edge = std::fabs(d - screen_r);
        float thickness = clampf(0.5f / along * focal, 0.005f, 0.08f);

        if (ring_edge < thickness) {
            float t_ring = 1.f - ring_edge / thickness;
            t_ring = t_ring * t_ring;
            float pulse = 0.7f + 0.3f * std::sin(g_time * 4.f);
            Col3 ring_col = {1.f * pulse, 0.8f * pulse, 0.2f * pulse};
            color = col_lerp(color, ring_col, t_ring * 0.9f);
        } else if (ring_edge < thickness * 3.f) {
            float glow = 1.f - (ring_edge - thickness) / (thickness * 2.f);
            color = color + Col3{0.8f, 0.6f, 0.1f} * (glow * glow * 0.2f);
        }
    }

    px_set_col(px, py, color);
}

// ── Post-processing ─────────────────────────────────────────────────────────

static void post_process(int pw, int ph) {
    float cx = pw * 0.5f, cy = ph * 0.5f;
    for (int y = 0; y < ph; ++y) {
        for (int x = 0; x < pw; ++x) {
            auto& px = g_pixels[static_cast<size_t>(y * pw + x)];
            float r = px.r/255.f, g = px.g/255.f, b = px.b/255.f;

            // ACES-like filmic tone mapping (preserves contrast better than Reinhard)
            auto tonemap = [](float x) {
                float a = x * (x * 2.51f + 0.03f);
                float d = x * (x * 2.43f + 0.59f) + 0.14f;
                return clampf(a / d, 0.f, 1.f);
            };
            r = tonemap(r); g = tonemap(g); b = tonemap(b);

            // Subtle vignette
            float dx = (x - cx)/cx, dy = (y - cy)/cy;
            float vig = 1.f - (dx*dx + dy*dy) * 0.12f;
            vig = clampf(vig, 0.6f, 1.f);
            r *= vig; g *= vig; b *= vig;

            px.r = static_cast<uint8_t>(clampf(r,0,1)*255.f);
            px.g = static_cast<uint8_t>(clampf(g,0,1)*255.f);
            px.b = static_cast<uint8_t>(clampf(b,0,1)*255.f);
        }
    }
}

// ── Paint ───────────────────────────────────────────────────────────────────

static void paint(Canvas& canvas, int w, int h) {
    if (w != g_w || h != g_h) return;
    game_tick(1.f / 30.f);

    int canvas_h = h - 1;
    int ph = canvas_h * 2;

    // Multi-threaded rendering
    static const int n_threads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    auto render_rows = [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < g_pixel_w; ++x)
                render_pixel(x, y, g_pixel_w, ph);
    };

    if (n_threads <= 1 || ph < 8) {
        render_rows(0, ph);
    } else {
        std::vector<std::jthread> threads;
        threads.reserve(static_cast<size_t>(n_threads));
        int chunk = (ph + n_threads - 1) / n_threads;
        for (int t = 0; t < n_threads; ++t) {
            int lo = t * chunk, hi = std::min(lo + chunk, ph);
            if (lo >= hi) break;
            threads.emplace_back([=] { render_rows(lo, hi); });
        }
    }

    post_process(g_pixel_w, ph);
    composite(canvas, canvas_h);

    // ── HUD ──
    {
        auto* pool = canvas.style_pool();
        float ground_h = terrain_height(g_cam_pos.x, g_cam_pos.z);
        float alt = g_cam_pos.y - std::fmax(ground_h, WATER_LEVEL);

        // Altimeter
        int bar_h = std::min(canvas_h - 2, 12);
        int bar_x = 1, bar_y0 = canvas_h - bar_h - 1;
        float alt_pct = clampf(alt / 80.f, 0, 1);
        int filled = static_cast<int>(alt_pct * bar_h);

        uint16_t s_frame = pool->intern(Style{}.with_fg(Color::rgb(50,50,70)).with_bg(Color::rgb(0,0,0)));
        uint16_t s_fill  = pool->intern(Style{}.with_fg(Color::rgb(50,180,240)).with_bg(Color::rgb(15,30,50)));
        for (int i = 0; i < bar_h; ++i) {
            int y = bar_y0 + bar_h - 1 - i;
            canvas.set(bar_x, y, i < filled ? U'\u2588' : U'\u2591', i < filled ? s_fill : s_frame);
        }

        char alt_buf[16]; std::snprintf(alt_buf, sizeof(alt_buf), "%dm", static_cast<int>(alt));
        uint16_t s_text = pool->intern(Style{}.with_fg(Color::rgb(100,160,220)).with_bg(Color::rgb(0,0,0)));
        canvas.write_text(0, bar_y0 - 1, alt_buf, s_text);

        // Speed
        char spd_buf[16]; std::snprintf(spd_buf, sizeof(spd_buf), "%dkm/h", static_cast<int>(g_speed * 3.6f));
        int slen = static_cast<int>(std::strlen(spd_buf));
        uint16_t s_spd = pool->intern(Style{}.with_fg(
            g_boost > 0.5f ? Color::rgb(255,150,50) : Color::rgb(100,220,100))
            .with_bg(Color::rgb(0,0,0)));
        canvas.write_text(w - slen - 1, canvas_h - 2, spd_buf, s_spd);
    }

    // Status bar
    int bar_y = h - 1;
    for (int x = 0; x < w; ++x) canvas.set(x, bar_y, U' ', g_bar_bg);
    const char* help = "TERRAIN \xe2\x94\x82 [wasd] steer \xe2\x94\x82 [space] ascend \xe2\x94\x82 [c] descend \xe2\x94\x82 [b] boost \xe2\x94\x82 [q] quit";
    canvas.write_text(1, bar_y, help, g_bar_dim);
    canvas.write_text(1, bar_y, "TERRAIN", g_bar_accent);

    char dbuf[32]; std::snprintf(dbuf, sizeof(dbuf), "%.1fkm", g_dist / 1000.f);
    canvas.write_text(w/2 - static_cast<int>(std::strlen(dbuf))/2, bar_y, dbuf, g_bar_alt);

    char sbuf[32]; std::snprintf(sbuf, sizeof(sbuf), "\xe2\x98\x85%d", g_score);
    int sblen = static_cast<int>(std::strlen(sbuf));
    if (w > sblen + 2) canvas.write_text(w - sblen - 1, bar_y, sbuf, g_bar_accent);
}

// ── Main ────────────────────────────────────────────────────────────────────

int main() {
    reset_game();
    (void)canvas_run(
        CanvasConfig{.fps = 30, .mouse = false, .mode = Mode::Fullscreen, .auto_clear = false, .title = "terrain"},
        rebuild, handle, paint
    );
}
