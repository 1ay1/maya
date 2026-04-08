// maya — Real-time raymarching 3D renderer
// Half-block SDF renderer: checkerboard ground, bobbing sphere, rotating
// torus, soft shadows, AO. Arrows=orbit, space=pause, 1-3=scene, q=quit

#include <maya/maya.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

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
inline vec3 abs(vec3 v) { return {std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)}; }
inline vec3 max(vec3 a, float b) { return {std::fmax(a.x,b), std::fmax(a.y,b), std::fmax(a.z,b)}; }

// ── SDF primitives ───────────────────────────────────────────────────────────

inline float sd_sphere(vec3 p, float r) { return length(p) - r; }

inline float sd_torus(vec3 p, float r1, float r2) {
    float q = std::sqrt(p.x*p.x + p.z*p.z) - r1;
    return std::sqrt(q*q + p.y*p.y) - r2;
}

inline float sd_plane(vec3 p, vec3 n, float h) { return dot(p, n) + h; }

inline float sd_box(vec3 p, vec3 b) {
    vec3 d = abs(p) - b;
    return length(max(d, 0.f)) + std::fmin(std::fmax(d.x, std::fmax(d.y, d.z)), 0.f);
}

inline float smooth_union(float a, float b, float k) {
    float h = std::fmax(k - std::fabs(a - b), 0.f) / k;
    return std::fmin(a, b) - h * h * k * 0.25f;
}

// ── Rotation ─────────────────────────────────────────────────────────────────

inline vec3 rot_y(vec3 p, float a) {
    float c = std::cos(a), s = std::sin(a);
    return {p.x*c + p.z*s, p.y, -p.x*s + p.z*c};
}
inline vec3 rot_x(vec3 p, float a) {
    float c = std::cos(a), s = std::sin(a);
    return {p.x, p.y*c - p.z*s, p.y*s + p.z*c};
}

// ── Scenes ───────────────────────────────────────────────────────────────────

static int   g_scene    = 0;
static float g_time     = 0.f;
static bool  g_paused   = false;
static float g_orbit_speed = 0.3f;
static float g_pitch    = 0.3f;
static int   g_frame    = 0;
static float g_elapsed  = 0.f;

// Material IDs: 0=ground, 1=sphere, 2=torus, 3=sky
struct Hit { float d; int mat; };

static Hit scene_classic(vec3 p) {
    float ground = sd_plane(p, {0,1,0}, 0.f);
    float bob = std::sin(g_time * 1.8f) * 0.5f + 1.5f;
    float sphere = sd_sphere(p - vec3{0, bob, 0}, 1.0f);
    vec3 tp = rot_y(p - vec3{0, 1.2f, 0}, g_time * 0.6f);
    tp = rot_x(tp, g_time * 0.4f);
    float torus = sd_torus(tp, 2.0f, 0.35f);

    Hit h = {ground, 0};
    if (sphere < h.d) h = {sphere, 1};
    if (torus < h.d)  h = {torus, 2};
    return h;
}

static Hit scene_metaballs(vec3 p) {
    float ground = sd_plane(p, {0,1,0}, 0.f);
    float t = g_time * 0.8f;
    float s1 = sd_sphere(p - vec3{std::sin(t)*1.5f, 1.2f + std::sin(t*1.3f)*0.4f, std::cos(t)*1.5f}, 0.8f);
    float s2 = sd_sphere(p - vec3{std::cos(t*0.7f)*1.8f, 1.5f + std::cos(t*1.1f)*0.3f, std::sin(t*0.9f)*1.8f}, 0.7f);
    float s3 = sd_sphere(p - vec3{0, 1.0f + std::sin(t*1.6f)*0.6f, 0}, 0.9f);
    float blob = smooth_union(smooth_union(s1, s2, 0.8f), s3, 0.8f);

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
    float orb = sd_sphere(p - vec3{0, 2.f + std::sin(g_time)*0.5f, 0}, 0.6f);

    Hit h = {ground, 0};
    if (col < h.d) h = {col, 2};
    if (orb < h.d) h = {orb, 1};
    return h;
}

