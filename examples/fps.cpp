// maya — FPS: Wolfenstein-style raycaster
//
// DDA raycasting engine with procedural brick textures, torch lighting,
// gradient sky, stone floor tiles, vignette, weapon view, enemy sprites,
// ambient occlusion, and color grading. Half-block at 2x vertical res.
//
// Keys: WASD/arrows=move  ,/.=turn  space=shoot  m=minimap  q/Esc=quit

#include <maya/internal.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

using namespace maya;

// ── Math ────────────────────────────────────────────────────────────────────

static constexpr float PI  = 3.14159265f;

static float clampf(float x, float lo, float hi) { return std::fmin(std::fmax(x, lo), hi); }
static float lerp(float a, float b, float t) { return a + (b - a) * t; }
static float smoothstep(float e0, float e1, float x) {
    float t = clampf((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

struct Col3 { float r, g, b; };
static Col3 col_add(Col3 a, Col3 b) { return {a.r+b.r, a.g+b.g, a.b+b.b}; }
static Col3 col_mul(Col3 a, float s) { return {a.r*s, a.g*s, a.b*s}; }
static Col3 col_lerp(Col3 a, Col3 b, float t) { return {lerp(a.r,b.r,t), lerp(a.g,b.g,t), lerp(a.b,b.b,t)}; }
static Col3 col_clamp(Col3 c) { return {clampf(c.r,0,1), clampf(c.g,0,1), clampf(c.b,0,1)}; }

// Simple hash for procedural textures
static float hash(float x, float y) {
    float h = std::sin(x * 127.1f + y * 311.7f) * 43758.5453f;
    return h - std::floor(h);
}

// ── Map ─────────────────────────────────────────────────────────────────────

static constexpr int MAP_W = 24;
static constexpr int MAP_H = 24;

// 0=empty, 1-5=wall, 6=door, 9=exit
// clang-format off
static int g_map[MAP_H][MAP_W] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,2,2,2,0,0,0,3,0,0,0,0,3,0,0,0,4,4,4,0,0,1},
    {1,0,0,2,0,0,0,0,0,3,0,0,0,0,3,0,0,0,0,0,4,0,0,1},
    {1,0,0,2,0,0,0,0,0,3,0,0,0,0,3,0,0,0,0,0,4,0,0,1},
    {1,0,0,0,0,0,0,0,0,3,3,6,3,3,3,0,0,0,0,0,4,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,4,4,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,5,5,5,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,5,0,5,0,0,2,0,0,0,2,0,0,0,0,0,3,3,3,3,3,0,0,1},
    {1,5,0,5,0,0,2,0,0,0,2,0,0,0,0,0,3,0,0,0,0,0,0,1},
    {1,5,0,0,0,0,2,0,0,0,2,0,0,0,0,0,3,0,0,0,0,0,0,1},
    {1,5,0,5,0,0,2,2,6,2,2,0,0,0,0,0,3,0,0,5,5,5,0,1},
    {1,5,5,5,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,5,0,5,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,5,9,5,0,1},
    {1,0,0,0,0,4,4,4,4,0,0,0,0,4,4,4,4,0,0,5,5,5,0,1},
    {1,0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,4,0,0,0,0,0,0,1},
    {1,0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,4,0,0,0,0,0,0,1},
    {1,0,0,0,0,4,0,0,0,0,3,3,0,0,0,0,4,0,0,0,0,0,0,1},
    {1,0,0,0,0,4,0,0,0,0,3,3,0,0,0,0,4,0,0,0,0,0,0,1},
    {1,0,0,0,0,4,4,4,4,0,0,0,0,4,4,4,4,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
};
// clang-format on

// ── Torch lights ────────────────────────────────────────────────────────────

struct Torch {
    float x, y;
    Col3  color;
    float intensity;
};

static const Torch g_torches[] = {
    {1.5f,  1.5f,  {1.0f, 0.7f, 0.3f}, 4.0f},
    {11.5f, 1.5f,  {1.0f, 0.7f, 0.3f}, 4.0f},
    {22.5f, 1.5f,  {0.6f, 0.7f, 1.0f}, 3.5f},
    {1.5f,  8.5f,  {1.0f, 0.7f, 0.3f}, 4.0f},
    {22.5f, 8.5f,  {1.0f, 0.6f, 0.2f}, 4.0f},
    {11.5f, 11.5f, {1.0f, 0.8f, 0.4f}, 5.0f},
    {1.5f,  22.5f, {1.0f, 0.7f, 0.3f}, 4.0f},
    {22.5f, 22.5f, {0.4f, 0.8f, 0.5f}, 3.5f},
    {11.5f, 17.5f, {1.0f, 0.5f, 0.2f}, 4.5f},
    {20.5f, 15.5f, {1.0f, 0.2f, 0.1f}, 6.0f}, // exit glow
};
static constexpr int NUM_TORCHES = sizeof(g_torches) / sizeof(g_torches[0]);

// ── Wall base colors ────────────────────────────────────────────────────────

struct WallMat {
    Col3 base;        // base color
    Col3 mortar;      // mortar/gap color
    float brick_w;    // brick width in UV
    float brick_h;    // brick height in UV
    float roughness;  // surface noise
};

static const WallMat WALL_MATS[] = {
    {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 1, 1, 0},           // 0: unused
    {{0.45f,0.44f,0.48f},{0.20f,0.20f,0.22f}, 0.25f,0.125f, 0.12f},// 1: gray stone
    {{0.60f,0.25f,0.18f},{0.15f,0.12f,0.10f}, 0.25f,0.125f, 0.08f},// 2: red brick
    {{0.25f,0.42f,0.22f},{0.10f,0.15f,0.08f}, 0.50f,0.25f,  0.15f},// 3: green moss
    {{0.32f,0.34f,0.52f},{0.12f,0.13f,0.20f}, 0.50f,0.50f,  0.05f},// 4: blue steel
    {{0.52f,0.45f,0.30f},{0.22f,0.18f,0.12f}, 0.33f,0.167f, 0.10f},// 5: tan sandstone
    {{0.65f,0.50f,0.15f},{0.30f,0.25f,0.08f}, 0.50f,1.00f,  0.03f},// 6: gold door
    {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 1, 1, 0},           // 7: unused
    {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 1, 1, 0},           // 8: unused
    {{0.70f,0.15f,0.12f},{0.35f,0.05f,0.05f}, 0.50f,0.25f,  0.06f},// 9: red exit
};

// ── Player ──────────────────────────────────────────────────────────────────

static float g_px = 2.5f, g_py = 2.5f;
static float g_pa = 0.0f;
static float g_fov = PI / 3.0f;
static int   g_health = 100;
static int   g_ammo = 50;
static int   g_score = 0;
static int   g_kills = 0;
static bool  g_won = false;

static float g_move_fwd = 0.f, g_move_strafe = 0.f, g_turn = 0.f;
static constexpr float MOVE_SPD = 0.18f;
static constexpr float TURN_SPD = 0.08f;
static constexpr float COLLISION_R = 0.2f;

// ── Enemies ─────────────────────────────────────────────────────────────────

struct Enemy {
    float x, y;
    int   hp, max_hp;
    int   type;       // 0=grunt, 1=fast, 2=heavy
    float timer;
    bool  active;
    bool  alert;
    float dist;
};

static std::vector<Enemy> g_enemies;
static std::mt19937 g_rng{std::random_device{}()};

static void spawn_enemies() {
    g_enemies.clear();
    g_enemies.push_back({11.5f, 3.5f,  30,30,  0, 0.f, true, false, 0.f});
    g_enemies.push_back({20.5f, 5.5f,  30,30,  0, 0.f, true, false, 0.f});
    g_enemies.push_back({3.5f, 12.5f,  20,20,  1, 0.f, true, false, 0.f});
    g_enemies.push_back({8.5f, 12.5f,  50,50,  2, 0.f, true, false, 0.f});
    g_enemies.push_back({18.5f, 11.5f, 30,30,  0, 0.f, true, false, 0.f});
    g_enemies.push_back({8.5f, 18.5f,  20,20,  1, 0.f, true, false, 0.f});
    g_enemies.push_back({13.5f, 19.5f, 30,30,  0, 0.f, true, false, 0.f});
    g_enemies.push_back({6.5f, 6.5f,   50,50,  2, 0.f, true, false, 0.f});
}

static int g_flash = 0, g_hit_flash = 0;
static float g_weapon_bob = 0.f; // weapon bobbing animation

struct Pickup { float x, y; int type; bool taken; };
static std::vector<Pickup> g_pickups;

static void spawn_pickups() {
    g_pickups.clear();
    g_pickups.push_back({4.5f, 1.5f, 1, false});
    g_pickups.push_back({11.5f, 7.5f, 0, false});
    g_pickups.push_back({1.5f, 11.5f, 1, false});
    g_pickups.push_back({16.5f, 1.5f, 0, false});
    g_pickups.push_back({22.5f, 8.5f, 1, false});
    g_pickups.push_back({7.5f, 17.5f, 0, false});
    g_pickups.push_back({14.5f, 17.5f, 1, false});
    g_pickups.push_back({22.5f, 17.5f, 0, false});
}

static int g_frame = 0;  // forward declaration for particles

// ── Particles (torch embers, dust, death) ───────────────────────────────────

struct Particle {
    float x, y, z;       // world pos (z = height, 0=floor, 1=ceiling)
    float vx, vy, vz;
    float life, max_life;
    Col3  color;
    float size;
};

static std::vector<Particle> g_particles;

static void spawn_torch_particles() {
    for (int i = 0; i < NUM_TORCHES; ++i) {
        if ((g_frame + i * 7) % 4 != 0) continue;
        float spread = 0.3f;
        float fi = static_cast<float>(i);
        float ff = static_cast<float>(g_frame);
        Particle p;
        p.x = g_torches[i].x + (hash(ff * 0.1f, fi) - 0.5f) * spread;
        p.y = g_torches[i].y + (hash(fi, ff * 0.1f) - 0.5f) * spread;
        p.z = 0.4f + hash(ff * 0.13f, fi * 2.7f) * 0.3f;
        p.vx = (hash(ff * 0.2f, fi) - 0.5f) * 0.02f;
        p.vy = (hash(fi, ff * 0.2f) - 0.5f) * 0.02f;
        p.vz = 0.01f + hash(ff * 0.3f, fi * 1.3f) * 0.015f;
        p.life = 1.0f;
        p.max_life = 1.0f;
        p.color = Col3{
            g_torches[i].color.r * 1.2f,
            g_torches[i].color.g * 0.8f,
            g_torches[i].color.b * 0.3f
        };
        p.size = 0.15f + hash(ff * 0.17f, fi) * 0.1f;
        g_particles.push_back(p);
    }
}

static void update_particles(float dt) {
    for (auto& p : g_particles) {
        p.x += p.vx; p.y += p.vy; p.z += p.vz;
        p.life -= dt * 1.5f;
        p.vz *= 0.98f;
    }
    std::erase_if(g_particles, [](const Particle& p) { return p.life <= 0.f || p.z > 1.0f; });
}

// ── Screen shake ────────────────────────────────────────────────────────────

static float g_shake = 0.f;

// ── Style interning ─────────────────────────────────────────────────────────

static constexpr int Q = 6;
static uint16_t g_styles[Q * Q * Q][Q * Q * Q];

static int to_idx(uint8_t r, uint8_t g, uint8_t b) {
    return (r * Q / 256) * Q * Q + (g * Q / 256) * Q + (b * Q / 256);
}
static uint8_t to8(int level) { return static_cast<uint8_t>(level * 255 / (Q - 1)); }

static uint16_t g_bar_bg, g_bar_dim, g_bar_accent, g_bar_health, g_bar_ammo;
static uint16_t g_minimap_wall, g_minimap_empty, g_minimap_player, g_minimap_enemy;

// ── State ───────────────────────────────────────────────────────────────────

static int g_w = 0, g_h = 0;
// g_frame declared above (before particles)
static bool g_show_map = true;
static bool g_dead = false;

static std::vector<float> g_zbuf;

struct Pixel { uint8_t r, g, b; };
static std::vector<Pixel> g_pixels;
static int g_pixel_w = 0, g_pixel_h = 0;

static void px_set(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x >= 0 && x < g_pixel_w && y >= 0 && y < g_pixel_h)
        g_pixels[static_cast<size_t>(y * g_pixel_w + x)] = {r, g, b};
}

static void px_blend(int x, int y, Col3 c, float alpha) {
    if (x < 0 || x >= g_pixel_w || y < 0 || y >= g_pixel_h) return;
    auto& px = g_pixels[static_cast<size_t>(y * g_pixel_w + x)];
    px.r = static_cast<uint8_t>(clampf(lerp(px.r / 255.f, c.r, alpha) * 255.f, 0, 255));
    px.g = static_cast<uint8_t>(clampf(lerp(px.g / 255.f, c.g, alpha) * 255.f, 0, 255));
    px.b = static_cast<uint8_t>(clampf(lerp(px.b / 255.f, c.b, alpha) * 255.f, 0, 255));
}

static Pixel px_get(int x, int y) {
    if (x >= 0 && x < g_pixel_w && y >= 0 && y < g_pixel_h)
        return g_pixels[static_cast<size_t>(y * g_pixel_w + x)];
    return {0, 0, 0};
}

// ── Rebuild ─────────────────────────────────────────────────────────────────

static void rebuild(StylePool& pool, int w, int h) {
    g_w = w; g_h = h;

    for (int fi = 0; fi < Q * Q * Q; ++fi) {
        int fr = fi / (Q * Q), fg = (fi / Q) % Q, fb = fi % Q;
        for (int bi = 0; bi < Q * Q * Q; ++bi) {
            int br = bi / (Q * Q), bg = (bi / Q) % Q, bb = bi % Q;
            g_styles[fi][bi] = pool.intern(
                Style{}.with_fg(Color::rgb(to8(fr), to8(fg), to8(fb)))
                       .with_bg(Color::rgb(to8(br), to8(bg), to8(bb))));
        }
    }

    g_bar_bg      = pool.intern(Style{}.with_bg(Color::rgb(10, 10, 10)).with_fg(Color::rgb(100, 100, 100)));
    g_bar_dim     = pool.intern(Style{}.with_bg(Color::rgb(10, 10, 10)).with_fg(Color::rgb(80, 80, 90)));
    g_bar_accent  = pool.intern(Style{}.with_bg(Color::rgb(10, 10, 10)).with_fg(Color::rgb(255, 200, 60)).with_bold());
    g_bar_health  = pool.intern(Style{}.with_bg(Color::rgb(10, 10, 10)).with_fg(Color::rgb(60, 220, 80)).with_bold());
    g_bar_ammo    = pool.intern(Style{}.with_bg(Color::rgb(10, 10, 10)).with_fg(Color::rgb(80, 180, 255)).with_bold());
    g_minimap_wall   = pool.intern(Style{}.with_bg(Color::rgb(80, 80, 90)).with_fg(Color::rgb(80, 80, 90)));
    g_minimap_empty  = pool.intern(Style{}.with_bg(Color::rgb(20, 20, 25)).with_fg(Color::rgb(20, 20, 25)));
    g_minimap_player = pool.intern(Style{}.with_bg(Color::rgb(255, 220, 50)).with_fg(Color::rgb(255, 220, 50)));
    g_minimap_enemy  = pool.intern(Style{}.with_bg(Color::rgb(255, 50, 50)).with_fg(Color::rgb(255, 50, 50)));

    g_pixel_w = w;
    g_pixel_h = (h - 1) * 2;
    g_pixels.assign(static_cast<size_t>(g_pixel_w * g_pixel_h), Pixel{0, 0, 0});
    g_zbuf.assign(static_cast<size_t>(w), 0.f);
}

// ── Collision ───────────────────────────────────────────────────────────────

static bool is_solid(int mx, int my) {
    if (mx < 0 || mx >= MAP_W || my < 0 || my >= MAP_H) return true;
    int t = g_map[my][mx];
    return t != 0 && t != 6;
}

static bool can_move(float nx, float ny) {
    float r = COLLISION_R;
    return !is_solid(int(nx-r), int(ny-r)) && !is_solid(int(nx+r), int(ny-r)) &&
           !is_solid(int(nx-r), int(ny+r)) && !is_solid(int(nx+r), int(ny+r));
}

// ── Game logic ──────────────────────────────────────────────────────────────

static float randf(float lo, float hi) {
    return std::uniform_real_distribution<float>(lo, hi)(g_rng);
}

static void reset_game() {
    g_px = 2.5f; g_py = 2.5f; g_pa = 0.0f;
    g_health = 100; g_ammo = 50; g_score = 0; g_kills = 0;
    g_dead = false; g_won = false; g_flash = 0; g_hit_flash = 0;
    g_frame = 0; g_weapon_bob = 0.f;
    spawn_enemies(); spawn_pickups();
}

static void shoot() {
    if (g_ammo <= 0 || g_dead) return;
    g_ammo--;
    g_flash = 4;

    float dx = std::cos(g_pa), dy = std::sin(g_pa);
    float best_dist = 999.f;
    Enemy* hit = nullptr;

    for (auto& e : g_enemies) {
        if (!e.active) continue;
        float ex = e.x - g_px, ey = e.y - g_py;
        float proj = ex * dx + ey * dy;
        if (proj < 0.1f) continue;
        float perp = std::fabs(ex * (-dy) + ey * dx);
        if (perp < 0.3f + proj * 0.04f && proj < best_dist) {
            best_dist = proj;
            hit = &e;
        }
    }

    if (hit) {
        int dmg = std::max(5, 25 - static_cast<int>(best_dist * 2.f));
        hit->hp -= dmg;
        hit->alert = true;
        if (hit->hp <= 0) {
            hit->active = false;
            g_score += (hit->type + 1) * 100;
            g_kills++;
        }
    }
}

static void tick(float dt) {
    if (g_dead || g_won) return;

    g_frame++;
    if (g_flash > 0) g_flash--;
    if (g_hit_flash > 0) g_hit_flash--;

    g_pa += g_turn * TURN_SPD;

    float fwd_x = std::cos(g_pa), fwd_y = std::sin(g_pa);
    float right_x = std::cos(g_pa + PI / 2.f), right_y = std::sin(g_pa + PI / 2.f);

    float mx = fwd_x * g_move_fwd + right_x * g_move_strafe;
    float my = fwd_y * g_move_fwd + right_y * g_move_strafe;

    float ml = std::sqrt(mx * mx + my * my);
    if (ml > 0.01f) {
        mx /= ml; my /= ml;
        float spd = MOVE_SPD * std::fmin(ml, 3.f);
        float nx = g_px + mx * spd, ny = g_py + my * spd;
        if (can_move(nx, g_py)) g_px = nx;
        if (can_move(g_px, ny)) g_py = ny;
        g_weapon_bob += ml * 0.3f;
    }
    g_move_fwd = 0.f; g_move_strafe = 0.f; g_turn = 0.f;

    // Check exit
    if (int(g_px) >= 0 && int(g_px) < MAP_W && int(g_py) >= 0 && int(g_py) < MAP_H
        && g_map[int(g_py)][int(g_px)] == 9) {
        g_won = true;
        g_score += g_health * 10;
    }

    // Pickups
    for (auto& p : g_pickups) {
        if (p.taken) continue;
        float dx = g_px - p.x, dy = g_py - p.y;
        if (dx * dx + dy * dy < 0.5f) {
            p.taken = true;
            if (p.type == 0) g_health = std::min(100, g_health + 25);
            else             g_ammo = std::min(99, g_ammo + 15);
            g_score += 50;
        }
    }

    // Enemy AI
    for (auto& e : g_enemies) {
        if (!e.active) continue;
        float dx = g_px - e.x, dy = g_py - e.y;
        e.dist = std::sqrt(dx * dx + dy * dy);
        if (e.dist < 8.f) e.alert = true;
        if (!e.alert) continue;
        e.timer += dt;

        float speed = (e.type == 1) ? 2.5f : (e.type == 2) ? 1.0f : 1.8f;
        if (e.dist > 1.2f) {
            float emx = dx / e.dist * speed * dt;
            float emy = dy / e.dist * speed * dt;
            float enx = e.x + emx, eny = e.y + emy;
            if (int(enx) >= 0 && int(enx) < MAP_W && int(eny) >= 0 && int(eny) < MAP_H
                && g_map[int(eny)][int(enx)] == 0) {
                e.x = enx; e.y = eny;
            }
        }
        if (e.dist < 1.5f && e.timer > 0.8f) {
            e.timer = 0.f;
            int dmg = (e.type == 2) ? 15 : (e.type == 1) ? 8 : 10;
            g_health -= dmg;
            g_hit_flash = 6;
            if (g_health <= 0) { g_health = 0; g_dead = true; }
        }
    }
}

// ── Events ──────────────────────────────────────────────────────────────────

static bool handle(const Event& ev) {
    if (key(ev, 'q') || key(ev, SpecialKey::Escape)) return false;
    on(ev, 'r', [] { reset_game(); });
    if (g_dead || g_won) return true;

    on(ev, 'w', [] { g_move_fwd += 1.f; });
    on(ev, 's', [] { g_move_fwd -= 1.f; });
    on(ev, 'a', [] { g_move_strafe -= 1.f; });
    on(ev, 'd', [] { g_move_strafe += 1.f; });
    on(ev, SpecialKey::Up,    [] { g_move_fwd += 1.f; });
    on(ev, SpecialKey::Down,  [] { g_move_fwd -= 1.f; });
    on(ev, SpecialKey::Left,  [] { g_turn -= 1.f; });
    on(ev, SpecialKey::Right, [] { g_turn += 1.f; });
    on(ev, ',', [] { g_turn -= 1.f; });
    on(ev, '.', [] { g_turn += 1.f; });
    on(ev, ' ', [] { shoot(); });
    on(ev, 'm', [] { g_show_map = !g_show_map; });

    return true;
}

// ── Raycasting ──────────────────────────────────────────────────────────────

struct RayHit {
    float dist;
    int   wall_type;
    float wall_x;
    bool  side;
    int   map_x, map_y;
};

static RayHit cast_ray(float angle) {
    float dx = std::cos(angle), dy = std::sin(angle);
    int mx = int(g_px), my = int(g_py);
    float delta_x = (dx == 0.f) ? 1e30f : std::fabs(1.0f / dx);
    float delta_y = (dy == 0.f) ? 1e30f : std::fabs(1.0f / dy);
    int step_x = (dx < 0) ? -1 : 1;
    int step_y = (dy < 0) ? -1 : 1;
    float side_x = (dx < 0) ? (g_px - mx) * delta_x : (mx + 1.0f - g_px) * delta_x;
    float side_y = (dy < 0) ? (g_py - my) * delta_y : (my + 1.0f - g_py) * delta_y;

    bool side_hit = false;
    for (int i = 0; i < 64; ++i) {
        if (side_x < side_y) { side_x += delta_x; mx += step_x; side_hit = false; }
        else                  { side_y += delta_y; my += step_y; side_hit = true; }
        if (mx < 0 || mx >= MAP_W || my < 0 || my >= MAP_H) break;
        int t = g_map[my][mx];
        if (t > 0) {
            float perp, wx;
            if (!side_hit) { perp = side_x - delta_x; wx = g_py + perp * dy; }
            else           { perp = side_y - delta_y; wx = g_px + perp * dx; }
            wx -= std::floor(wx);
            return {perp, t, wx, side_hit, mx, my};
        }
    }
    return {64.f, 0, 0.f, false, 0, 0};
}

// ── Torch lighting at a world point ─────────────────────────────────────────

static Col3 compute_light(float wx, float wy) {
    Col3 ambient = {0.06f, 0.05f, 0.08f};
    Col3 total = ambient;
    float flicker = 0.85f + 0.15f * std::sin(g_frame * 0.23f + wx * 3.7f);

    for (int i = 0; i < NUM_TORCHES; ++i) {
        float dx = wx - g_torches[i].x, dy = wy - g_torches[i].y;
        float d2 = dx * dx + dy * dy;
        float atten = g_torches[i].intensity / (1.0f + d2 * 0.8f);
        atten *= flicker + 0.05f * std::sin(g_frame * 0.37f + i * 2.1f);
        total = col_add(total, col_mul(g_torches[i].color, atten));
    }
    return total;
}

// ── Smooth noise (value noise with cubic interpolation) ─────────────────────

static float value_noise(float x, float y) {
    float ix = std::floor(x), iy = std::floor(y);
    float fx = x - ix, fy = y - iy;
    // Cubic hermite smoothing
    fx = fx * fx * (3.f - 2.f * fx);
    fy = fy * fy * (3.f - 2.f * fy);
    float a = hash(ix, iy);
    float b = hash(ix + 1, iy);
    float c = hash(ix, iy + 1);
    float d = hash(ix + 1, iy + 1);
    return lerp(lerp(a, b, fx), lerp(c, d, fx), fy);
}

static float fbm(float x, float y, int octaves) {
    float sum = 0.f, amp = 0.5f;
    for (int i = 0; i < octaves; ++i) {
        sum += value_noise(x, y) * amp;
        x *= 2.17f; y *= 2.17f;
        amp *= 0.5f;
    }
    return sum;
}

// ── Procedural brick texture ────────────────────────────────────────────────

static Col3 brick_texture(float u, float v, int wall_type) {
    const auto& mat = WALL_MATS[wall_type];

    float bu = u / mat.brick_w;
    float bv = v / mat.brick_h;

    int row = static_cast<int>(std::floor(bv));
    if (row & 1) bu += 0.5f;

    float fu = bu - std::floor(bu);
    float fv = bv - std::floor(bv);
    float brick_ix = std::floor(bu), brick_iy = std::floor(bv);

    // Mortar — soft edge with depth shading
    float mortar_w = 0.07f;
    float mu = std::fmin(fu, 1.f - fu) / mortar_w;
    float mv = std::fmin(fv, 1.f - fv) / mortar_w;
    float mortar_edge = std::fmin(mu, mv);
    mortar_edge = clampf(mortar_edge, 0.f, 1.f);

    if (mortar_edge < 1.0f) {
        // Mortar with depth and grit
        float grit = fbm(u * 40.f, v * 40.f, 3) * 0.08f;
        float depth = (1.f - mortar_edge) * 0.06f; // mortar recessed = darker
        Col3 mc = {
            clampf(mat.mortar.r + grit - depth, 0, 1),
            clampf(mat.mortar.g + grit - depth, 0, 1),
            clampf(mat.mortar.b + grit - depth, 0, 1),
        };
        if (mortar_edge > 0.5f) {
            // Blend zone — brick edge highlight (chipped edge)
            float edge_highlight = (mortar_edge - 0.5f) * 2.f * 0.08f;
            mc = col_add(mc, {edge_highlight, edge_highlight, edge_highlight});
        }
        if (mortar_edge >= 0.7f) {
            // Transition zone: mix with brick
        } else {
            return mc;
        }
    }

    // Per-brick identity
    float bid = hash(brick_ix, brick_iy);
    float bid2 = hash(brick_ix + 77.f, brick_iy + 33.f);
    float bid3 = hash(brick_ix + 13.f, brick_iy + 91.f);

    // Base color with per-brick hue/value shift
    Col3 c = mat.base;
    float hue_shift = (bid - 0.5f) * mat.roughness * 2.5f;
    float val_shift = (bid2 - 0.5f) * mat.roughness * 1.5f;
    c.r = clampf(c.r + hue_shift + val_shift, 0.02f, 0.95f);
    c.g = clampf(c.g + hue_shift * 0.7f + val_shift, 0.02f, 0.95f);
    c.b = clampf(c.b + hue_shift * 0.4f + val_shift, 0.02f, 0.95f);

    // Multi-octave surface noise (grain / roughness)
    float grain = fbm(u * 30.f + bid * 100.f, v * 30.f + bid2 * 100.f, 3);
    float fine_noise = (grain - 0.5f) * mat.roughness * 1.8f;
    c.r = clampf(c.r + fine_noise, 0, 1);
    c.g = clampf(c.g + fine_noise * 0.85f, 0, 1);
    c.b = clampf(c.b + fine_noise * 0.7f, 0, 1);

    // Stain / discoloration patches per brick
    if (bid3 > 0.6f) {
        float stain_str = (bid3 - 0.6f) * 2.5f;
        float stain_mask = smoothstep(0.3f, 0.7f, value_noise(u * 8.f + bid * 50.f, v * 8.f));
        Col3 stain_col = {c.r * 0.7f, c.g * 0.65f, c.b * 0.6f}; // darker stain
        c = col_lerp(c, stain_col, stain_mask * stain_str * 0.4f);
    }

    // Chipped / damaged bricks (some bricks have pits)
    if (bid > 0.85f) {
        float pit = smoothstep(0.35f, 0.40f, std::fabs(fu - 0.3f - bid2 * 0.4f))
                  * smoothstep(0.35f, 0.40f, std::fabs(fv - 0.3f - bid3 * 0.4f));
        pit = 1.0f - pit;
        if (pit > 0.5f) {
            c = col_mul(c, 0.6f + (1.0f - pit) * 0.4f);
        }
    }

    // Subtle bevel — edges of brick slightly lighter (catches light)
    float bevel = smoothstep(0.0f, 0.15f, fu) * smoothstep(1.0f, 0.85f, fu)
                * smoothstep(0.0f, 0.15f, fv) * smoothstep(1.0f, 0.85f, fv);
    float bevel_light = (1.0f - bevel) * 0.04f;
    c = col_add(c, {bevel_light, bevel_light, bevel_light});

    // Blend mortar/brick in transition zone
    if (mortar_edge < 1.0f && mortar_edge >= 0.7f) {
        float grit = fbm(u * 40.f, v * 40.f, 3) * 0.08f;
        Col3 mc = {
            clampf(mat.mortar.r + grit, 0, 1),
            clampf(mat.mortar.g + grit, 0, 1),
            clampf(mat.mortar.b + grit, 0, 1),
        };
        float t = (mortar_edge - 0.7f) / 0.3f;
        c = col_lerp(mc, c, t);
    }

    return c;
}

// ── Floor texture ───────────────────────────────────────────────────────────

static Col3 floor_texture(float wx, float wy) {
    // Large flagstone tiles (irregular sizes)
    float tile_u = wx * 1.0f, tile_v = wy * 1.0f;
    float iu = std::floor(tile_u), iv = std::floor(tile_v);
    float fu = tile_u - iu, fv = tile_v - iv;

    // Tile identity
    float tid = hash(iu, iv);
    float tid2 = hash(iu + 37.f, iv + 91.f);
    float tid3 = hash(iu + 73.f, iv + 17.f);

    // Grout — recessed with depth
    float grout_w = 0.05f;
    float gu = std::fmin(fu, 1.f - fu) / grout_w;
    float gv = std::fmin(fv, 1.f - fv) / grout_w;
    float grout_edge = clampf(std::fmin(gu, gv), 0.f, 1.f);

    if (grout_edge < 0.6f) {
        float depth = (1.f - grout_edge) * 0.04f;
        float grit = hash(wx * 50.f, wy * 50.f) * 0.02f;
        float v = 0.04f + grit - depth;
        return {v * 0.7f, v * 0.65f, v * 0.6f};
    }

    // Base stone color — warm gray/brown with per-tile variation
    float base_val = 0.13f + tid * 0.07f;
    Col3 c;
    // Some tiles are warmer, some cooler
    if (tid < 0.33f) {
        c = {base_val * 0.95f, base_val * 0.88f, base_val * 0.78f}; // warm brown
    } else if (tid < 0.66f) {
        c = {base_val * 0.85f, base_val * 0.85f, base_val * 0.88f}; // cool gray
    } else {
        c = {base_val * 0.90f, base_val * 0.82f, base_val * 0.72f}; // sandstone
    }

    // Stone grain: multi-octave noise for natural texture
    float grain = fbm(wx * 12.f + tid * 100.f, wy * 12.f + tid2 * 100.f, 4);
    float fine = (grain - 0.5f) * 0.08f;
    c.r = clampf(c.r + fine, 0, 1);
    c.g = clampf(c.g + fine * 0.9f, 0, 1);
    c.b = clampf(c.b + fine * 0.8f, 0, 1);

    // Mineral veins (darker lines running through stone)
    float vein_raw = value_noise(wx * 6.f + tid * 30.f, wy * 4.f + tid2 * 30.f);
    float vein = smoothstep(0.48f, 0.52f, vein_raw);
    float vein_dark = (1.0f - vein) * 0.06f;
    c.r -= vein_dark; c.g -= vein_dark; c.b -= vein_dark * 0.8f;

    // Speckles (mineral inclusions)
    float speck = hash(wx * 43.f, wy * 37.f);
    if (speck > 0.92f) {
        float bright = (speck - 0.92f) * 12.5f;
        float speck_hue = hash(wx * 43.1f, wy * 37.1f);
        if (speck_hue < 0.3f)
            c = col_add(c, {bright * 0.08f, bright * 0.06f, bright * 0.02f}); // gold fleck
        else if (speck_hue < 0.6f)
            c = col_add(c, {bright * 0.04f, bright * 0.04f, bright * 0.06f}); // quartz
        else
            c = col_add(c, {bright * 0.02f, bright * 0.05f, bright * 0.02f}); // green mineral
    }

    // Cracks (darker thin lines)
    float crack_raw = std::sin(wx * 7.3f + wy * 3.1f + tid3 * 20.f)
                    * std::sin(wx * 3.7f - wy * 5.9f + tid * 15.f);
    if (crack_raw > 0.85f) {
        float crack_str = (crack_raw - 0.85f) * 6.67f * 0.08f;
        c.r -= crack_str; c.g -= crack_str; c.b -= crack_str;
    }

    // Worn/polished center of tiles (foot traffic)
    float center_dist = std::sqrt((fu - 0.5f) * (fu - 0.5f) + (fv - 0.5f) * (fv - 0.5f));
    float polish = smoothstep(0.4f, 0.15f, center_dist) * 0.03f;
    c = col_add(c, {polish, polish, polish * 1.2f}); // slight sheen

    // Edge darkening (ambient occlusion at grout edges)
    if (grout_edge < 1.0f) {
        float ao = grout_edge;
        c = col_mul(c, 0.7f + ao * 0.3f);
    }

    return col_clamp(c);
}

// ── Render column ───────────────────────────────────────────────────────────

static void render_column(int col, int pixel_h) {
    float angle = g_pa - g_fov / 2.f + g_fov * (static_cast<float>(col) / g_pixel_w);
    RayHit hit = cast_ray(angle);

    float perp = hit.dist * std::cos(angle - g_pa);
    g_zbuf[col] = perp;

    float wall_h = static_cast<float>(pixel_h) / (perp + 0.001f);
    int wall_top = static_cast<int>((pixel_h - wall_h) / 2.f);
    int wall_bot = static_cast<int>((pixel_h + wall_h) / 2.f);

    // World position of wall hit for lighting
    float hit_wx, hit_wy;
    if (!hit.side) {
        hit_wx = static_cast<float>(hit.map_x) + (std::cos(angle) > 0 ? 0.f : 1.f);
        hit_wy = g_py + hit.dist * std::sin(angle);
    } else {
        hit_wx = g_px + hit.dist * std::cos(angle);
        hit_wy = static_cast<float>(hit.map_y) + (std::sin(angle) > 0 ? 0.f : 1.f);
    }

    for (int y = 0; y < pixel_h; ++y) {
        Col3 color;

        if (y < wall_top) {
            // ── Sky: gradient + stars + moon + aurora ──
            float t = static_cast<float>(y) / std::max(1, wall_top);
            Col3 sky_top  = {0.01f, 0.01f, 0.06f};
            Col3 sky_mid  = {0.03f, 0.03f, 0.10f};
            Col3 sky_low  = {0.06f, 0.04f, 0.08f};

            if (t < 0.5f) color = col_lerp(sky_top, sky_mid, t * 2.f);
            else          color = col_lerp(sky_mid, sky_low, (t - 0.5f) * 2.f);

            float sky_u = (angle / PI + 1.f) * 200.f;
            float sky_v = y * 0.5f;

            // Stars (multi-layer for depth)
            for (int layer = 0; layer < 3; ++layer) {
                float scale = 1.0f + layer * 0.7f;
                float su = std::floor(sky_u * scale), sv = std::floor(sky_v * scale);
                float star = hash(su + layer * 100.f, sv);
                float thresh = 0.988f - layer * 0.003f;
                if (star > thresh) {
                    float twinkle = 0.5f + 0.5f * std::sin(g_frame * (0.08f + layer * 0.03f) + star * 80.f);
                    float bright = (star - thresh) * 80.f * twinkle;
                    // Star color variation
                    float temp = hash(su, sv + 77.f);
                    Col3 sc = temp < 0.3f ? Col3{bright, bright * 0.85f, bright * 0.6f}  // warm
                            : temp < 0.6f ? Col3{bright * 0.7f, bright * 0.85f, bright}  // cool
                            :               Col3{bright, bright, bright * 0.95f};         // white
                    color = col_add(color, sc);
                }
            }

            // Moon
            float moon_angle = 0.8f; // fixed position
            float moon_y_pos = 0.2f;
            float moon_u = (angle - moon_angle);
            if (moon_u > PI) moon_u -= 2.f * PI;
            if (moon_u < -PI) moon_u += 2.f * PI;
            float moon_v = t - moon_y_pos;
            float moon_d = std::sqrt(moon_u * moon_u * 4.f + moon_v * moon_v * 16.f);
            if (moon_d < 0.15f) {
                float moon_shade = 1.0f - moon_d / 0.15f;
                moon_shade = moon_shade * moon_shade;
                // Crescent: offset circle subtraction
                float crater_d = std::sqrt((moon_u + 0.03f) * (moon_u + 0.03f) * 4.f + moon_v * moon_v * 16.f);
                if (crater_d > 0.12f) {
                    color = col_add(color, {moon_shade * 0.5f, moon_shade * 0.48f, moon_shade * 0.4f});
                }
                // Moon glow
                if (moon_d < 0.25f) {
                    float glow = (1.0f - moon_d / 0.25f) * 0.08f;
                    color = col_add(color, {glow * 0.6f, glow * 0.6f, glow * 0.8f});
                }
            }

            // Aurora borealis (subtle wavy bands)
            if (t < 0.6f) {
                float au = angle * 3.f + g_frame * 0.005f;
                float wave = std::sin(au * 2.f) * 0.5f + std::sin(au * 3.7f + 1.f) * 0.3f;
                float band = smoothstep(0.0f, 0.08f, std::fabs(t * 4.f - 1.0f + wave * 0.3f) - 0.5f);
                float aurora = (1.0f - band) * 0.06f * (0.5f + 0.5f * std::sin(g_frame * 0.02f));
                color = col_add(color, {aurora * 0.2f, aurora * 0.8f, aurora * 0.5f});
            }

        } else if (y >= wall_bot) {
            // ── Floor with reflections ──
            float row_dist = static_cast<float>(pixel_h) / (2.f * y - pixel_h + 0.001f);
            float floor_x = g_px + row_dist * std::cos(angle);
            float floor_y = g_py + row_dist * std::sin(angle);

            Col3 tex = floor_texture(floor_x, floor_y);
            Col3 light = compute_light(floor_x, floor_y);

            // Distance fog
            float fog = std::fmin(row_dist * 0.06f, 0.9f);
            Col3 fog_color = {0.02f, 0.02f, 0.04f};

            Col3 lit = col_clamp(Col3{
                tex.r * light.r * 2.5f,
                tex.g * light.g * 2.5f,
                tex.b * light.b * 2.5f
            });

            // Wet floor reflection: mirror the wall color above into the floor
            float reflect_str = 0.12f * (1.0f - fog); // stronger close, fades with distance
            int mirror_y = pixel_h - 1 - y; // reflected y
            if (mirror_y >= 0 && mirror_y < wall_top) {
                // Sky reflection (very faint)
                float sky_t = static_cast<float>(mirror_y) / std::max(1, wall_top);
                Col3 sky_ref = {0.03f + sky_t * 0.02f, 0.03f + sky_t * 0.02f, 0.06f + sky_t * 0.03f};
                lit = col_lerp(lit, sky_ref, reflect_str * 0.5f);
            } else if (mirror_y >= wall_top && mirror_y < wall_bot && hit.wall_type > 0) {
                // Wall reflection
                float v = static_cast<float>(mirror_y - wall_top) / std::max(1.f, static_cast<float>(wall_bot - wall_top));
                Col3 wall_tex = brick_texture(hit.wall_x, v, hit.wall_type);
                lit = col_lerp(lit, col_mul(wall_tex, 0.3f), reflect_str);
            }

            // Torch glow puddles on floor
            for (int ti = 0; ti < NUM_TORCHES; ++ti) {
                float dx = floor_x - g_torches[ti].x, dy = floor_y - g_torches[ti].y;
                float d2 = dx * dx + dy * dy;
                if (d2 < 4.0f) {
                    float glow = (1.0f - d2 / 4.0f) * 0.15f;
                    float flicker = 0.8f + 0.2f * std::sin(g_frame * 0.2f + ti * 1.5f);
                    lit = col_add(lit, col_mul(g_torches[ti].color, glow * flicker));
                }
            }

            color = col_lerp(lit, fog_color, fog);

        } else {
            // ── Wall ──
            if (hit.wall_type == 0) {
                color = {0, 0, 0};
            } else {
                float v = static_cast<float>(y - wall_top) / std::max(1.f, static_cast<float>(wall_bot - wall_top));

                // Get procedural texture
                Col3 tex = brick_texture(hit.wall_x, v, hit.wall_type);

                // Compute lighting at wall surface
                Col3 light = compute_light(hit_wx, hit_wy);

                // Side shading (normal-based)
                float side_shade = hit.side ? 0.65f : 1.0f;

                // Moisture/drip stains running down walls
                float drip_u = hit.wall_x * 7.3f;
                float drip_seed = hash(std::floor(drip_u), static_cast<float>(hit.map_x * 13 + hit.map_y * 7));
                if (drip_seed > 0.7f) {
                    float drip_width = smoothstep(0.0f, 0.15f, std::fabs(drip_u - std::floor(drip_u) - 0.5f));
                    float drip_flow = smoothstep(0.0f, 0.3f + drip_seed * 0.5f, v);
                    float moisture = (1.0f - drip_width) * drip_flow * 0.15f;
                    tex.r -= moisture; tex.g -= moisture * 0.8f; tex.b -= moisture * 0.5f;
                    // Slight specular on wet areas
                    float spec = moisture * 2.0f * (0.5f + 0.5f * std::sin(v * 20.f));
                    tex = col_add(tex, {spec * 0.3f, spec * 0.3f, spec * 0.4f});
                }

                // Moss near bottom of walls (types 1, 3, 5)
                if ((hit.wall_type == 1 || hit.wall_type == 3 || hit.wall_type == 5) && v > 0.75f) {
                    float moss_t = smoothstep(0.75f, 0.95f, v);
                    float moss_noise = hash(hit.wall_x * 13.f, v * 9.f);
                    if (moss_noise > 0.4f) {
                        Col3 moss_col = {0.12f, 0.22f, 0.08f};
                        tex = col_lerp(tex, moss_col, moss_t * 0.5f * (moss_noise - 0.4f) * 1.67f);
                    }
                }

                // Cracks
                float crack_h = hash(hit.wall_x * 5.1f + hit.map_x, v * 3.7f + hit.map_y);
                if (crack_h > 0.97f) {
                    tex = col_mul(tex, 0.6f);
                }

                // Ambient occlusion near top/bottom edges
                float ao = 1.0f;
                float edge_top = smoothstep(0.0f, 0.10f, v);
                float edge_bot = smoothstep(1.0f, 0.88f, v);
                ao = std::fmin(edge_top, edge_bot);
                ao = 0.4f + ao * 0.6f;

                // Distance fog
                float fog = std::fmin(perp * 0.045f, 0.85f);
                Col3 fog_color = {0.02f, 0.02f, 0.04f};

                // Combine
                Col3 lit = {
                    tex.r * light.r * side_shade * ao * 2.5f,
                    tex.g * light.g * side_shade * ao * 2.5f,
                    tex.b * light.b * side_shade * ao * 2.5f,
                };

                color = col_lerp(col_clamp(lit), fog_color, fog);

                // Door glow
                if (hit.wall_type == 6) {
                    float glow = 0.5f + 0.5f * std::sin(g_frame * 0.15f);
                    color = col_add(color, {glow * 0.12f, glow * 0.08f, 0.f});
                }
                // Exit pulse
                if (hit.wall_type == 9) {
                    float glow = 0.5f + 0.5f * std::sin(g_frame * 0.2f);
                    color = col_add(color, {glow * 0.15f, 0.f, 0.f});
                }
            }
        }

        color = col_clamp(color);
        px_set(col, y,
               static_cast<uint8_t>(color.r * 255.f),
               static_cast<uint8_t>(color.g * 255.f),
               static_cast<uint8_t>(color.b * 255.f));
    }
}

// ── Render sprites ──────────────────────────────────────────────────────────

static void render_sprite(float sx, float sy, int type, int hp, int max_hp,
                          float sprite_scale, int pixel_h) {
    float dx = sx - g_px, dy = sy - g_py;
    float dist = std::sqrt(dx * dx + dy * dy);
    if (dist < 0.1f || dist > 20.f) return;

    // Camera transform
    float cos_a = std::cos(g_pa), sin_a = std::sin(g_pa);
    float tx = dx * sin_a - dy * cos_a;
    float ty = dx * cos_a + dy * sin_a;
    // Adjust sign so it matches the camera plane convention
    float inv_det = 1.0f / (std::cos(g_pa + PI / 2.f) * sin_a - std::sin(g_pa + PI / 2.f) * cos_a);
    tx = inv_det * (sin_a * dx - cos_a * dy);
    ty = inv_det * (-std::sin(g_pa + PI / 2.f) * dx + std::cos(g_pa + PI / 2.f) * dy);
    if (ty < 0.2f) return;

    int screen_x = static_cast<int>((g_pixel_w / 2.f) * (1.f + tx / ty));
    float h = sprite_scale * static_cast<float>(pixel_h) / ty;
    int s_top = static_cast<int>((pixel_h - h) / 2.f);
    int s_bot = static_cast<int>((pixel_h + h) / 2.f);
    int s_left = screen_x - static_cast<int>(h / 2.f);
    int s_right = screen_x + static_cast<int>(h / 2.f);

    float fog = std::fmin(ty * 0.05f, 0.8f);
    Col3 light = compute_light(sx, sy);

    // Enemy colors
    Col3 body_col, head_col, eye_col;
    switch (type) {
        case 0: body_col = {0.75f, 0.20f, 0.15f}; head_col = {0.85f, 0.30f, 0.25f}; break;
        case 1: body_col = {0.15f, 0.65f, 0.20f}; head_col = {0.25f, 0.75f, 0.30f}; break;
        case 2: body_col = {0.25f, 0.25f, 0.75f}; head_col = {0.35f, 0.35f, 0.85f}; break;
        default: body_col = {0.5f, 0.5f, 0.5f}; head_col = {0.6f, 0.6f, 0.6f};
    }
    eye_col = {1.0f, 0.9f, 0.5f};

    // Damage flash (when recently hit)
    float hurt_tint = (hp < max_hp) ? 0.15f : 0.f;

    Col3 fog_color = {0.02f, 0.02f, 0.04f};

    for (int col = std::max(0, s_left); col < std::min(g_pixel_w, s_right); ++col) {
        if (ty >= g_zbuf[col]) continue;

        float u = static_cast<float>(col - s_left) / (s_right - s_left);
        float cx = u - 0.5f;

        for (int row = std::max(0, s_top); row < std::min(pixel_h, s_bot); ++row) {
            float v = static_cast<float>(row - s_top) / (s_bot - s_top);
            float cy = v - 0.5f;

            Col3 c;
            bool draw = false;

            // Body shape - rounded rectangle
            float body_r = 0.35f - std::fabs(cy) * 0.15f; // narrower at top/bottom
            if (v > 0.25f && v < 0.95f && std::fabs(cx) < body_r) {
                // Body shading
                float shade = 1.0f - std::fabs(cx) / body_r * 0.4f;
                c = col_mul(body_col, shade);
                draw = true;
            }

            // Head - circle
            float head_cx = 0.f, head_cy = 0.15f;
            float head_r = 0.18f;
            float hdx = cx - head_cx, hdy = cy - head_cy;
            float hd = std::sqrt(hdx * hdx + hdy * hdy);
            if (hd < head_r) {
                float shade = 1.0f - hd / head_r * 0.3f;
                c = col_mul(head_col, shade);
                draw = true;

                // Eyes
                float eye_y_range = (v > 0.28f && v < 0.36f);
                float left_eye  = std::fabs(cx + 0.07f);
                float right_eye = std::fabs(cx - 0.07f);
                if (eye_y_range && (left_eye < 0.03f || right_eye < 0.03f)) {
                    c = eye_col;
                }

                // Mouth
                if (v > 0.36f && v < 0.40f && std::fabs(cx) < 0.06f) {
                    c = {0.15f, 0.05f, 0.05f};
                }
            }

            // Arms (type 2 heavy has bigger arms)
            float arm_w = (type == 2) ? 0.12f : 0.08f;
            if (v > 0.35f && v < 0.65f) {
                float left_arm  = std::fabs(cx + 0.30f);
                float right_arm = std::fabs(cx - 0.30f);
                if (left_arm < arm_w || right_arm < arm_w) {
                    c = col_mul(body_col, 0.8f);
                    draw = true;
                }
            }

            if (!draw) continue;

            // Apply lighting, fog, hurt tint
            Col3 lit = {
                c.r * light.r * 2.0f + hurt_tint,
                c.g * light.g * 2.0f,
                c.b * light.b * 2.0f,
            };
            Col3 final = col_clamp(col_lerp(lit, fog_color, fog));

            px_set(col, row,
                   static_cast<uint8_t>(final.r * 255.f),
                   static_cast<uint8_t>(final.g * 255.f),
                   static_cast<uint8_t>(final.b * 255.f));
        }
    }

    // Health bar above enemy
    if (hp < max_hp && hp > 0) {
        int bar_w = std::max(4, (s_right - s_left) / 2);
        int bar_y = std::max(0, s_top - 3);
        int bar_x = screen_x - bar_w / 2;
        float pct = static_cast<float>(hp) / max_hp;
        for (int bx = 0; bx < bar_w; ++bx) {
            int px = bar_x + bx;
            if (px < 0 || px >= g_pixel_w || ty >= g_zbuf[px]) continue;
            bool filled = (static_cast<float>(bx) / bar_w) < pct;
            if (filled)
                px_set(px, bar_y, 60, 220, 80);
            else
                px_set(px, bar_y, 80, 30, 30);
        }
    }
}

// ── Render pickup sprite ────────────────────────────────────────────────────

static void render_pickup(float sx, float sy, int type, int pixel_h) {
    float dx = sx - g_px, dy = sy - g_py;
    float dist = std::sqrt(dx * dx + dy * dy);
    if (dist < 0.1f || dist > 15.f) return;

    float cos_a = std::cos(g_pa), sin_a = std::sin(g_pa);
    float inv_det = 1.0f / (std::cos(g_pa + PI / 2.f) * sin_a - std::sin(g_pa + PI / 2.f) * cos_a);
    float tx = inv_det * (sin_a * dx - cos_a * dy);
    float ty = inv_det * (-std::sin(g_pa + PI / 2.f) * dx + std::cos(g_pa + PI / 2.f) * dy);
    if (ty < 0.2f) return;

    int screen_x = static_cast<int>((g_pixel_w / 2.f) * (1.f + tx / ty));
    float h = 0.4f * static_cast<float>(pixel_h) / ty;
    int s_top = static_cast<int>((pixel_h - h) / 2.f);
    int s_bot = static_cast<int>((pixel_h + h) / 2.f);
    int s_left = screen_x - static_cast<int>(h / 2.f);
    int s_right = screen_x + static_cast<int>(h / 2.f);

    // Bobbing glow
    float pulse = 0.7f + 0.3f * std::sin(g_frame * 0.12f);
    Col3 color = (type == 0) ? Col3{0.15f, 0.85f * pulse, 0.25f}  // health = green
                             : Col3{0.25f, 0.55f * pulse, 1.0f * pulse}; // ammo = blue
    Col3 fog_color = {0.02f, 0.02f, 0.04f};
    float fog = std::fmin(ty * 0.05f, 0.8f);

    for (int col = std::max(0, s_left); col < std::min(g_pixel_w, s_right); ++col) {
        if (ty >= g_zbuf[col]) continue;
        float u = static_cast<float>(col - s_left) / (s_right - s_left);
        for (int row = std::max(0, s_top); row < std::min(pixel_h, s_bot); ++row) {
            float v = static_cast<float>(row - s_top) / (s_bot - s_top);
            float cx = u - 0.5f, cy = v - 0.5f;
            float d = std::sqrt(cx * cx + cy * cy);
            if (d > 0.35f) continue;

            // Glowing orb with inner shine
            float glow = 1.0f - d / 0.35f;
            glow = glow * glow;
            Col3 c = col_mul(color, glow);

            // Inner bright core
            if (d < 0.12f) {
                float core = 1.0f - d / 0.12f;
                c = col_add(c, {core * 0.5f, core * 0.5f, core * 0.5f});
            }

            Col3 final = col_clamp(col_lerp(c, fog_color, fog));
            px_set(col, row,
                   static_cast<uint8_t>(final.r * 255.f),
                   static_cast<uint8_t>(final.g * 255.f),
                   static_cast<uint8_t>(final.b * 255.f));
        }
    }
}

// ── Weapon sprite ───────────────────────────────────────────────────────────

static void draw_weapon(int pixel_h) {
    int w = g_pixel_w;
    int base_x = w / 2 + 4;
    int base_y = pixel_h + 2;

    // Bobbing
    float bob_x = std::sin(g_weapon_bob) * 4.f;
    float bob_y = std::fabs(std::cos(g_weapon_bob * 2.f)) * 3.f;

    // Recoil
    float recoil = g_flash > 0 ? static_cast<float>(g_flash) * 4.f : 0.f;

    // Shake
    float shake_x = g_shake * (hash(g_frame * 0.5f, 0.f) - 0.5f) * 6.f;
    float shake_y = g_shake * (hash(0.f, g_frame * 0.5f) - 0.5f) * 4.f;

    int ox = base_x + static_cast<int>(bob_x + shake_x);
    int oy = base_y + static_cast<int>(bob_y + recoil + shake_y);

    // Gun barrel (dark gunmetal with highlight)
    for (int dy = -22; dy <= 0; ++dy) {
        for (int dx = -4; dx <= 4; ++dx) {
            float t = 1.0f - static_cast<float>(-dy) / 22.f;
            float r = std::fabs(dx) / 4.5f;
            float shade = 0.20f + t * 0.12f;
            shade *= (1.0f - r * r * 0.5f);

            // Metallic highlight on left edge
            float highlight = (dx == -2 || dx == -3) ? 0.06f : 0.0f;
            float rim = (dx == 3 || dx == -4) ? -0.03f : 0.0f;

            float cr = clampf((shade + highlight + rim) * 0.85f, 0, 1);
            float cg = clampf((shade + highlight + rim) * 0.88f, 0, 1);
            float cb = clampf((shade + highlight + rim) * 1.0f, 0, 1);
            px_set(ox + dx, oy + dy,
                   static_cast<uint8_t>(cr * 255.f),
                   static_cast<uint8_t>(cg * 255.f),
                   static_cast<uint8_t>(cb * 255.f));
        }
    }

    // Muzzle ring
    for (int dx = -3; dx <= 3; ++dx) {
        float r = std::fabs(dx) / 3.5f;
        float shade = 0.30f * (1.0f - r * 0.5f);
        px_set(ox + dx, oy - 22,
               static_cast<uint8_t>(shade * 200.f),
               static_cast<uint8_t>(shade * 210.f),
               static_cast<uint8_t>(shade * 230.f));
    }

    // Receiver / body (wider section)
    for (int dy = 0; dy <= 5; ++dy) {
        for (int dx = -6; dx <= 6; ++dx) {
            float r = std::fabs(dx) / 7.f;
            float shade = 0.22f - r * 0.08f;
            float highlight = (dx == -4) ? 0.04f : 0.0f;
            float cr = clampf((shade + highlight) * 0.85f, 0, 1);
            float cg = clampf((shade + highlight) * 0.85f, 0, 1);
            float cb = clampf((shade + highlight) * 0.92f, 0, 1);
            px_set(ox + dx, oy + dy,
                   static_cast<uint8_t>(cr * 255.f),
                   static_cast<uint8_t>(cg * 255.f),
                   static_cast<uint8_t>(cb * 255.f));
        }
    }

    // Grip (wood-brown with grain)
    for (int dy = 5; dy <= 14; ++dy) {
        for (int dx = -4; dx <= 4; ++dx) {
            float t = static_cast<float>(dy - 5) / 9.f;
            float r = std::fabs(dx) / 5.f;
            float shade = 0.18f + t * 0.06f;
            shade *= (1.0f - r * 0.25f);
            // Wood grain
            float grain = std::sin(dy * 1.5f + dx * 0.3f) * 0.02f;
            shade += grain;
            px_set(ox + dx, oy + dy,
                   static_cast<uint8_t>(clampf(shade * 1.2f, 0, 1) * 255.f),
                   static_cast<uint8_t>(clampf(shade * 0.85f, 0, 1) * 255.f),
                   static_cast<uint8_t>(clampf(shade * 0.55f, 0, 1) * 255.f));
        }
    }

    // Trigger guard (thin arc)
    for (int dy = 5; dy <= 9; ++dy) {
        int tdx = (dy < 7) ? 5 : (dy < 9) ? 4 : 3;
        float shade = 0.25f;
        px_set(ox + tdx, oy + dy,
               static_cast<uint8_t>(shade * 200.f),
               static_cast<uint8_t>(shade * 210.f),
               static_cast<uint8_t>(shade * 230.f));
    }

    // Muzzle flash (multi-layered bloom)
    if (g_flash > 0) {
        int flash_y = oy - 24;
        float strength = static_cast<float>(g_flash) / 4.f;

        // Inner bright core
        int r1 = 3;
        for (int dy = -r1; dy <= r1; ++dy)
            for (int dx = -r1; dx <= r1; ++dx) {
                float d = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                if (d > r1) continue;
                float t = (1.0f - d / r1);
                px_blend(ox + dx, flash_y + dy, {1.0f, 1.0f, 0.9f}, t * t * strength);
            }

        // Mid glow
        int r2 = 6 + g_flash * 2;
        for (int dy = -r2; dy <= r2; ++dy)
            for (int dx = -r2; dx <= r2; ++dx) {
                float d = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                if (d > r2) continue;
                float t = (1.0f - d / r2);
                px_blend(ox + dx, flash_y + dy, {1.0f, 0.8f, 0.3f}, t * t * strength * 0.6f);
            }

        // Outer warm glow (lights up surroundings)
        int r3 = 12 + g_flash * 3;
        for (int dy = -r3; dy <= r3; ++dy)
            for (int dx = -r3; dx <= r3; ++dx) {
                float d = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                if (d > r3) continue;
                float t = (1.0f - d / r3);
                px_blend(ox + dx, flash_y + dy, {0.8f, 0.5f, 0.1f}, t * strength * 0.15f);
            }
    }
}

// ── Post-processing ─────────────────────────────────────────────────────────

// ── Render particles in screen space ─────────────────────────────────────────

static void render_particles(int pixel_h) {
    float cos_a = std::cos(g_pa), sin_a = std::sin(g_pa);
    float inv_det = 1.0f / (std::cos(g_pa + PI / 2.f) * sin_a -
                             std::sin(g_pa + PI / 2.f) * cos_a);

    for (auto& p : g_particles) {
        float dx = p.x - g_px, dy = p.y - g_py;
        float tx = inv_det * (sin_a * dx - cos_a * dy);
        float ty = inv_det * (-std::sin(g_pa + PI / 2.f) * dx + std::cos(g_pa + PI / 2.f) * dy);
        if (ty < 0.1f || ty > 15.f) continue;

        int sx = static_cast<int>((g_pixel_w / 2.f) * (1.f + tx / ty));
        // Map z (height) to screen y
        float screen_h = static_cast<float>(pixel_h) / ty;
        int sy = static_cast<int>(pixel_h / 2.f - (p.z - 0.5f) * screen_h);

        float alpha = p.life * 0.8f;
        float fog = std::fmin(ty * 0.08f, 0.8f);
        alpha *= (1.0f - fog);

        int r = static_cast<int>(p.size * screen_h * 0.5f);
        r = std::max(1, std::min(r, 4));

        for (int py = -r; py <= r; ++py)
            for (int px = -r; px <= r; ++px) {
                float d = std::sqrt(static_cast<float>(px * px + py * py));
                if (d > r) continue;
                float falloff = (1.0f - d / (r + 0.5f));
                px_blend(sx + px, sy + py, p.color, alpha * falloff * falloff);
            }
    }
}

// ── Dust motes (screen-space particles) ─────────────────────────────────────

static void render_dust(int pixel_h) {
    // Floating dust particles, parallax based on angle
    for (int i = 0; i < 40; ++i) {
        float seed_x = hash(static_cast<float>(i), 42.f);
        float seed_y = hash(42.f, static_cast<float>(i));
        float speed = 0.3f + hash(static_cast<float>(i), 99.f) * 0.4f;

        float x = std::fmod(seed_x * g_pixel_w + g_frame * speed + g_pa * 50.f * (seed_x - 0.5f),
                           static_cast<float>(g_pixel_w));
        if (x < 0) x += g_pixel_w;
        float y = std::fmod(seed_y * pixel_h + g_frame * speed * 0.5f,
                           static_cast<float>(pixel_h));

        float bright = 0.15f + 0.1f * std::sin(g_frame * 0.05f + i * 1.7f);
        float alpha = bright * (0.3f + 0.7f * std::sin(g_frame * 0.03f + seed_x * 20.f));
        if (alpha < 0.05f) continue;

        px_blend(static_cast<int>(x), static_cast<int>(y), {0.8f, 0.7f, 0.5f}, alpha);
    }
}

static void post_process(int pixel_h) {
    int w = g_pixel_w, h = pixel_h;
    float cx = w * 0.5f, cy = h * 0.5f;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            auto& px = g_pixels[static_cast<size_t>(y * w + x)];
            float r = px.r / 255.f, g = px.g / 255.f, b = px.b / 255.f;

            // Bloom: if pixel is bright, bleed into neighbors (cheap 1-pass approximation)
            float lum = r * 0.299f + g * 0.587f + b * 0.114f;
            if (lum > 0.5f) {
                float bloom = (lum - 0.5f) * 0.15f;
                r += bloom * 0.8f;
                g += bloom * 0.7f;
                b += bloom * 0.6f;
            }

            // Vignette (stronger, oval)
            float dx = (x - cx) / cx, dy = (y - cy) / cy;
            float vig = 1.0f - (dx * dx * 0.8f + dy * dy * 0.5f) * 0.4f;
            vig = clampf(vig, 0.2f, 1.0f);
            r *= vig; g *= vig; b *= vig;

            // Film grain
            float grain = (hash(x + g_frame * 0.1f, y + g_frame * 0.07f) - 0.5f) * 0.025f;
            r += grain; g += grain; b += grain;

            // Color grading: warm shadows, cool highlights
            float mid = (r + g + b) / 3.f;
            if (mid < 0.3f) {
                // Warm shadows
                r += (0.3f - mid) * 0.06f;
                b -= (0.3f - mid) * 0.04f;
            } else if (mid > 0.6f) {
                // Cool highlights
                b += (mid - 0.6f) * 0.05f;
            }

            // Slight contrast S-curve
            r = clampf(r, 0, 1); g = clampf(g, 0, 1); b = clampf(b, 0, 1);
            r = r * r * (3.f - 2.f * r); // smoothstep contrast
            g = g * g * (3.f - 2.f * g);
            b = b * b * (3.f - 2.f * b);

            // Gamma
            r = std::pow(clampf(r, 0, 1), 0.88f);
            g = std::pow(clampf(g, 0, 1), 0.90f);
            b = std::pow(clampf(b, 0, 1), 0.92f);

            // Damage flash
            if (g_hit_flash > 0) {
                float alpha = static_cast<float>(g_hit_flash) / 6.f * 0.4f;
                r = clampf(r + alpha * 0.5f, 0, 1);
                g *= (1.f - alpha * 0.6f);
                b *= (1.f - alpha * 0.6f);
            }

            px.r = static_cast<uint8_t>(clampf(r, 0, 1) * 255.f);
            px.g = static_cast<uint8_t>(clampf(g, 0, 1) * 255.f);
            px.b = static_cast<uint8_t>(clampf(b, 0, 1) * 255.f);
        }
    }
}

