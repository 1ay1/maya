// examples/space3d.cpp — TERRAIN: flight over raymarched terrain.
//
// Per-pixel raymarched heightmap terrain with water reflections, ambient
// occlusion, soft shadows, erosion-like texturing and a golden-hour sky.
// Fly through the gold rings.
//
//   Model      the camera (position, yaw, pitch, speed, boost), distance,
//              score, the rings, and when each steering key was last hit.
//   update()   Tick flies the camera (a key counts as held for a few
//              frames after its last repeat), spawns and collects rings.
//   view()     each pixel a ray (Image::fill_rows, all cores), tone mapped;
//              a status bar with altitude, speed, distance and score.
//
// Keys: wasd/arrows steer   space ascend   c descend   b boost   r reset   q quit

#include <maya/app.hpp>
#include <maya/element/pixels.hpp>
#include <maya/maya.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using namespace maya;
using namespace maya::dsl;
using namespace std::chrono_literals;

namespace {

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

// Lattice hash -> [0, 1). An integer mix, not the shader-classic
// fract(sin(dot) * 43758): terrain_height() is 13 value_noise lookups (52
// hashes) per ray-march step, and sinf of an argument in the thousands pays
// a full range reduction every time. That made sinf the #1 frame in a
// profile at 568% CPU / 7 fps. Same role (a well-mixed value per lattice
// point), a few integer ops.
static float hash(float x, float y) {
    auto h = static_cast<std::uint32_t>(static_cast<std::int32_t>(x)) * 0x8da6b343u
           ^ static_cast<std::uint32_t>(static_cast<std::int32_t>(y)) * 0xd8163841u;
    h ^= h >> 15; h *= 0x2c1b3c6du;
    h ^= h >> 12; h *= 0x297a2d39u;
    h ^= h >> 15;
    return static_cast<float>(h >> 8) * (1.f / 16777216.f);
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
static const v3 g_sun_dir = normalize(v3{0.5f, 0.28f, -0.7f});
static const Col3 g_sun_color = {1.6f, 1.15f, 0.7f};

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

static constexpr float WATER_LEVEL = 0.0f;
static constexpr float MIN_HEIGHT  = 4.f;
static constexpr float CAM_PITCH_RANGE = 0.35f;

static constexpr float BASE_SPEED  = 40.f;
static constexpr float BOOST_SPEED = 120.f;
static constexpr float STEER_RATE  = 1.8f;
static constexpr float PITCH_RATE  = 0.8f;
static constexpr float VERT_RATE   = 18.f;

// ── model ──────────────────────────────────────────────────────────────────

static constexpr int HOLD_FRAMES = 5;       // a key counts as held this many frames after its last repeat
enum KeyAction { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_ASCEND, K_DESCEND, K_BOOST, K_COUNT };

struct Ring { v3 pos; float radius; bool collected; };

struct Model {
    int w = 0, h = 0;                        // the picture, in pixels
    v3    cam_pos   = {0, 35, 0};
    float cam_yaw   = 0.f;
    float cam_pitch = -0.06f;
    float speed     = BASE_SPEED;
    float boost     = 0.f;
    int   frame     = 0;
    float time      = 0.f;
    float dist      = 0.f;
    int   score     = 0;
    std::array<int, K_COUNT> key_last = {-100, -100, -100, -100, -100, -100, -100};
    std::vector<Ring> rings;
};

static void spawn_rings_ahead(Model& m) {
    float ahead_z = m.cam_pos.z - 200.f;
    for (int i = 0; i < 5; ++i) {
        float rz = ahead_z - i * 60.f;
        bool exists = false;
        for (auto& r : m.rings) {
            if (std::fabs(r.pos.z - rz) < 30.f) { exists = true; break; }
        }
        if (exists) continue;
        float rx = m.cam_pos.x + (hash(rz * 0.1f, 0.f) - 0.5f) * 80.f;
        float th = terrain_height(rx, rz);
        float ry = std::fmax(th, WATER_LEVEL) + 12.f + hash(rz * 0.1f, 1.f) * 15.f;
        m.rings.push_back({v3{rx, ry, rz}, 5.f, false});
    }
    m.rings.erase(std::remove_if(m.rings.begin(), m.rings.end(),
        [&](const Ring& r) { return r.pos.z > m.cam_pos.z + 50.f; }), m.rings.end());
}

static void game_tick(Model& m, float dt) {
    m.frame++;
    m.time += dt;

    auto held = [&m](KeyAction k) { return (m.frame - m.key_last[k]) < HOLD_FRAMES; };
    float turn  = (held(K_RIGHT) ? 1.f : 0.f) - (held(K_LEFT)  ? 1.f : 0.f);
    float pitch = (held(K_UP)    ? 1.f : 0.f) - (held(K_DOWN)  ? 1.f : 0.f);
    float vert  = (held(K_ASCEND)? 1.f : 0.f) - (held(K_DESCEND)? 1.f : 0.f);
    bool boosting = held(K_BOOST);

    m.cam_yaw += turn * STEER_RATE * dt;
    m.cam_pitch += pitch * PITCH_RATE * dt;
    m.cam_pitch = clampf(m.cam_pitch, -CAM_PITCH_RANGE, CAM_PITCH_RANGE);

    float target_speed = boosting ? BOOST_SPEED : BASE_SPEED;
    m.speed = lerp(m.speed, target_speed, 3.f * dt);
    m.boost = lerp(m.boost, boosting ? 1.f : 0.f, 4.f * dt);

    float fwd_x = std::sin(m.cam_yaw), fwd_z = -std::cos(m.cam_yaw);
    m.cam_pos.x += fwd_x * m.speed * dt;
    m.cam_pos.z += fwd_z * m.speed * dt;
    m.cam_pos.y += vert * VERT_RATE * dt;

    float ground = terrain_height(m.cam_pos.x, m.cam_pos.z);
    float min_y = std::fmax(ground, WATER_LEVEL) + MIN_HEIGHT;
    if (m.cam_pos.y < min_y) m.cam_pos.y = lerp(m.cam_pos.y, min_y, 8.f * dt);
    m.cam_pos.y = clampf(m.cam_pos.y, min_y, 120.f);

    m.dist += m.speed * dt;

    spawn_rings_ahead(m);
    for (auto& r : m.rings) {
        if (r.collected) continue;
        if (len(m.cam_pos - r.pos) < r.radius + 3.f) { r.collected = true; m.score += 100; }
    }
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

static Col3 water_shade(float time, v3 pos, v3 rd, float dist) {
    // Multi-octave animated waves
    float t = time;
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

static Col3 trace(const Model& m, int px, int py, int pw, int ph) {
    float u = (2.f * px - pw) / static_cast<float>(ph);
    float v_coord = (ph - 2.f * py) / static_cast<float>(ph);

    // Camera basis
    float cy = std::cos(m.cam_yaw), sy = std::sin(m.cam_yaw);
    float cp = std::cos(m.cam_pitch), sp = std::sin(m.cam_pitch);
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
        water_t = (m.cam_pos.y - WATER_LEVEL) / (-rd.y);
        if (water_t < 0) water_t = max_dist;
    }

    // March only as far as the water: terrain behind the water plane is
    // never shown (the shade below takes the water whenever hit_t >= water_t),
    // but the loop used to keep marching to max_dist for every ray that hit
    // water, up to 200 terrain_height calls (13 noise lookups each) spent
    // on nothing. Same image, and water covers a lot of this terrain.
    const float march_end = std::fmin(max_dist, water_t);
    for (int step = 0; step < 200 && t < march_end; ++step) {
        v3 p = m.cam_pos + rd * t;
        float h = terrain_height(p.x, p.z);

        if (p.y < h) {
            // Binary search refinement (8 iterations for precision)
            float lo = t - dt_step, hi = t;
            for (int r = 0; r < 8; ++r) {
                float mid = (lo + hi) * 0.5f;
                v3 mp = m.cam_pos + rd * mid;
                if (mp.y < terrain_height(mp.x, mp.z)) hi = mid;
                else lo = mid;
            }
            hit_t = (lo + hi) * 0.5f;
            hit_pos = m.cam_pos + rd * hit_t;
            hit_terrain = true;
            break;
        }

        float above = p.y - h;
        // Step by height-above-terrain, with a floor that grows with
        // distance: one pixel covers ~t/focal world units, so resolving the
        // surface to finer than that far away is work nobody can see. The
        // fixed 0.3 floor made every grazing ray near the horizon crawl:
        // measured 74 terrain_height calls per pixel in the march (90% of
        // the frame). The binary search on a hit still refines to the same
        // precision, so edges stay sharp.
        dt_step = clampf(above * 0.3f, 0.3f + t * 0.004f, 6.f + t * 0.01f);
        t += dt_step;
    }

    // ── Shade ──

    Col3 color;

    if (hit_terrain && hit_t < water_t) {
        v3 n = terrain_normal(hit_pos.x, hit_pos.z);
        color = terrain_shade(hit_pos, n, rd);
        color = apply_fog(color, hit_t, rd);
    } else if (water_t < max_dist && water_t < hit_t) {
        v3 wp = m.cam_pos + rd * water_t;
        color = water_shade(m.time, wp, rd, water_t);
        color = apply_fog(color, water_t, rd);
    } else {
        color = sky_color(rd);
    }

    // ── Rings ──
    for (auto& ring : m.rings) {
        if (ring.collected) continue;
        v3 to_ring = ring.pos - m.cam_pos;
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
            float pulse = 0.7f + 0.3f * std::sin(m.time * 4.f);
            Col3 ring_col = {1.f * pulse, 0.8f * pulse, 0.2f * pulse};
            color = col_lerp(color, ring_col, t_ring * 0.9f);
        } else if (ring_edge < thickness * 3.f) {
            float glow = 1.f - (ring_edge - thickness) / (thickness * 2.f);
            color = color + Col3{0.8f, 0.6f, 0.1f} * (glow * glow * 0.2f);
        }
    }

    return color;
}

// ACES-ish tone map, then a soft vignette.
static Rgb finish(Col3 c, int x, int y, int pw, int ph) {
    auto tonemap = [](float v) {
        const float a = v * (v * 2.51f + 0.03f);
        const float d = v * (v * 2.43f + 0.59f) + 0.14f;
        return clampf(a / d, 0.f, 1.f);
    };
    const float cx = pw * 0.5f, cy = ph * 0.5f;
    const float dx = (x - cx) / cx, dy = (y - cy) / cy;
    const float vig = clampf(1.f - (dx * dx + dy * dy) * 0.12f, 0.6f, 1.f);
    // 5 bits a channel keeps the renderer's style cache hot.
    auto q = [vig](float v) { return static_cast<std::uint8_t>(static_cast<int>(clampf(v * vig, 0, 1) * 255.f) & 0xF8); };
    return {q(tonemap(clampf(c.r, 0, 1))), q(tonemap(clampf(c.g, 0, 1))), q(tonemap(clampf(c.b, 0, 1)))};
}

// ── program ────────────────────────────────────────────────────────────────

struct Tick {};
struct Resize { int cols, rows; };
struct Press  { KeyAction k; };
struct Reset {};
struct Quit {};
using Msg = std::variant<Tick, Resize, Press, Reset, Quit>;

struct Terrain {
    using Model = ::Model;
    using Msg   = ::Msg;
    using Cmd   = jaal::Cmd<Msg>;
    using Sub   = jaal::Sub<Msg, on_key, on_resize>;

    static Cmd update(Model& m, Tick)     { game_tick(m, 1.f / 30.f); return {}; }
    static Cmd update(Model& m, Resize r) { m.w = std::max(1, r.cols); m.h = std::max(2, (r.rows - 1) * 2); return {}; }
    static Cmd update(Model& m, Reset)    { m = Model{.w = m.w, .h = m.h}; return {}; }
    static Cmd update(Model&, Quit)       { return Cmd::quit(0); }
    static Cmd update(Model& m, Press p) {
        m.key_last[p.k] = m.frame;
        switch (p.k) {                        // opposite directions cancel
            case K_UP:    m.key_last[K_DOWN]  = -100; break;
            case K_DOWN:  m.key_last[K_UP]    = -100; break;
            case K_LEFT:  m.key_last[K_RIGHT] = -100; break;
            case K_RIGHT: m.key_last[K_LEFT]  = -100; break;
            default: break;
        }
        return {};
    }

    static Element status_bar(const Model& m) {
        const float ground = terrain_height(m.cam_pos.x, m.cam_pos.z);
        const int alt = static_cast<int>(m.cam_pos.y - std::fmax(ground, WATER_LEVEL));
        char right[96];
        std::snprintf(right, sizeof right, " alt %dm │ %dkm/h │ %.1fkm │ ★%d ",
                      alt, static_cast<int>(m.speed * 3.6f), m.dist / 1000.f, m.score);
        const auto accent = Color::rgb(255, 200, 60);
        return h(text(" TERRAIN") | fgc(accent) | Bold,
                 text(" │ [wasd] steer │ [space] ascend │ [c] descend │ [b] boost │ [r] reset │ [q] quit")
                     | fgc(Color::rgb(90, 90, 110)),
                 spacer(),
                 text(right) | fgc(m.boost > 0.5f ? Color::rgb(255, 150, 50) : accent) | Bold)
               | bgc(Color::rgb(10, 10, 15));
    }

    static Element view(const Model& m) {
        if (m.w < 4 || m.h < 4) return text("");
        Image img(m.w, m.h);
        img.fill_rows([&](int x, int y) { return finish(trace(m, x, y, m.w, m.h), x, y, m.w, m.h); });
        return v(pixels(std::move(img)), status_bar(m));
    }

    static Sub subscribe(const Model&) {
        return Sub::batch(
            Sub::every(33ms, Tick{}),
            Sub::on(on_resize{}, [](const ResizeEvent& r) -> std::optional<Msg> {
                return Resize{r.width.value, r.height.value};
            }),
            keys<Sub>({
                {'w', Press{K_UP}}, {'s', Press{K_DOWN}}, {'a', Press{K_LEFT}}, {'d', Press{K_RIGHT}},
                {SpecialKey::Up, Press{K_UP}}, {SpecialKey::Down, Press{K_DOWN}},
                {SpecialKey::Left, Press{K_LEFT}}, {SpecialKey::Right, Press{K_RIGHT}},
                {' ', Press{K_ASCEND}}, {'c', Press{K_DESCEND}}, {'b', Press{K_BOOST}},
                {'r', Reset{}}, {'q', Quit{}}, {SpecialKey::Escape, Quit{}},
            }));
    }
    static bool subs_key(const Model&) { return true; }
};

static_assert(Program<Terrain>);

}  // namespace

int main() { return run<Terrain>({.title = "terrain"}); }
