// examples/jaal_raymarch.cpp — GENERATED from raymarch.cpp by tools/port_canvas.py.
// Do not edit: change raymarch.cpp and re-run the tool.
//
// The same demo on jaal: its canvas_run() call becomes run_canvas()
// (maya/jaal/canvas.hpp), which draws it as a `paint` element through
// maya::Screen, so it gets the Screen's flow control (never more than one
// frame ahead of the terminal: `q` is instant over a slow ssh link).
//
// Built only with -DMAYA_WITH_JAAL=ON.
#include <maya/jaal/canvas.hpp>
// maya — Real-time raymarching 3D renderer
// Half-block SDF renderer: reflective surfaces, sunset sky, stars, fresnel,
// tone mapping, colored lights. Arrows=orbit, space=pause, 1-4=scene, q=quit

#include <maya/internal.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace maya;

static constexpr float PI  = 3.14159265f;
static constexpr float TAU = 6.28318530f;
static constexpr float EPS = 0.001f;
static constexpr int   MAX_STEPS = 64;
static constexpr float MAX_DIST  = 50.f;

// ── vec3 ─────────────────────────────────────────────────────────────────────

struct vec3 {
    float x, y, z;
    vec3 operator+(vec3 b) const { return {x+b.x, y+b.y, z+b.z}; }
    vec3 operator-(vec3 b) const { return {x-b.x, y-b.y, z-b.z}; }
    vec3 operator*(float s) const { return {x*s, y*s, z*s}; }
    vec3 operator*(vec3 b) const { return {x*b.x, y*b.y, z*b.z}; }
    vec3 operator-() const { return {-x, -y, -z}; }
};
inline float dot(vec3 a, vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline float length(vec3 v) { return std::sqrt(dot(v, v)); }
inline vec3 normalize(vec3 v) { float l = length(v); return v * (1.f / (l + 1e-9f)); }
inline vec3 vabs(vec3 v) { return {std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)}; }
inline vec3 vmax(vec3 a, float b) { return {std::fmax(a.x,b), std::fmax(a.y,b), std::fmax(a.z,b)}; }
inline vec3 vmin(vec3 a, float b) { return {std::fmin(a.x,b), std::fmin(a.y,b), std::fmin(a.z,b)}; }
inline vec3 mix(vec3 a, vec3 b, float t) { return a * (1.f - t) + b * t; }
inline float clampf(float x, float lo, float hi) { return std::fmin(std::fmax(x, lo), hi); }
inline float smoothstep(float e0, float e1, float x) {
    float t = clampf((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// ── SDF primitives ───────────────────────────────────────────────────────────

inline float sd_sphere(vec3 p, float r) { return length(p) - r; }

inline float sd_torus(vec3 p, float r1, float r2) {
    float q = std::sqrt(p.x*p.x + p.z*p.z) - r1;
    return std::sqrt(q*q + p.y*p.y) - r2;
}

inline float sd_plane(vec3 p, vec3 n, float h) { return dot(p, n) + h; }

inline float sd_box(vec3 p, vec3 b) {
    vec3 d = vabs(p) - b;
    return length(vmax(d, 0.f)) + std::fmin(std::fmax(d.x, std::fmax(d.y, d.z)), 0.f);
}

inline float sd_capsule(vec3 p, vec3 a, vec3 b, float r) {
    vec3 ab = b - a, ap = p - a;
    float t = clampf(dot(ap, ab) / dot(ab, ab), 0.f, 1.f);
    return length(p - (a + ab * t)) - r;
}

inline float smooth_union(float a, float b, float k) {
    float h = std::fmax(k - std::fabs(a - b), 0.f) / k;
    return std::fmin(a, b) - h * h * k * 0.25f;
}

// ── Scenes ───────────────────────────────────────────────────────────────────

static int   g_scene    = 0;
static float g_time     = 0.f;
static bool  g_paused   = false;
static float g_orbit_speed = 0.3f;
static float g_pitch    = 0.3f;
static int   g_frame    = 0;
static float g_elapsed  = 0.f;

// Material IDs: 0=ground, 1=primary, 2=secondary, 3=sky, 4=emissive
struct Hit { float d; int mat; };

// ── Per-frame scene constants ────────────────────────────────────────────────
// Everything that depends only on g_time (object positions, rotation
// sin/cos) is computed ONCE per frame here. The scene functions below run for
// every march step of every ray, 60+ times per pixel: evaluating the same
// sin(g_time * k) there put sinf/sincosf at the top of the profile (175% CPU).
struct Rot { float c, s; };
static Rot rot_of(float a) { return {std::cos(a), std::sin(a)}; }
inline vec3 rot_y(vec3 p, Rot r) { return {p.x*r.c + p.z*r.s, p.y, -p.x*r.s + p.z*r.c}; }
inline vec3 rot_x(vec3 p, Rot r) { return {p.x, p.y*r.c - p.z*r.s, p.y*r.s + p.z*r.c}; }
inline vec3 rot_z(vec3 p, Rot r) { return {p.x*r.c - p.y*r.s, p.x*r.s + p.y*r.c, p.z}; }

static struct FrameConsts {
    // classic
    float bob; Rot torus_y, torus_x, ring_y;
    // metaballs
    vec3 m1, m2, m3, m4;
    // columns
    vec3 orb_c, p1, p2;
    // cathedral
    vec3 cols[8]; float orb_y; Rot r1_y, r1_x, r2_y, r2_z;
} g_fc;

static void update_frame_consts() {
    const float T = g_time;
    g_fc.bob = std::sin(T * 1.8f) * 0.5f + 1.5f;
    g_fc.torus_y = rot_of(T * 0.6f);
    g_fc.torus_x = rot_of(T * 0.4f);
    g_fc.ring_y  = rot_of(-T * 1.2f);
    const float t = T * 0.8f;
    g_fc.m1 = {std::sin(t)*1.5f, 1.2f + std::sin(t*1.3f)*0.4f, std::cos(t)*1.5f};
    g_fc.m2 = {std::cos(t*0.7f)*1.8f, 1.5f + std::cos(t*1.1f)*0.3f, std::sin(t*0.9f)*1.8f};
    g_fc.m3 = {0, 1.0f + std::sin(t*1.6f)*0.6f, 0};
    g_fc.m4 = {std::sin(t*1.1f)*1.2f, 0.8f + std::cos(t*0.8f)*0.5f, std::cos(t*1.4f)*1.2f};
    g_fc.orb_c = {0, 2.f + std::sin(T)*0.5f, 0};
    g_fc.p1 = {std::sin(T*1.5f)*3.f, 1.5f + std::cos(T*2.f)*0.5f, std::cos(T*1.5f)*3.f};
    g_fc.p2 = {std::cos(T*1.2f)*2.5f, 2.5f + std::sin(T*1.7f)*0.3f, std::sin(T*1.2f)*2.5f};
    for (int i = 0; i < 8; ++i) {
        const float a = float(i) * TAU / 8.f;
        g_fc.cols[i] = {std::cos(a) * 4.f, 0, std::sin(a) * 4.f};
    }
    g_fc.orb_y = 3.f + std::sin(T * 0.7f) * 0.3f;
    g_fc.r1_y = rot_of(T * 0.5f);  g_fc.r1_x = rot_of(PI * 0.3f);
    g_fc.r2_y = rot_of(-T * 0.4f); g_fc.r2_z = rot_of(PI * 0.4f);
}

static Hit scene_classic(vec3 p) {
    float ground = sd_plane(p, {0,1,0}, 0.f);
    float sphere = sd_sphere(p - vec3{0, g_fc.bob, 0}, 1.0f);
    vec3 tp = rot_y(p - vec3{0, 1.2f, 0}, g_fc.torus_y);
    tp = rot_x(tp, g_fc.torus_x);
    float torus = sd_torus(tp, 2.0f, 0.35f);

    // Floating crystal ring
    vec3 rp = rot_y(p - vec3{0, g_fc.bob, 0}, g_fc.ring_y);
    float ring = sd_torus(rp, 1.8f, 0.06f);

    Hit h = {ground, 0};
    if (sphere < h.d) h = {sphere, 1};
    if (torus < h.d)  h = {torus, 2};
    if (ring < h.d)   h = {ring, 4};
    return h;
}

static Hit scene_metaballs(vec3 p) {
    float ground = sd_plane(p, {0,1,0}, 0.f);
    float s1 = sd_sphere(p - g_fc.m1, 0.8f);
    float s2 = sd_sphere(p - g_fc.m2, 0.7f);
    float s3 = sd_sphere(p - g_fc.m3, 0.9f);
    float s4 = sd_sphere(p - g_fc.m4, 0.5f);
    float blob = smooth_union(smooth_union(s1, s2, 0.8f), smooth_union(s3, s4, 0.8f), 0.8f);

    Hit h = {ground, 0};
    if (blob < h.d) h = {blob, 1};
    return h;
}

static Hit scene_columns(vec3 p) {
    float ground = sd_plane(p, {0,1,0}, 0.f);
    // Infinite repeating columns
    vec3 rp = p;
    rp.x = std::fmod(std::fabs(rp.x) + 2.f, 4.f) - 2.f;
    rp.z = std::fmod(std::fabs(rp.z) + 2.f, 4.f) - 2.f;
    float col = sd_box(rp - vec3{0, 2.5f, 0}, {0.3f, 2.5f, 0.3f});
    // Floating orb
    float orb = sd_sphere(p - g_fc.orb_c, 0.6f);
    // Orbiting emissive particles
    float p1 = sd_sphere(p - g_fc.p1, 0.15f);
    float p2 = sd_sphere(p - g_fc.p2, 0.12f);

    Hit h = {ground, 0};
    if (col < h.d) h = {col, 2};
    if (orb < h.d) h = {orb, 1};
    if (p1 < h.d)  h = {p1, 4};
    if (p2 < h.d)  h = {p2, 4};
    return h;
}

static Hit scene_cathedral(vec3 p) {
    float ground = sd_plane(p, {0,1,0}, 0.f);

    // Tall arched columns in a circle
    float cols = MAX_DIST;
    for (int i = 0; i < 8; ++i) {
        const vec3 cp = g_fc.cols[i];
        float c = sd_capsule(p, cp, cp + vec3{0, 5.f, 0}, 0.2f);
        cols = std::fmin(cols, c);
    }

    // Central glowing orb
    float orb = sd_sphere(p - vec3{0, g_fc.orb_y, 0}, 0.8f);

    // Rotating rings around orb
    vec3 r1p = rot_y(p - vec3{0, 3.f, 0}, g_fc.r1_y);
    r1p = rot_x(r1p, g_fc.r1_x);
    float ring1 = sd_torus(r1p, 1.8f, 0.05f);
    vec3 r2p = rot_y(p - vec3{0, 3.f, 0}, g_fc.r2_y);
    r2p = rot_z(r2p, g_fc.r2_z);
    float ring2 = sd_torus(r2p, 2.2f, 0.04f);

    Hit h = {ground, 0};
    if (cols < h.d)  h = {cols, 2};
    if (orb < h.d)   h = {orb, 1};
    if (ring1 < h.d) h = {ring1, 4};
    if (ring2 < h.d) h = {ring2, 4};
    return h;
}

static Hit map(vec3 p) {
    switch (g_scene) {
        case 1:  return scene_metaballs(p);
        case 2:  return scene_columns(p);
        case 3:  return scene_cathedral(p);
        default: return scene_classic(p);
    }
}

static float map_dist(vec3 p) { return map(p).d; }

// ── Raymarching ──────────────────────────────────────────────────────────────

static Hit raymarch(vec3 ro, vec3 rd) {
    float t = 0.f;
    Hit h = {MAX_DIST, 3};
    for (int i = 0; i < MAX_STEPS && t < MAX_DIST; ++i) {
        vec3 p = ro + rd * t;
        Hit s = map(p);
        if (s.d < EPS) { h = {t, s.mat}; break; }
        t += s.d;
    }
    h.d = t;
    return h;
}

static vec3 calc_normal(vec3 p) {
    const float e = 0.001f;
    float d = map_dist(p);
    return normalize({
        map_dist(p + vec3{e,0,0}) - d,
        map_dist(p + vec3{0,e,0}) - d,
        map_dist(p + vec3{0,0,e}) - d
    });
}

// ── Lighting ─────────────────────────────────────────────────────────────────

static float soft_shadow(vec3 ro, vec3 rd, float mint, float maxt, float k) {
    float res = 1.f;
    float t = mint;
    for (int i = 0; i < 20 && t < maxt; ++i) {
        float d = map_dist(ro + rd * t);
        if (d < EPS) return 0.f;
        res = std::fmin(res, k * d / t);
        t += std::fmax(d, 0.02f);
    }
    return std::fmax(res, 0.f);
}

static float calc_ao(vec3 p, vec3 n) {
    float occ = 0.f;
    float scale = 1.f;
    for (int i = 0; i < 3; ++i) {
        float h = 0.01f + 0.15f * float(i);
        float d = map_dist(p + n * h);
        occ += (h - d) * scale;
        scale *= 0.95f;
    }
    return std::fmax(1.f - 3.f * occ, 0.f);
}

// Fresnel-Schlick approximation
static float fresnel(float cosTheta, float f0) {
    return f0 + (1.f - f0) * std::pow(1.f - clampf(cosTheta, 0.f, 1.f), 5.f);
}

struct Color3 { float r, g, b; };
inline Color3 cmix(Color3 a, Color3 b, float t) {
    return {a.r*(1-t)+b.r*t, a.g*(1-t)+b.g*t, a.b*(1-t)+b.b*t};
}
inline Color3 cadd(Color3 a, Color3 b) { return {a.r+b.r, a.g+b.g, a.b+b.b}; }
inline Color3 cmul(Color3 a, float s) { return {a.r*s, a.g*s, a.b*s}; }
inline Color3 cmul(Color3 a, Color3 b) { return {a.r*b.r, a.g*b.g, a.b*b.b}; }

// ── Sky ──────────────────────────────────────────────────────────────────────

static Color3 sky_color(vec3 rd) {
    float y = rd.y;

    // Sunset gradient: deep blue → orange → warm horizon
    Color3 deep_sky = {0.02f, 0.03f, 0.12f};   // deep navy
    Color3 mid_sky  = {0.08f, 0.05f, 0.20f};    // purple
    Color3 horizon  = {0.45f, 0.18f, 0.08f};    // warm orange
    Color3 ground_c = {0.02f, 0.02f, 0.03f};    // below horizon

    Color3 col;
    if (y < 0.f) {
        col = cmix(ground_c, horizon, smoothstep(-0.3f, 0.f, y));
    } else if (y < 0.15f) {
        col = cmix(horizon, mid_sky, smoothstep(0.f, 0.15f, y));
    } else {
        col = cmix(mid_sky, deep_sky, smoothstep(0.15f, 0.7f, y));
    }

    // Sun glow
    vec3 sun_dir = normalize({0.6f, 0.15f, -0.4f});
    float sun_dot = std::fmax(dot(rd, sun_dir), 0.f);
    float sun = std::pow(sun_dot, 64.f);
    float glow = std::pow(sun_dot, 8.f);
    col = cadd(col, {sun * 2.f, sun * 1.5f, sun * 0.8f});
    col = cadd(col, {glow * 0.3f, glow * 0.12f, glow * 0.04f});

    // Stars (hash-based, only in upper sky)
    if (y > 0.2f) {
        // Simple star field using pseudo-random from direction
        float star_h = std::fabs(std::sin(rd.x * 213.17f + rd.z * 437.23f) *
                                  std::cos(rd.x * 171.31f + rd.z * 339.41f));
        star_h = std::pow(star_h, 80.f);
        float star_bright = star_h * smoothstep(0.2f, 0.5f, y) * 2.f;
        // Twinkling
        float twinkle = 0.7f + 0.3f * std::sin(g_time * 3.f + rd.x * 100.f + rd.z * 200.f);
        star_bright *= twinkle;
        col = cadd(col, {star_bright, star_bright, star_bright * 1.2f});
    }

    return col;
}

// ── Shading ──────────────────────────────────────────────────────────────────

static Color3 shade(vec3 p, vec3 n, vec3 rd, int mat, int depth);

static Color3 get_reflection(vec3 p, vec3 n, vec3 rd, int depth) {
    if (depth > 1) return sky_color(rd);
    vec3 refl_dir = rd - n * (2.f * dot(rd, n));
    vec3 refl_ro = p + n * 0.03f;
    Hit rh = raymarch(refl_ro, refl_dir);
    if (rh.d >= MAX_DIST) return sky_color(refl_dir);
    vec3 rp = refl_ro + refl_dir * rh.d;
    vec3 rn = calc_normal(rp);
    Color3 rc = shade(rp, rn, refl_dir, rh.mat, depth + 1);
    // Fade reflections with distance
    float fog = 1.f - std::exp(-0.02f * rh.d * rh.d);
    return cmix(rc, sky_color(refl_dir), fog);
}

static Color3 shade(vec3 p, vec3 n, vec3 rd, int mat, int depth) {
    // Two lights: warm key + cool fill
    vec3 key_dir = normalize({0.6f, 0.8f, -0.4f});
    vec3 fill_dir = normalize({-0.4f, 0.3f, 0.6f});
    Color3 key_col = {1.2f, 0.95f, 0.7f};   // warm sun
    Color3 fill_col = {0.15f, 0.18f, 0.35f}; // cool sky fill

    float key_diff = std::fmax(dot(n, key_dir), 0.f);
    float fill_diff = std::fmax(dot(n, fill_dir), 0.f);
    float shadow = soft_shadow(p + n * 0.02f, key_dir, 0.02f, 12.f, 16.f);
    float ao = calc_ao(p, n);

    // Specular (Blinn-Phong)
    vec3 half_v = normalize(key_dir - rd);
    float ndh = std::fmax(dot(n, half_v), 0.f);

    // Fresnel for reflectivity
    float ndv = std::fmax(dot(n, rd * -1.f), 0.f);

    Color3 albedo;
    float roughness = 0.5f;
    float metallic = 0.f;
    float emissive = 0.f;

    if (mat == 0) {
        // Reflective checkerboard ground
        int cx = (int)std::floor(p.x + 0.001f) + (int)std::floor(p.z + 0.001f);
        if (cx & 1) albedo = {0.40f, 0.38f, 0.42f};
        else        albedo = {0.10f, 0.10f, 0.12f};
        roughness = 0.3f;
        metallic = 0.1f;
    } else if (mat == 1) {
        // Chrome/glass sphere — highly reflective
        if (g_scene == 1) {
            // Metaballs: iridescent color based on normal
            float t = g_time * 0.3f;
            albedo = {
                0.3f + 0.3f * std::sin(n.x * 3.f + t),
                0.3f + 0.3f * std::sin(n.y * 3.f + t + 2.1f),
                0.5f + 0.3f * std::sin(n.z * 3.f + t + 4.2f)
            };
            metallic = 0.7f;
            roughness = 0.15f;
        } else if (g_scene == 3) {
            // Cathedral: glowing crystal orb
            albedo = {0.4f, 0.7f, 1.0f};
            metallic = 0.9f;
            roughness = 0.05f;
            emissive = 0.6f + 0.3f * std::sin(g_time * 2.f);
        } else {
            albedo = {0.15f, 0.3f, 0.8f};
            metallic = 0.9f;
            roughness = 0.05f;
        }
    } else if (mat == 2) {
        if (g_scene == 2) {
            // Marble columns
            float marble = 0.5f + 0.5f * std::sin(p.y * 5.f + std::sin(p.x * 2.f) * 2.f);
            albedo = {0.75f * marble + 0.2f, 0.72f * marble + 0.18f, 0.68f * marble + 0.15f};
            roughness = 0.4f;
        } else if (g_scene == 3) {
            // Cathedral columns: dark stone
            albedo = {0.25f, 0.22f, 0.28f};
            roughness = 0.7f;
        } else {
            // Gold torus
            albedo = {0.9f, 0.65f, 0.15f};
            metallic = 0.85f;
            roughness = 0.15f;
        }
    } else if (mat == 4) {
        // Emissive
        float pulse = 0.7f + 0.3f * std::sin(g_time * 4.f);
        if (g_scene == 3) {
            albedo = {0.3f * pulse, 0.6f * pulse, 1.0f * pulse};
        } else if (g_scene == 0) {
            albedo = {0.2f * pulse, 0.8f * pulse, 1.0f * pulse};
        } else {
            albedo = {1.0f * pulse, 0.4f * pulse, 0.1f * pulse};
        }
        emissive = 2.0f;
    }

    // Specular power from roughness
    float spec_power = 2.f / (roughness * roughness + 0.001f);
    float spec = std::pow(ndh, spec_power);

    // Fresnel blend
    float f = fresnel(ndv, metallic > 0.5f ? 0.7f : 0.04f);

    // Diffuse contribution
    Color3 diff_light = cadd(
        cmul(key_col, key_diff * shadow),
        cmul(fill_col, fill_diff)
    );
    Color3 diffuse = cmul(cmul(albedo, diff_light), 1.f - f * metallic);

    // Specular contribution (metallic tints specular with albedo)
    Color3 spec_col = metallic > 0.5f ? albedo : Color3{1.f, 1.f, 1.f};
    Color3 specular = cmul(cmul(spec_col, key_col), spec * shadow * f);

    // Ambient
    Color3 ambient = cmul(albedo, 0.08f);

    Color3 col = cadd(cadd(diffuse, specular), ambient);

    // Reflections for metallic/smooth surfaces. The reflected ray depends
    // only on (p, n, rd), so it's traced ONCE and shared: the ground
    // (roughness 0.3) qualifies for this block AND the ground block below,
    // and each used to call get_reflection itself - a second full
    // raymarch + normal + shade + shadow + AO for the same ray, on every
    // ground pixel (the floor is most of the screen in every scene).
    const bool want_refl = (metallic > 0.3f || roughness < 0.35f) && depth < 1;
    const bool want_ground_refl = (mat == 0 && depth < 1);
    Color3 refl{};
    if (want_refl || want_ground_refl) refl = get_reflection(p, n, rd, depth);

    if (want_refl) {
        float refl_strength = f * (1.f - roughness * 0.7f);
        if (metallic > 0.5f) {
            col = cmix(col, cmul(refl, albedo), refl_strength * 0.6f);
        } else {
            col = cmix(col, refl, refl_strength * 0.25f);
        }
    }

    // Ground reflection (subtle)
    if (want_ground_refl) {
        float ground_f = fresnel(ndv, 0.02f);
        col = cmix(col, refl, ground_f * 0.35f);
    }

    // Emissive glow
    if (emissive > 0.f) {
        col = cadd(col, cmul(albedo, emissive));
    }

    // Apply AO
    col = cmul(col, ao);

    return col;
}

// ── Render a single pixel ────────────────────────────────────────────────────

static Color3 render_pixel(vec3 ro, vec3 rd) {
    Hit h = raymarch(ro, rd);
    if (h.d >= MAX_DIST) return sky_color(rd);

    vec3 p = ro + rd * h.d;
    vec3 n = calc_normal(p);
    Color3 col = shade(p, n, rd, h.mat, 0);

    // Distance fog — tinted with warm horizon
    float fog = 1.f - std::exp(-0.012f * h.d * h.d);
    Color3 fog_col = sky_color(rd);
    col = cmix(col, fog_col, fog);

    // Tone mapping (ACES-ish)
    auto tonemap = [](float x) {
        float a = x * (x + 0.0245786f) - 0.000090537f;
        float b = x * (0.983729f * x + 0.4329510f) + 0.238081f;
        return clampf(a / b, 0.f, 1.f);
    };
    col = {tonemap(col.r), tonemap(col.g), tonemap(col.b)};

    return col;
}

// ── Style interning ──────────────────────────────────────────────────────────

static constexpr int Q = 6; // quantization levels (6³×6³ = 46656 < 65535 style pool limit)
static uint16_t g_styles[Q*Q*Q][Q*Q*Q]; // [fg_idx][bg_idx]
static uint16_t g_bar_bg, g_bar_text, g_bar_accent, g_bar_dim;

static uint8_t to8(int qi) {
    return static_cast<uint8_t>(qi * 255 / (Q - 1));
}

static int quantize(float v) {
    return std::clamp(static_cast<int>(v * (Q - 0.01f)), 0, Q - 1);
}

static int color_idx(float r, float g, float b) {
    return quantize(r) * Q * Q + quantize(g) * Q + quantize(b);
}

static void intern_styles(StylePool& pool, int /*w*/, int /*h*/) {
    for (int fi = 0; fi < Q*Q*Q; ++fi) {
        int fr = fi / (Q*Q), fg = (fi / Q) % Q, fb = fi % Q;
        for (int bi = 0; bi < Q*Q*Q; ++bi) {
            int br = bi / (Q*Q), bg = (bi / Q) % Q, bb = bi % Q;
            g_styles[fi][bi] = pool.intern(
                Style{}.with_fg(Color::rgb(to8(fr), to8(fg), to8(fb)))
                       .with_bg(Color::rgb(to8(br), to8(bg), to8(bb))));
        }
    }
    g_bar_bg     = pool.intern(Style{}.with_fg(Color::rgb(180,180,180)).with_bg(Color::rgb(15,12,20)));
    g_bar_text   = pool.intern(Style{}.with_fg(Color::rgb(200,200,200)).with_bg(Color::rgb(15,12,20)));
    g_bar_accent = pool.intern(Style{}.with_fg(Color::rgb(255,160,60)).with_bg(Color::rgb(15,12,20)).with_bold());
    g_bar_dim    = pool.intern(Style{}.with_fg(Color::rgb(80,60,90)).with_bg(Color::rgb(15,12,20)));
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main() {
    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();

    return run_canvas(
        CanvasConfig{.fps = 30, .mouse = false, .mode = Mode::Fullscreen, .auto_clear = false, .title = "RAYMARCH"},

        // on_resize
        intern_styles,

        // on_event
        [&](const Event& ev) -> bool {
            if (key(ev, 'q') || key(ev, SpecialKey::Escape)) return false;
            on(ev, ' ', [] { g_paused = !g_paused; });
            on(ev, '1', [] { g_scene = 0; });
            on(ev, '2', [] { g_scene = 1; });
            on(ev, '3', [] { g_scene = 2; });
            on(ev, '4', [] { g_scene = 3; });
            on(ev, SpecialKey::Left,  [] { g_orbit_speed -= 0.1f; });
            on(ev, SpecialKey::Right, [] { g_orbit_speed += 0.1f; });
            on(ev, SpecialKey::Up,    [] { g_pitch = std::fmin(g_pitch + 0.1f, 1.2f); });
            on(ev, SpecialKey::Down,  [] { g_pitch = std::fmax(g_pitch - 0.1f, -0.2f); });
            return true;
        },

        // on_paint
        [&](Canvas& canvas, int W, int H) {
            auto now = Clock::now();
            float dt = std::chrono::duration<float>(now - last).count();
            last = now;
            dt = std::fmin(dt, 0.1f);
            if (!g_paused) { g_time += dt; g_elapsed += dt; }
            g_frame++;

            if (W < 10 || H < 5) return;
            update_frame_consts();   // once per frame, never per ray sample

            int bar_y = H - 1;
            int canvas_h = H - 1;
            int pixel_h = canvas_h * 2;
            int pixel_w = W;

            // Camera
            float angle = g_time * g_orbit_speed;
            float cam_r = g_scene == 3 ? 8.f : 6.f;
            float cam_y = 2.5f + std::sin(g_time * 0.2f) * 0.8f;
            vec3 ro = {std::cos(angle) * cam_r, cam_y, std::sin(angle) * cam_r};
            vec3 target = {0.f, g_scene == 3 ? 2.5f : 1.f, 0.f};

            // Camera basis
            vec3 fwd = normalize(target - ro);
            vec3 right = normalize({fwd.z, 0.f, -fwd.x});
            vec3 up = {
                right.y * fwd.z - right.z * fwd.y,
                right.z * fwd.x - right.x * fwd.z,
                right.x * fwd.y - right.y * fwd.x
            };
            float cp = std::cos(g_pitch), sp = std::sin(g_pitch);
            vec3 fwd2 = fwd * cp + up * sp;
            vec3 up2  = up * cp - fwd * sp;

            float aspect = static_cast<float>(pixel_w) / static_cast<float>(pixel_h);
            float fov = 1.2f;

            // Parallel ray tracing — split rows across hardware threads.
            // Each thread writes to non-overlapping canvas rows, so no
            // synchronization is needed. Style lookups use the pre-interned
            // g_styles table (read-only), so this is fully data-race-free.
            static const int n_threads = std::max(1, static_cast<int>(
                std::thread::hardware_concurrency()));

            auto trace_rows = [&](int first, int stride) {
                for (int cy = first; cy < canvas_h; cy += stride) {
                    for (int cx = 0; cx < pixel_w; ++cx) {
                        int py_top = cy * 2;
                        float u = (2.f * (cx + 0.5f) / pixel_w - 1.f) * aspect * fov;
                        float v_top = (1.f - 2.f * (py_top + 0.5f) / pixel_h) * fov;
                        vec3 rd_top = normalize(fwd2 + right * u + up2 * v_top);
                        Color3 c_top = render_pixel(ro, rd_top);

                        float v_bot = (1.f - 2.f * (py_top + 1.5f) / pixel_h) * fov;
                        vec3 rd_bot = normalize(fwd2 + right * u + up2 * v_bot);
                        Color3 c_bot = render_pixel(ro, rd_bot);

                        int fi = color_idx(c_top.r, c_top.g, c_top.b);
                        int bi = color_idx(c_bot.r, c_bot.g, c_bot.b);
                        canvas.set(cx, cy, U'\u2580', g_styles[fi][bi]);
                    }
                }
            };

            // Rows INTERLEAVED across threads, and this thread takes a share:
            // contiguous bands left the threads with cheap sky rows idle
            // (__ulock_wait) while the ones with the scene finished the frame.
            if (n_threads <= 1 || canvas_h < 4) {
                trace_rows(0, 1);
            } else {
                std::vector<std::jthread> threads;
                threads.reserve(static_cast<size_t>(n_threads - 1));
                for (int t = 1; t < n_threads; ++t)
                    threads.emplace_back([=, &trace_rows] { trace_rows(t, n_threads); });
                trace_rows(0, n_threads);
                // jthread destructor joins automatically
            }

            // Status bar
            int fps = g_elapsed > 0.5f ? static_cast<int>(g_frame / g_elapsed) : 0;
            static const char* scene_names[] = {"sphere+torus", "metaballs", "columns", "cathedral"};
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                " RAYMARCH \xe2\x94\x82 fps:%d \xe2\x94\x82 %s \xe2\x94\x82 [\xe2\x86\x90\xe2\x86\x92] orbit [\xe2\x86\x91\xe2\x86\x93] pitch [1-4] scene [spc] %s ",
                fps, scene_names[g_scene], g_paused ? "resume" : "pause");
            for (int x = 0; x < W; ++x)
                canvas.set(x, bar_y, U' ', g_bar_bg);
            canvas.write_text(0, bar_y, buf, g_bar_accent);
        }
    );
}