// ── Crosshair ───────────────────────────────────────────────────────────────

static void draw_crosshair(int pixel_h) {
    int cx = g_pixel_w / 2, cy = pixel_h / 2;
    int gap = 2, len = 4;
    Col3 cc = {1.0f, 1.0f, 1.0f};
    float alpha = 0.7f;

    for (int i = gap; i < gap + len; ++i) {
        px_blend(cx + i, cy, cc, alpha);
        px_blend(cx - i, cy, cc, alpha);
        px_blend(cx, cy + i, cc, alpha);
        px_blend(cx, cy - i, cc, alpha);
    }
    // Center dot
    px_blend(cx, cy, cc, 0.9f);
}

// ── Composite ───────────────────────────────────────────────────────────────

static void composite(Canvas& canvas, int canvas_h) {
    for (int cy = 0; cy < canvas_h; ++cy) {
        int y_top = cy * 2, y_bot = cy * 2 + 1;
        for (int cx = 0; cx < g_pixel_w; ++cx) {
            Pixel top = px_get(cx, y_top);
            Pixel bot = px_get(cx, y_bot);
            canvas.set(cx, cy, U'\u2580',
                       g_styles[to_idx(top.r, top.g, top.b)][to_idx(bot.r, bot.g, bot.b)]);
        }
    }
}