static Hit map(vec3 p) {
    switch (g_scene) {
        case 1:  return scene_metaballs(p);
        case 2:  return scene_columns(p);
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
    for (int i = 0; i < 24 && t < maxt; ++i) {
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
    for (int i = 0; i < 5; ++i) {
        float h = 0.01f + 0.12f * float(i);
        float d = map_dist(p + n * h);
        occ += (h - d) * scale;
        scale *= 0.95f;
    }
    return std::fmax(1.f - 3.f * occ, 0.f);
}

struct Color3 { float r, g, b; };

static Color3 shade(vec3 p, vec3 n, vec3 rd, int mat) {
    vec3 light_dir = normalize({0.6f, 0.8f, -0.4f});
    float diff = std::fmax(dot(n, light_dir), 0.f);
    float shadow = soft_shadow(p + n * 0.02f, light_dir, 0.02f, 10.f, 16.f);
    float ao = calc_ao(p, n);

    // Specular
    vec3 refl = rd - n * (2.f * dot(rd, n));
    float spec = std::pow(std::fmax(dot(refl, light_dir), 0.f), 32.f);

    Color3 albedo;
    if (mat == 0) {
        // Checkerboard ground
        int cx = (int)std::floor(p.x) + (int)std::floor(p.z);
        if (cx & 1) albedo = {0.35f, 0.35f, 0.38f};
        else        albedo = {0.15f, 0.15f, 0.17f};
        spec *= 0.1f;
    } else if (mat == 1) {
        // Metallic blue sphere
        albedo = {0.15f, 0.3f, 0.7f};
        spec *= 1.5f;
    } else {
        // Orange/gold torus
        albedo = {0.8f, 0.5f, 0.1f};
        spec *= 0.8f;
    }

    float ambient = 0.12f;
    float lit = ambient + diff * shadow * 0.85f;
    return {
        std::fmin((albedo.r * lit + spec * shadow) * ao, 1.f),
        std::fmin((albedo.g * lit + spec * shadow) * ao, 1.f),
        std::fmin((albedo.b * lit + spec * shadow) * ao, 1.f)
    };
}

// ── Sky ──────────────────────────────────────────────────────────────────────

static Color3 sky_color(vec3 rd) {
    float t = std::fmax(rd.y * 0.5f + 0.5f, 0.f);
    return {
        0.02f + 0.04f * t,
        0.02f + 0.06f * t,
        0.05f + 0.15f * t
    };
}

// ── Render a single pixel ────────────────────────────────────────────────────

static Color3 render_pixel(vec3 ro, vec3 rd) {
    Hit h = raymarch(ro, rd);
    if (h.d >= MAX_DIST) return sky_color(rd);

    vec3 p = ro + rd * h.d;
    vec3 n = calc_normal(p);
    Color3 col = shade(p, n, rd, h.mat);

    // Distance fog
    float fog = 1.f - std::exp(-0.015f * h.d * h.d);
    Color3 fog_col = sky_color(rd);
    col.r = col.r * (1.f - fog) + fog_col.r * fog;
    col.g = col.g * (1.f - fog) + fog_col.g * fog;
    col.b = col.b * (1.f - fog) + fog_col.b * fog;
    return col;
}

// ── Style interning ──────────────────────────────────────────────────────────

// Quantize colors to reduce style count: 6 levels per channel = 216 combos
// For half-block we need fg+bg combos. We pre-intern a palette and map at paint.
static constexpr int CLEVELS = 16;
static uint16_t g_half_styles[CLEVELS][CLEVELS][CLEVELS][CLEVELS][CLEVELS][CLEVELS]; // too large!

// Instead: quantize each color to an index, intern fg/bg combos on demand.
// Better approach: use a flat palette and intern fg+bg pairs.
// Actually, simplest: quantize to 6 levels per channel, intern all fg combos with bg combos.
// 6^3 = 216 fg colors, 216 bg colors = 46656 styles. That's fine for StylePool.

static constexpr int Q = 6; // quantization levels
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
    // Pre-intern all Q^3 x Q^3 fg/bg combos
    for (int fi = 0; fi < Q*Q*Q; ++fi) {
        int fr = fi / (Q*Q), fg = (fi / Q) % Q, fb = fi % Q;
        for (int bi = 0; bi < Q*Q*Q; ++bi) {
            int br = bi / (Q*Q), bg = (bi / Q) % Q, bb = bi % Q;
            g_styles[fi][bi] = pool.intern(
                Style{}.with_fg(Color::rgb(to8(fr), to8(fg), to8(fb)))
                       .with_bg(Color::rgb(to8(br), to8(bg), to8(bb))));
        }
    }
    g_bar_bg     = pool.intern(Style{}.with_fg(Color::rgb(180,180,180)).with_bg(Color::rgb(20,20,30)));
    g_bar_text   = pool.intern(Style{}.with_fg(Color::rgb(200,200,200)).with_bg(Color::rgb(20,20,30)));
    g_bar_accent = pool.intern(Style{}.with_fg(Color::rgb(80,180,255)).with_bg(Color::rgb(20,20,30)).with_bold());
    g_bar_dim    = pool.intern(Style{}.with_fg(Color::rgb(80,80,100)).with_bg(Color::rgb(20,20,30)));
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main() {
    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();

    (void)canvas_run(
        CanvasConfig{.fps = 30, .mouse = false, .mode = Mode::Fullscreen, .title = "RAYMARCH"},

        // on_resize
        intern_styles,

        // on_event
        [&](const Event& ev) -> bool {
            if (key(ev, 'q') || key(ev, SpecialKey::Escape)) return false;
            on(ev, ' ', [] { g_paused = !g_paused; });
            on(ev, '1', [] { g_scene = 0; });
            on(ev, '2', [] { g_scene = 1; });
            on(ev, '3', [] { g_scene = 2; });
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

            int bar_y = H - 1;
            int canvas_h = H - 1;  // rows for 3D rendering
            int pixel_h = canvas_h * 2; // half-block doubles vertical resolution
            int pixel_w = W;

            // Camera
            float angle = g_time * g_orbit_speed;
            float cam_r = 6.f;
            float cam_y = 2.5f + std::sin(g_time * 0.2f) * 0.8f;
            vec3 ro = {std::cos(angle) * cam_r, cam_y, std::sin(angle) * cam_r};
            vec3 target = {0.f, 1.f, 0.f};

            // Camera basis
            vec3 fwd = normalize(target - ro);
            vec3 right = normalize({fwd.z, 0.f, -fwd.x});
            vec3 up = {
                right.y * fwd.z - right.z * fwd.y,
                right.z * fwd.x - right.x * fwd.z,
                right.x * fwd.y - right.y * fwd.x
            };
            // Apply pitch
            float cp = std::cos(g_pitch), sp = std::sin(g_pitch);
            vec3 fwd2 = fwd * cp + up * sp;
            vec3 up2  = up * cp - fwd * sp;

            float aspect = static_cast<float>(pixel_w) / static_cast<float>(pixel_h);
            float fov = 1.2f;

            // Render pixels row by row, two per terminal row
            for (int cy = 0; cy < canvas_h; ++cy) {
                for (int cx = 0; cx < pixel_w; ++cx) {
                    // Top pixel
                    int py_top = cy * 2;
                    float u_top = (2.f * (cx + 0.5f) / pixel_w - 1.f) * aspect * fov;
                    float v_top = (1.f - 2.f * (py_top + 0.5f) / pixel_h) * fov;
                    vec3 rd_top = normalize(fwd2 + right * u_top + up2 * v_top);
                    Color3 c_top = render_pixel(ro, rd_top);

                    // Bottom pixel
                    int py_bot = cy * 2 + 1;
                    float v_bot = (1.f - 2.f * (py_bot + 0.5f) / pixel_h) * fov;
                    vec3 rd_bot = normalize(fwd2 + right * u_top + up2 * v_bot);
                    Color3 c_bot = render_pixel(ro, rd_bot);

                    int fi = color_idx(c_top.r, c_top.g, c_top.b);
                    int bi = color_idx(c_bot.r, c_bot.g, c_bot.b);
                    canvas.set(cx, cy, U'\u2580', g_styles[fi][bi]); // ▀
                }
            }

            // Status bar
            int fps = g_elapsed > 0.5f ? static_cast<int>(g_frame / g_elapsed) : 0;
            static const char* scene_names[] = {"sphere+torus", "metaballs", "columns"};
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                " RAYMARCH \xe2\x94\x82 fps:%d \xe2\x94\x82 scene:%s \xe2\x94\x82 [\xe2\x86\x90\xe2\x86\x92] orbit [\xe2\x86\x91\xe2\x86\x93] pitch [1-3] scene [spc] %s [q] quit ",
                fps, scene_names[g_scene], g_paused ? "resume" : "pause");
            // Fill bar background
            for (int x = 0; x < W; ++x)
                canvas.set(x, bar_y, U' ', g_bar_bg);
            // Write bar text
            canvas.write_text(0, bar_y, buf, g_bar_accent);
        }
    );
}
