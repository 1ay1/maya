// examples/raymarch.cpp — a real-time SDF raymarcher.
//
// Reflective surfaces, a sunset sky with stars, fresnel, tone mapping and
// coloured lights, in four scenes.
//
//   Model      the scene, the clock, the orbit speed and camera pitch.
//   update()   Tick advances the clock (unless paused); keys pick a scene,
//              orbit, tilt.
//   view()     every pixel is a ray: a pure function of the model, traced
//              with Image::fill_rows (rows across all cores); plus a bar.
//
// Keys: 1-4 scene   left/right orbit speed   up/down pitch   space pause   q quit

#include <maya/app.hpp>
#include <maya/element/pixels.hpp>
#include <maya/maya.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <variant>

using namespace maya;
using namespace maya::dsl;
using namespace std::chrono_literals;

namespace {

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


// Material IDs: 0=ground, 1=primary, 2=secondary, 3=sky, 4=emissive
struct Hit { float d; int mat; };

// ── Per-frame scene constants ────────────────────────────────────────────────
// Everything that depends only on the time (object positions, rotation
// sin/cos) is computed ONCE per frame here. The scene functions below run for
// every march step of every ray, 60+ times per pixel: evaluating the same
// sin(time * k) there put sinf/sincosf at the top of the profile (175% CPU).
struct Rot { float c, s; };
static Rot rot_of(float a) { return {std::cos(a), std::sin(a)}; }
inline vec3 rot_y(vec3 p, Rot r) { return {p.x*r.c + p.z*r.s, p.y, -p.x*r.s + p.z*r.c}; }
inline vec3 rot_x(vec3 p, Rot r) { return {p.x, p.y*r.c - p.z*r.s, p.y*r.s + p.z*r.c}; }
inline vec3 rot_z(vec3 p, Rot r) { return {p.x*r.c - p.y*r.s, p.x*r.s + p.y*r.c, p.z}; }

// Everything a ray needs to know about this frame: which scene, the time,
// and the animated positions computed once (never per ray sample).
struct Scene {
    int id; float time;
    // classic
    float bob; Rot torus_y, torus_x, ring_y;
    // metaballs
    vec3 m1, m2, m3, m4;
    // columns
    vec3 orb_c, p1, p2;
    // cathedral
    vec3 cols[8]; float orb_y; Rot r1_y, r1_x, r2_y, r2_z;
};

static Scene make_scene(int id, float T) {
    Scene sc{};
    sc.id = id; sc.time = T;
    sc.bob = std::sin(T * 1.8f) * 0.5f + 1.5f;
    sc.torus_y = rot_of(T * 0.6f);
    sc.torus_x = rot_of(T * 0.4f);
    sc.ring_y  = rot_of(-T * 1.2f);
    const float t = T * 0.8f;
    sc.m1 = {std::sin(t)*1.5f, 1.2f + std::sin(t*1.3f)*0.4f, std::cos(t)*1.5f};
    sc.m2 = {std::cos(t*0.7f)*1.8f, 1.5f + std::cos(t*1.1f)*0.3f, std::sin(t*0.9f)*1.8f};
    sc.m3 = {0, 1.0f + std::sin(t*1.6f)*0.6f, 0};
    sc.m4 = {std::sin(t*1.1f)*1.2f, 0.8f + std::cos(t*0.8f)*0.5f, std::cos(t*1.4f)*1.2f};
    sc.orb_c = {0, 2.f + std::sin(T)*0.5f, 0};
    sc.p1 = {std::sin(T*1.5f)*3.f, 1.5f + std::cos(T*2.f)*0.5f, std::cos(T*1.5f)*3.f};
    sc.p2 = {std::cos(T*1.2f)*2.5f, 2.5f + std::sin(T*1.7f)*0.3f, std::sin(T*1.2f)*2.5f};
    for (int i = 0; i < 8; ++i) {
        const float a = float(i) * TAU / 8.f;
        sc.cols[i] = {std::cos(a) * 4.f, 0, std::sin(a) * 4.f};
    }
    sc.orb_y = 3.f + std::sin(T * 0.7f) * 0.3f;
    sc.r1_y = rot_of(T * 0.5f);  sc.r1_x = rot_of(PI * 0.3f);
    sc.r2_y = rot_of(-T * 0.4f); sc.r2_z = rot_of(PI * 0.4f);
    return sc;
}

static Hit scene_classic(const Scene& S, vec3 p) {
    float ground = sd_plane(p, {0,1,0}, 0.f);
    float sphere = sd_sphere(p - vec3{0, S.bob, 0}, 1.0f);
    vec3 tp = rot_y(p - vec3{0, 1.2f, 0}, S.torus_y);
    tp = rot_x(tp, S.torus_x);
    float torus = sd_torus(tp, 2.0f, 0.35f);

    // Floating crystal ring
    vec3 rp = rot_y(p - vec3{0, S.bob, 0}, S.ring_y);
    float ring = sd_torus(rp, 1.8f, 0.06f);

    Hit h = {ground, 0};
    if (sphere < h.d) h = {sphere, 1};
    if (torus < h.d)  h = {torus, 2};
    if (ring < h.d)   h = {ring, 4};
    return h;
}

static Hit scene_metaballs(const Scene& S, vec3 p) {
    float ground = sd_plane(p, {0,1,0}, 0.f);
    float s1 = sd_sphere(p - S.m1, 0.8f);
    float s2 = sd_sphere(p - S.m2, 0.7f);
    float s3 = sd_sphere(p - S.m3, 0.9f);
    float s4 = sd_sphere(p - S.m4, 0.5f);
    float blob = smooth_union(smooth_union(s1, s2, 0.8f), smooth_union(s3, s4, 0.8f), 0.8f);

    Hit h = {ground, 0};
    if (blob < h.d) h = {blob, 1};
    return h;
}

static Hit scene_columns(const Scene& S, vec3 p) {
    float ground = sd_plane(p, {0,1,0}, 0.f);
    // Infinite repeating columns
    vec3 rp = p;
    rp.x = std::fmod(std::fabs(rp.x) + 2.f, 4.f) - 2.f;
    rp.z = std::fmod(std::fabs(rp.z) + 2.f, 4.f) - 2.f;
    float col = sd_box(rp - vec3{0, 2.5f, 0}, {0.3f, 2.5f, 0.3f});
    // Floating orb
    float orb = sd_sphere(p - S.orb_c, 0.6f);
    // Orbiting emissive particles
    float p1 = sd_sphere(p - S.p1, 0.15f);
    float p2 = sd_sphere(p - S.p2, 0.12f);

    Hit h = {ground, 0};
    if (col < h.d) h = {col, 2};
    if (orb < h.d) h = {orb, 1};
    if (p1 < h.d)  h = {p1, 4};
    if (p2 < h.d)  h = {p2, 4};
    return h;
}

static Hit scene_cathedral(const Scene& S, vec3 p) {
    float ground = sd_plane(p, {0,1,0}, 0.f);

    // Tall arched columns in a circle
    float cols = MAX_DIST;
    for (int i = 0; i < 8; ++i) {
        const vec3 cp = S.cols[i];
        float c = sd_capsule(p, cp, cp + vec3{0, 5.f, 0}, 0.2f);
        cols = std::fmin(cols, c);
    }

    // Central glowing orb
    float orb = sd_sphere(p - vec3{0, S.orb_y, 0}, 0.8f);

    // Rotating rings around orb
    vec3 r1p = rot_y(p - vec3{0, 3.f, 0}, S.r1_y);
    r1p = rot_x(r1p, S.r1_x);
    float ring1 = sd_torus(r1p, 1.8f, 0.05f);
    vec3 r2p = rot_y(p - vec3{0, 3.f, 0}, S.r2_y);
    r2p = rot_z(r2p, S.r2_z);
    float ring2 = sd_torus(r2p, 2.2f, 0.04f);

    Hit h = {ground, 0};
    if (cols < h.d)  h = {cols, 2};
    if (orb < h.d)   h = {orb, 1};
    if (ring1 < h.d) h = {ring1, 4};
    if (ring2 < h.d) h = {ring2, 4};
    return h;
}

static Hit map(const Scene& S, vec3 p) {
    switch (S.id) {
        case 1:  return scene_metaballs(S, p);
        case 2:  return scene_columns(S, p);
        case 3:  return scene_cathedral(S, p);
        default: return scene_classic(S, p);
    }
}

static float map_dist(const Scene& S, vec3 p) { return map(S, p).d; }

// ── Raymarching ──────────────────────────────────────────────────────────────

static Hit raymarch(const Scene& S, vec3 ro, vec3 rd) {
    float t = 0.f;
    Hit h = {MAX_DIST, 3};
    for (int i = 0; i < MAX_STEPS && t < MAX_DIST; ++i) {
        vec3 p = ro + rd * t;
        Hit s = map(S, p);
        if (s.d < EPS) { h = {t, s.mat}; break; }
        t += s.d;
    }
    h.d = t;
    return h;
}

static vec3 calc_normal(const Scene& S, vec3 p) {
    const float e = 0.001f;
    float d = map_dist(S, p);
    return normalize({
        map_dist(S, p + vec3{e,0,0}) - d,
        map_dist(S, p + vec3{0,e,0}) - d,
        map_dist(S, p + vec3{0,0,e}) - d
    });
}

// ── Lighting ─────────────────────────────────────────────────────────────────

static float soft_shadow(const Scene& S, vec3 ro, vec3 rd, float mint, float maxt, float k) {
    float res = 1.f;
    float t = mint;
    for (int i = 0; i < 20 && t < maxt; ++i) {
        float d = map_dist(S, ro + rd * t);
        if (d < EPS) return 0.f;
        res = std::fmin(res, k * d / t);
        t += std::fmax(d, 0.02f);
    }
    return std::fmax(res, 0.f);
}

static float calc_ao(const Scene& S, vec3 p, vec3 n) {
    float occ = 0.f;
    float scale = 1.f;
    for (int i = 0; i < 3; ++i) {
        float h = 0.01f + 0.15f * float(i);
        float d = map_dist(S, p + n * h);
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

static Color3 sky_color(const Scene& S, vec3 rd) {
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
        float twinkle = 0.7f + 0.3f * std::sin(S.time * 3.f + rd.x * 100.f + rd.z * 200.f);
        star_bright *= twinkle;
        col = cadd(col, {star_bright, star_bright, star_bright * 1.2f});
    }

    return col;
}

// ── Shading ──────────────────────────────────────────────────────────────────

static Color3 shade(const Scene& S, vec3 p, vec3 n, vec3 rd, int mat, int depth);

static Color3 get_reflection(const Scene& S, vec3 p, vec3 n, vec3 rd, int depth) {
    if (depth > 1) return sky_color(S, rd);
    vec3 refl_dir = rd - n * (2.f * dot(rd, n));
    vec3 refl_ro = p + n * 0.03f;
    Hit rh = raymarch(S, refl_ro, refl_dir);
    if (rh.d >= MAX_DIST) return sky_color(S, refl_dir);
    vec3 rp = refl_ro + refl_dir * rh.d;
    vec3 rn = calc_normal(S, rp);
    Color3 rc = shade(S, rp, rn, refl_dir, rh.mat, depth + 1);
    // Fade reflections with distance
    float fog = 1.f - std::exp(-0.02f * rh.d * rh.d);
    return cmix(rc, sky_color(S, refl_dir), fog);
}

static Color3 shade(const Scene& S, vec3 p, vec3 n, vec3 rd, int mat, int depth) {
    // Two lights: warm key + cool fill
    vec3 key_dir = normalize({0.6f, 0.8f, -0.4f});
    vec3 fill_dir = normalize({-0.4f, 0.3f, 0.6f});
    Color3 key_col = {1.2f, 0.95f, 0.7f};   // warm sun
    Color3 fill_col = {0.15f, 0.18f, 0.35f}; // cool sky fill

    float key_diff = std::fmax(dot(n, key_dir), 0.f);
    float fill_diff = std::fmax(dot(n, fill_dir), 0.f);
    float shadow = soft_shadow(S, p + n * 0.02f, key_dir, 0.02f, 12.f, 16.f);
    float ao = calc_ao(S, p, n);

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
        if (S.id == 1) {
            // Metaballs: iridescent color based on normal
            float t = S.time * 0.3f;
            albedo = {
                0.3f + 0.3f * std::sin(n.x * 3.f + t),
                0.3f + 0.3f * std::sin(n.y * 3.f + t + 2.1f),
                0.5f + 0.3f * std::sin(n.z * 3.f + t + 4.2f)
            };
            metallic = 0.7f;
            roughness = 0.15f;
        } else if (S.id == 3) {
            // Cathedral: glowing crystal orb
            albedo = {0.4f, 0.7f, 1.0f};
            metallic = 0.9f;
            roughness = 0.05f;
            emissive = 0.6f + 0.3f * std::sin(S.time * 2.f);
        } else {
            albedo = {0.15f, 0.3f, 0.8f};
            metallic = 0.9f;
            roughness = 0.05f;
        }
    } else if (mat == 2) {
        if (S.id == 2) {
            // Marble columns
            float marble = 0.5f + 0.5f * std::sin(p.y * 5.f + std::sin(p.x * 2.f) * 2.f);
            albedo = {0.75f * marble + 0.2f, 0.72f * marble + 0.18f, 0.68f * marble + 0.15f};
            roughness = 0.4f;
        } else if (S.id == 3) {
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
        float pulse = 0.7f + 0.3f * std::sin(S.time * 4.f);
        if (S.id == 3) {
            albedo = {0.3f * pulse, 0.6f * pulse, 1.0f * pulse};
        } else if (S.id == 0) {
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
    if (want_refl || want_ground_refl) refl = get_reflection(S, p, n, rd, depth);

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

static Color3 render_pixel(const Scene& S, vec3 ro, vec3 rd) {
    Hit h = raymarch(S, ro, rd);
    if (h.d >= MAX_DIST) return sky_color(S, rd);

    vec3 p = ro + rd * h.d;
    vec3 n = calc_normal(S, p);
    Color3 col = shade(S, p, n, rd, h.mat, 0);

    // Distance fog — tinted with warm horizon
    float fog = 1.f - std::exp(-0.012f * h.d * h.d);
    Color3 fog_col = sky_color(S, rd);
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

// ── program ──────────────────────────────────────────────────────────────────

constexpr std::array<const char*, 4> kSceneNames = {"sphere+torus", "metaballs", "columns", "cathedral"};

struct Model {
    int w = 0, h = 0;           // the picture, in pixels (h = 2 * rows)
    int scene = 0;
    float time = 0.f;
    float orbit_speed = 0.3f;
    float pitch = 0.3f;
    bool paused = false;
};

struct Tick {};
struct Resize   { int cols, rows; };
struct SetScene { int id; };
struct Orbit    { float by; };
struct Pitch    { float by; };
struct Pause {};
struct Quit {};
using Msg = std::variant<Tick, Resize, SetScene, Orbit, Pitch, Pause, Quit>;

// 5 bits a channel: invisible after tone mapping, and it keeps the number of
// distinct (top, bottom) colour pairs small enough for the style cache.
Rgb to_rgb(Color3 c) {
    auto q = [](float f) { return static_cast<std::uint8_t>(static_cast<int>(std::clamp(f, 0.f, 1.f) * 255.f) & 0xF8); };
    return {q(c.r), q(c.g), q(c.b)};
}

struct Raymarch {
    using Model = ::Model;
    using Msg   = ::Msg;
    using Cmd   = jaal::Cmd<Msg>;
    using Sub   = jaal::Sub<Msg, on_key, on_resize>;

    static constexpr float kDt = 1.f / 30.f;

    static Cmd update(Model& m, Tick)       { m.time += kDt; return {}; }
    static Cmd update(Model& m, Resize r)   { m.w = std::max(1, r.cols); m.h = std::max(2, (r.rows - 1) * 2); return {}; }
    static Cmd update(Model& m, SetScene s) { m.scene = s.id; return {}; }
    static Cmd update(Model& m, Orbit o)    { m.orbit_speed += o.by; return {}; }
    static Cmd update(Model& m, Pitch p)    { m.pitch = std::clamp(m.pitch + p.by, -0.2f, 1.2f); return {}; }
    static Cmd update(Model& m, Pause)      { m.paused = !m.paused; return {}; }
    static Cmd update(Model&, Quit)         { return Cmd::quit(0); }

    static Image render(const Model& m) {
        const Scene S = make_scene(m.scene, m.time);

        const float angle = m.time * m.orbit_speed;
        const float cam_r = m.scene == 3 ? 8.f : 6.f;
        const float cam_y = 2.5f + std::sin(m.time * 0.2f) * 0.8f;
        const vec3 ro = {std::cos(angle) * cam_r, cam_y, std::sin(angle) * cam_r};
        const vec3 target = {0.f, m.scene == 3 ? 2.5f : 1.f, 0.f};

        const vec3 fwd = normalize(target - ro);
        const vec3 right = normalize({fwd.z, 0.f, -fwd.x});
        const vec3 up = {right.y * fwd.z - right.z * fwd.y,
                         right.z * fwd.x - right.x * fwd.z,
                         right.x * fwd.y - right.y * fwd.x};
        const float cp = std::cos(m.pitch), sp = std::sin(m.pitch);
        const vec3 fwd2 = fwd * cp + up * sp;
        const vec3 up2  = up * cp - fwd * sp;

        const float aspect = static_cast<float>(m.w) / static_cast<float>(m.h);
        constexpr float fov = 1.2f;

        Image img(m.w, m.h);
        img.fill_rows([&](int x, int y) {
            const float u = (2.f * (static_cast<float>(x) + 0.5f) / static_cast<float>(m.w) - 1.f) * aspect * fov;
            const float v = (1.f - 2.f * (static_cast<float>(y) + 0.5f) / static_cast<float>(m.h)) * fov;
            return to_rgb(render_pixel(S, ro, normalize(fwd2 + right * u + up2 * v)));
        });
        return img;
    }

    static Element status_bar(const Model& m) {
        const auto bg = Color::rgb(15, 12, 20);
        return h(text(" RAYMARCH ") | fgc(Color::rgb(255, 160, 60)) | Bold,
                 text(std::string("│ ") + kSceneNames[static_cast<std::size_t>(m.scene)] + " │ [←→] orbit [↑↓] pitch [1-4] scene [spc] " +
                      (m.paused ? "resume" : "pause") + " [q] quit") | fgc(Color::rgb(200, 200, 200)),
                 spacer()) | bgc(bg);
    }

    static Element view(const Model& m) {
        if (m.w < 10 || m.h < 8) return text("");
        return v(pixels(render(m)), status_bar(m));
    }

    // Paused, the picture is still: no clock.
    static Sub subscribe(const Model& m) {
        auto input = Sub::batch(
            Sub::on(on_resize{}, [](const ResizeEvent& r) -> std::optional<Msg> {
                return Resize{r.width.value, r.height.value};
            }),
            keys<Sub>({
                {'1', SetScene{0}}, {'2', SetScene{1}}, {'3', SetScene{2}}, {'4', SetScene{3}},
                {SpecialKey::Left, Orbit{-0.1f}}, {SpecialKey::Right, Orbit{0.1f}},
                {SpecialKey::Up, Pitch{0.1f}}, {SpecialKey::Down, Pitch{-0.1f}},
                {' ', Pause{}}, {'q', Quit{}}, {SpecialKey::Escape, Quit{}},
            }));
        if (m.paused) return input;
        return Sub::batch(Sub::every(33ms, Tick{}), std::move(input));
    }
    static bool subs_key(const Model& m) { return m.paused; }
};

static_assert(Program<Raymarch>);

}  // namespace

int main() { return run<Raymarch>({.title = "raymarch"}); }