// ── Minimap ─────────────────────────────────────────────────────────────────

static void draw_minimap(Canvas& canvas) {
    int map_size = std::min(12, std::min(g_w / 4, (g_h - 1) / 3));
    if (map_size < 4) return;
    int ox = g_w - map_size - 1, oy = 1;
    int view_r = map_size / 2;
    int pmx = int(g_px), pmy = int(g_py);

    for (int dy = -view_r; dy < view_r; ++dy) {
        for (int dx = -view_r; dx < view_r; ++dx) {
            int mx = pmx + dx, my = pmy + dy;
            int sx = ox + dx + view_r, sy = oy + dy + view_r;
            if (sx < 0 || sx >= g_w || sy < 0 || sy >= g_h - 1) continue;

            uint16_t style = g_minimap_empty;
            if (mx < 0 || mx >= MAP_W || my < 0 || my >= MAP_H)
                style = g_minimap_wall;
            else if (g_map[my][mx] > 0)
                style = g_minimap_wall;
            if (mx == pmx && my == pmy) style = g_minimap_player;
            for (auto& e : g_enemies) {
                if (!e.active) continue;
                if (int(e.x) == mx && int(e.y) == my) style = g_minimap_enemy;
            }
            canvas.set(sx, sy, U'\u2588', style);
        }
    }
}

// ── Paint ───────────────────────────────────────────────────────────────────

static void paint(Canvas& canvas, int w, int h) {
    if (w != g_w || h != g_h) return;
    float dt = 1.0f / 30.0f;
    tick(dt);
    spawn_torch_particles();
    update_particles(dt);
    if (g_shake > 0.f) g_shake *= 0.85f;
    if (g_flash == 3) g_shake = 1.0f; // trigger on shot

    int canvas_h = h - 1;
    int pixel_h = canvas_h * 2;
    std::fill(g_pixels.begin(), g_pixels.end(), Pixel{0, 0, 0});

    // Render walls (multi-threaded)
    static const int n_threads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    auto render_cols = [&](int x0, int x1) { for (int x = x0; x < x1; ++x) render_column(x, pixel_h); };

    if (n_threads <= 1 || g_pixel_w < 16) {
        render_cols(0, g_pixel_w);
    } else {
        std::vector<std::jthread> threads;
        threads.reserve(static_cast<size_t>(n_threads));
        int chunk = (g_pixel_w + n_threads - 1) / n_threads;
        for (int t = 0; t < n_threads; ++t) {
            int lo = t * chunk, hi = std::min(lo + chunk, g_pixel_w);
            if (lo >= hi) break;
            threads.emplace_back([=, &canvas] { render_cols(lo, hi); });
        }
    }

    // Sprites (farthest first)
    std::vector<Enemy*> sorted;
    for (auto& e : g_enemies) { if (e.active) { e.dist = std::sqrt((e.x-g_px)*(e.x-g_px)+(e.y-g_py)*(e.y-g_py)); sorted.push_back(&e); } }
    std::sort(sorted.begin(), sorted.end(), [](auto* a, auto* b) { return a->dist > b->dist; });
    for (auto* e : sorted) {
        float scale = (e->type == 1) ? 0.65f : (e->type == 2) ? 1.0f : 0.8f;
        render_sprite(e->x, e->y, e->type, e->hp, e->max_hp, scale, pixel_h);
    }

    // Pickups
    for (auto& p : g_pickups) {
        if (p.taken) continue;
        render_pickup(p.x, p.y, p.type, pixel_h);
    }

    // Torch particles
    render_particles(pixel_h);

    // Dust motes
    render_dust(pixel_h);

    // Weapon + crosshair
    draw_weapon(pixel_h);
    draw_crosshair(pixel_h);

    // Post-processing (bloom, vignette, color grading, film grain)
    post_process(pixel_h);

    // Composite to canvas
    composite(canvas, canvas_h);

    // Minimap
    if (g_show_map) draw_minimap(canvas);

    // Game over / victory
    if (g_dead || g_won) {
        int cy = canvas_h / 2;
        const char* msg = g_dead ? "YOU DIED" : "ESCAPE!";
        int mlen = static_cast<int>(std::strlen(msg));
        auto* pool = canvas.style_pool();
        uint16_t s = pool->intern(
            Style{}.with_fg(g_dead ? Color::rgb(255, 60, 60) : Color::rgb(80, 255, 120))
                   .with_bg(Color::rgb(0, 0, 0)).with_bold());
        canvas.write_text((w - mlen) / 2, cy, msg, s);
        const char* sub = "[r] restart  [q] quit";
        canvas.write_text((w - static_cast<int>(std::strlen(sub))) / 2, cy + 1, sub,
            pool->intern(Style{}.with_fg(Color::rgb(160, 160, 160)).with_bg(Color::rgb(0, 0, 0))));
        char sbuf[64];
        std::snprintf(sbuf, sizeof(sbuf), "SCORE: %d  KILLS: %d", g_score, g_kills);
        canvas.write_text((w - static_cast<int>(std::strlen(sbuf))) / 2, cy + 2, sbuf,
            pool->intern(Style{}.with_fg(Color::rgb(255, 200, 60)).with_bg(Color::rgb(0, 0, 0)).with_bold()));
    }

    // Status bar
    int bar_y = h - 1;
    for (int x = 0; x < w; ++x) canvas.set(x, bar_y, U' ', g_bar_bg);
    const char* help = "FPS \xe2\x94\x82 [wasd] move \xe2\x94\x82 [,/.] turn \xe2\x94\x82 [space] shoot \xe2\x94\x82 [m] map \xe2\x94\x82 [q] quit";
    canvas.write_text(1, bar_y, help, g_bar_dim);
    canvas.write_text(1, bar_y, "FPS", g_bar_accent);

    char hbuf[32]; std::snprintf(hbuf, sizeof(hbuf), "\xe2\x99\xa5%d", g_health);
    if (w / 2 - 12 > 0) canvas.write_text(w / 2 - 12, bar_y, hbuf, g_bar_health);
    char abuf[32]; std::snprintf(abuf, sizeof(abuf), "\xe2\x96\xaa%d", g_ammo);
    if (w / 2 - 4 > 0) canvas.write_text(w / 2 - 4, bar_y, abuf, g_bar_ammo);
    char sbuf[48]; std::snprintf(sbuf, sizeof(sbuf), "K:%d  \xe2\x98\x85%d", g_kills, g_score);
    int slen = static_cast<int>(std::strlen(sbuf));
    if (w > slen + 2) canvas.write_text(w - slen - 1, bar_y, sbuf, g_bar_accent);
}

// ── Main ────────────────────────────────────────────────────────────────────

int main() {
    spawn_enemies();
    spawn_pickups();

    (void)canvas_run(
        CanvasConfig{.fps = 30, .mouse = false, .mode = Mode::Fullscreen, .auto_clear = false, .title = "fps"},
        rebuild, handle, paint
    );
}
