// maya — FPS: Wolfenstein-style raycaster
//
// DDA raycasting with real brick-textured walls, mortar lines, stone floors,
// torch-lit dungeon atmosphere, enemies, pickups, weapon, minimap, and
// half-block rendering.
//
// Keys: WASD/arrows=move  ,/.=turn  space=shoot  m=minimap  r=restart  q/Esc=quit

#include <maya/app.hpp>
#include <maya/element/pixels.hpp>
#include <maya/maya.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <cstdint>
#include <thread>
#include <array>
#include <chrono>
#include <optional>
#include <thread>
#include <variant>
#include <vector>

using namespace maya;
using namespace maya::dsl;
using namespace std::chrono_literals;

namespace {

// ── Math ────────────────────────────────────────────────────────────────────

static constexpr float PI = 3.14159265f;

static float clampf(float x, float lo, float hi) { return std::fmin(std::fmax(x, lo), hi); }
static float lerp(float a, float b, float t) { return a + (b - a) * t; }
static float smoothstep(float e0, float e1, float x) {
    float t = clampf((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

struct Col3 { float r, g, b; };
static Col3 operator+(Col3 a, Col3 b) { return {a.r+b.r, a.g+b.g, a.b+b.b}; }
static Col3 operator*(Col3 a, float s) { return {a.r*s, a.g*s, a.b*s}; }
static Col3 col_lerp(Col3 a, Col3 b, float t) { return {lerp(a.r,b.r,t), lerp(a.g,b.g,t), lerp(a.b,b.b,t)}; }
static Col3 col_clamp(Col3 c) { return {clampf(c.r,0,1), clampf(c.g,0,1), clampf(c.b,0,1)}; }

// Lattice hash -> [0, 1): an integer mix, not fract(sin(dot) * 43758).
// sinf of an argument in the thousands pays a full range reduction on every
// texel, and it was the top libm frame in this demo's profile.
static float hash(float x, float y) {
    auto h = static_cast<std::uint32_t>(static_cast<std::int32_t>(std::floor(x))) * 0x8da6b343u
           ^ static_cast<std::uint32_t>(static_cast<std::int32_t>(std::floor(y))) * 0xd8163841u;
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
    float a = hash(ix, iy), b = hash(ix + 1, iy);
    float c = hash(ix, iy + 1), d = hash(ix + 1, iy + 1);
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

// ── Map ─────────────────────────────────────────────────────────────────────

static constexpr int MAP_W = 24;
static constexpr int MAP_H = 24;

// 0=empty, 1-5=wall types, 6=door, 9=exit
// clang-format off
static const int g_map[MAP_H][MAP_W] = {
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

// ── Wall materials ──────────────────────────────────────────────────────────

struct WallMat {
    Col3  base;      // average brick color
    Col3  mortar;    // mortar/grout color
    float brick_w;   // brick width in UV (0-1) per row
    float brick_h;   // brick height in UV
};

static const WallMat WALL_MATS[] = {
    //  base color                mortar                    bw     bh
    {{0,0,0},              {0,0,0},                  1,     1},      // 0: unused
    {{0.52f,0.10f,0.07f}, {0.22f,0.18f,0.15f},      0.25f, 0.12f},  // 1: classic red brick
    {{0.55f,0.12f,0.06f}, {0.24f,0.20f,0.16f},      0.20f, 0.10f},  // 2: red-orange brick
    {{0.40f,0.07f,0.06f}, {0.18f,0.14f,0.12f},      0.22f, 0.11f},  // 3: dark crimson brick
    {{0.48f,0.14f,0.08f}, {0.20f,0.16f,0.13f},      0.28f, 0.14f},  // 4: warm brick
    {{0.44f,0.10f,0.07f}, {0.19f,0.15f,0.13f},      0.25f, 0.13f},  // 5: old dark brick
    {{0.50f,0.40f,0.12f}, {0.25f,0.20f,0.10f},      0.30f, 0.15f},  // 6: gold door
    {{0,0,0},              {0,0,0},                  1,     1},      // 7: unused
    {{0,0,0},              {0,0,0},                  1,     1},      // 8: unused
    {{0.58f,0.08f,0.05f}, {0.25f,0.12f,0.08f},      0.22f, 0.12f},  // 9: exit
};

// ── Torches ─────────────────────────────────────────────────────────────────

struct Torch { float x, y; Col3 color; float intensity; };

static const Torch g_torches[] = {
    {1.5f,  1.5f,  {1.0f, 0.75f, 0.35f}, 5.0f},
    {11.5f, 1.5f,  {1.0f, 0.75f, 0.35f}, 5.0f},
    {22.5f, 1.5f,  {0.5f, 0.6f,  0.9f},  4.0f},
    {1.5f,  8.5f,  {1.0f, 0.75f, 0.35f}, 5.0f},
    {22.5f, 8.5f,  {1.0f, 0.70f, 0.30f}, 5.0f},
    {11.5f, 11.5f, {1.0f, 0.80f, 0.40f}, 5.5f},
    {1.5f,  22.5f, {1.0f, 0.75f, 0.35f}, 5.0f},
    {22.5f, 22.5f, {0.4f, 0.75f, 0.45f}, 4.0f},
    {11.5f, 17.5f, {1.0f, 0.60f, 0.25f}, 5.5f},
    {20.5f, 15.5f, {1.0f, 0.30f, 0.12f}, 6.0f},
};
static constexpr int NUM_TORCHES = sizeof(g_torches) / sizeof(g_torches[0]);

// ── Player ──────────────────────────────────────────────────────────────────

static constexpr float MOVE_SPD = 0.10f;
static constexpr float TURN_SPD = 0.05f;

static constexpr int MOVE_HOLD_FRAMES = 5;
static constexpr int TURN_HOLD_FRAMES = 3;
enum KeyAction { K_FWD, K_BACK, K_LEFT, K_RIGHT, K_TURN_L, K_TURN_R, K_COUNT };
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


static std::vector<Enemy> spawn_enemies() {
    std::vector<Enemy> out;
    out.push_back({11.5f, 3.5f,  30,30,  0, 0.f, true, false, 0.f});
    out.push_back({20.5f, 5.5f,  30,30,  0, 0.f, true, false, 0.f});
    out.push_back({3.5f, 12.5f,  20,20,  1, 0.f, true, false, 0.f});
    out.push_back({8.5f, 12.5f,  50,50,  2, 0.f, true, false, 0.f});
    out.push_back({18.5f, 11.5f, 30,30,  0, 0.f, true, false, 0.f});
    out.push_back({8.5f, 18.5f,  20,20,  1, 0.f, true, false, 0.f});
    out.push_back({13.5f, 19.5f, 30,30,  0, 0.f, true, false, 0.f});
    out.push_back({6.5f, 6.5f,   50,50,  2, 0.f, true, false, 0.f});
    return out;
}


struct Pickup { float x, y; int type; bool taken; };

static std::vector<Pickup> spawn_pickups() {
    std::vector<Pickup> out;
    out.push_back({4.5f, 1.5f, 1, false});
    out.push_back({11.5f, 7.5f, 0, false});
    out.push_back({1.5f, 11.5f, 1, false});
    out.push_back({16.5f, 1.5f, 0, false});
    out.push_back({22.5f, 8.5f, 1, false});
    out.push_back({7.5f, 17.5f, 0, false});
    out.push_back({14.5f, 17.5f, 1, false});
    out.push_back({22.5f, 17.5f, 0, false});
    return out;
}

// ── Model ───────────────────────────────────────────────────────────────────

struct Model {
    int w = 0, h = 0;                               // terminal cells
    float px = 2.5f, py = 2.5f, pa = 0.0f;          // player position, angle
    float fov = PI / 3.0f;
    int   health = 100, ammo = 50, score = 0, kills = 0;
    bool  won = false, dead = false, show_map = true;
    std::array<int, K_COUNT> key_last_seen = {-100, -100, -100, -100, -100, -100};
    std::vector<Enemy> enemies = spawn_enemies();
    std::vector<Pickup> pickups = spawn_pickups();
    int   flash = 0, hit_flash = 0;                 // muzzle flash, damage flash (frames)
    float weapon_bob = 0.f;
    int   frame = 0;
    std::mt19937 rng{std::random_device{}()};
};




// One frame's pixels and depth buffer. Lives only inside view().
struct Frame {
    Image img;
    std::vector<float> zbuf;                        // per column
    int w, h;
    Frame(int w_, int h_) : img(w_, h_), zbuf(static_cast<std::size_t>(w_), 1e9f), w(w_), h(h_) {}
};

// ── Pixels ──────────────────────────────────────────────────────────────────

static void px_set(Frame& f, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x >= 0 && x < f.w && y >= 0 && y < f.h) f.img(x, y) = {r, g, b};
}

static void px_blend(Frame& f, int x, int y, Col3 c, float alpha) {
    if (x < 0 || x >= f.w || y < 0 || y >= f.h) return;
    Rgb& px = f.img(x, y);
    px.r = static_cast<uint8_t>(clampf(lerp(px.r / 255.f, c.r, alpha) * 255.f, 0, 255));
    px.g = static_cast<uint8_t>(clampf(lerp(px.g / 255.f, c.g, alpha) * 255.f, 0, 255));
    px.b = static_cast<uint8_t>(clampf(lerp(px.b / 255.f, c.b, alpha) * 255.f, 0, 255));
}

// ── Collision ───────────────────────────────────────────────────────────────

static bool is_solid(const Model& m, int mx, int my) {
    if (mx < 0 || mx >= MAP_W || my < 0 || my >= MAP_H) return true;
    int t = g_map[my][mx];
    return t != 0 && t != 6;
}

static bool can_move(const Model& m, float nx, float ny) {
    float r = COLLISION_R;
    return !is_solid(m, int(nx-r), int(ny-r)) && !is_solid(m, int(nx+r), int(ny-r)) &&
           !is_solid(m, int(nx-r), int(ny+r)) && !is_solid(m, int(nx+r), int(ny+r));
}

// ── Game logic ──────────────────────────────────────────────────────────────

static void reset_game(Model& m) {
    Model fresh;
    fresh.w = m.w; fresh.h = m.h; fresh.show_map = m.show_map;
    fresh.rng = std::move(m.rng);
    m = std::move(fresh);
}

static void shoot(Model& m) {
    if (m.ammo <= 0 || m.dead) return;
    m.ammo--;
    m.flash = 4;

    float dx = std::cos(m.pa), dy = std::sin(m.pa);
    float best_dist = 999.f;
    Enemy* hit = nullptr;

    for (auto& e : m.enemies) {
        if (!e.active) continue;
        float ex = e.x - m.px, ey = e.y - m.py;
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
            m.score += (hit->type + 1) * 100;
            m.kills++;
        }
    }
}

static void tick(Model& m, float dt) {
    if (m.dead || m.won) return;

    m.frame++;
    if (m.flash > 0) m.flash--;
    if (m.hit_flash > 0) m.hit_flash--;

    auto held = [&m](KeyAction k) {
        int window = (k == K_TURN_L || k == K_TURN_R) ? TURN_HOLD_FRAMES : MOVE_HOLD_FRAMES;
        return (m.frame - m.key_last_seen[k]) < window;
    };
    float move_fwd    = (held(K_FWD)    ? 1.f : 0.f) - (held(K_BACK)   ? 1.f : 0.f);
    float move_strafe = (held(K_RIGHT)  ? 1.f : 0.f) - (held(K_LEFT)   ? 1.f : 0.f);
    float turn        = (held(K_TURN_R) ? 1.f : 0.f) - (held(K_TURN_L) ? 1.f : 0.f);

    m.pa += turn * TURN_SPD;

    float fwd_x = std::cos(m.pa), fwd_y = std::sin(m.pa);
    float right_x = std::cos(m.pa + PI / 2.f), right_y = std::sin(m.pa + PI / 2.f);

    float mx = fwd_x * move_fwd + right_x * move_strafe;
    float my = fwd_y * move_fwd + right_y * move_strafe;

    float ml = std::sqrt(mx * mx + my * my);
    if (ml > 0.01f) {
        mx /= ml; my /= ml;
        float spd = MOVE_SPD * std::fmin(ml, 3.f);
        float nx = m.px + mx * spd, ny = m.py + my * spd;
        if (can_move(m, nx, m.py)) m.px = nx;
        if (can_move(m, m.px, ny)) m.py = ny;
        m.weapon_bob += ml * 0.3f;
    }

    // Check exit
    if (int(m.px) >= 0 && int(m.px) < MAP_W && int(m.py) >= 0 && int(m.py) < MAP_H
        && g_map[int(m.py)][int(m.px)] == 9) {
        m.won = true;
        m.score += m.health * 10;
    }

    // Pickups
    for (auto& p : m.pickups) {
        if (p.taken) continue;
        float dx = m.px - p.x, dy = m.py - p.y;
        if (dx * dx + dy * dy < 0.5f) {
            p.taken = true;
            if (p.type == 0) m.health = std::min(100, m.health + 25);
            else             m.ammo = std::min(99, m.ammo + 15);
            m.score += 50;
        }
    }

    // Enemy AI
    for (auto& e : m.enemies) {
        if (!e.active) continue;
        float dx = m.px - e.x, dy = m.py - e.y;
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
            m.health -= dmg;
            m.hit_flash = 6;
            if (m.health <= 0) { m.health = 0; m.dead = true; }
        }
    }
}

// ── Events ──────────────────────────────────────────────────────────────────

// ── Raycasting ──────────────────────────────────────────────────────────────

struct RayHit {
    float dist;
    int   wall_type;
    float wall_x;   // 0-1 texture U coordinate
    bool  side;      // true = Y-side hit
    int   map_x, map_y;
};

static RayHit cast_ray(const Model& m, float angle) {
    float dx = std::cos(angle), dy = std::sin(angle);
    int mx = int(m.px), my = int(m.py);
    float delta_x = (dx == 0.f) ? 1e30f : std::fabs(1.0f / dx);
    float delta_y = (dy == 0.f) ? 1e30f : std::fabs(1.0f / dy);
    int step_x = (dx < 0) ? -1 : 1;
    int step_y = (dy < 0) ? -1 : 1;
    float side_x = (dx < 0) ? (m.px - mx) * delta_x : (mx + 1.0f - m.px) * delta_x;
    float side_y = (dy < 0) ? (m.py - my) * delta_y : (my + 1.0f - m.py) * delta_y;

    bool side_hit = false;
    for (int i = 0; i < 64; ++i) {
        if (side_x < side_y) { side_x += delta_x; mx += step_x; side_hit = false; }
        else                  { side_y += delta_y; my += step_y; side_hit = true; }
        if (mx < 0 || mx >= MAP_W || my < 0 || my >= MAP_H) break;
        int t = g_map[my][mx];
        if (t > 0) {
            float perp, wx;
            if (!side_hit) { perp = side_x - delta_x; wx = m.py + perp * dy; }
            else           { perp = side_y - delta_y; wx = m.px + perp * dx; }
            wx -= std::floor(wx);
            return {perp, t, wx, side_hit, mx, my};
        }
    }
    return {64.f, 0, 0.f, false, 0, 0};
}

// ── Lighting ────────────────────────────────────────────────────────────────

static Col3 compute_light(const Model& m, float wx, float wy) {
    // Cool dim ambient — dark dungeon baseline
    Col3 total = {0.06f, 0.06f, 0.09f};

    for (int i = 0; i < NUM_TORCHES; ++i) {
        float dx = wx - g_torches[i].x, dy = wy - g_torches[i].y;
        float d2 = dx * dx + dy * dy;
        // Realistic inverse-square with linear cutoff
        float atten = g_torches[i].intensity / (1.0f + d2 * 0.5f);
        // Flicker
        float flicker = 0.88f + 0.12f * std::sin(m.frame * 0.15f + i * 2.3f);
        total = total + g_torches[i].color * (atten * flicker);
    }
    return total;
}

// ── Brick wall texture ──────────────────────────────────────────────────────
//
// Real brick pattern: running bond (offset every other row), mortar lines,
// per-brick color variation, surface roughness, edge weathering.

static Col3 wall_texture(float u, float v, int wall_type, bool side) {
    const auto& mat = WALL_MATS[wall_type];

    // Door: flat color with panel detail instead of bricks
    if (wall_type == 6) {
        float panel_v = std::fmod(v * 4.f, 1.f);
        float panel_u = std::fmod(u * 2.f, 1.f);
        float frame = std::fmin(std::fmin(panel_u, 1.f - panel_u),
                                std::fmin(panel_v, 1.f - panel_v));
        float is_frame = smoothstep(0.08f, 0.05f, frame);
        Col3 door = mat.base;
        Col3 trim = mat.base * 0.65f;
        Col3 c = col_lerp(door, trim, is_frame);
        // Wood grain
        float grain = value_noise(u * 1.f, v * 30.f) * 0.08f;
        c = c * (0.92f + grain);
        return side ? c * 0.75f : c;
    }

    float bw = mat.brick_w;
    float bh = mat.brick_h;

    // Brick grid coordinates
    float row = v / bh;
    int   row_i = static_cast<int>(std::floor(row));
    float row_f = row - row_i;

    // Running bond: offset every other row by half a brick width
    float u_offset = u + (row_i & 1) * bw * 0.5f;
    float col = u_offset / bw;
    int   col_i = static_cast<int>(std::floor(col));
    float col_f = col - col_i;

    // Mortar lines
    float mortar_w = 0.06f;   // fraction of brick size
    float mu = std::fmin(col_f, 1.f - col_f) / mortar_w;
    float mv = std::fmin(row_f, 1.f - row_f) / mortar_w;
    float mortar_mask = clampf(std::fmin(mu, mv), 0.f, 1.f);

    // In the mortar?
    if (mortar_mask < 0.5f) {
        Col3 mortar = mat.mortar;
        // Mortar depth variation
        float depth_noise = value_noise(u * 40.f + 3.f, v * 40.f + 7.f);
        mortar = mortar * (0.85f + depth_noise * 0.3f);
        // Side face darkening
        if (side) mortar = mortar * 0.8f;
        return mortar;
    }

    // Per-brick identity (deterministic from grid cell)
    float brick_id = hash(static_cast<float>(col_i) + wall_type * 100.f,
                          static_cast<float>(row_i) + wall_type * 37.f);
    float brick_id2 = hash(static_cast<float>(col_i) + 50.f, static_cast<float>(row_i) + 90.f);

    // Base brick color with per-brick variation
    Col3 c = mat.base;
    // Hue shift: some bricks are slightly more orange, some more brown
    float hue_shift = (brick_id - 0.5f) * 0.12f;
    c.r = clampf(c.r + hue_shift, 0, 1);
    c.g = clampf(c.g + hue_shift * 0.3f, 0, 1);
    // Value shift: some bricks lighter or darker
    float value_shift = (brick_id2 - 0.5f) * 0.15f;
    c = c * (1.f + value_shift);

    // Surface roughness: fine noise on each brick
    float rough = value_noise((u + brick_id * 10.f) * 25.f,
                              (v + brick_id2 * 10.f) * 25.f);
    c = c * (0.88f + rough * 0.24f);

    // Coarse pitting / chips
    float pit = value_noise(u * 12.f + brick_id * 5.f, v * 12.f + brick_id2 * 5.f);
    if (pit > 0.78f) {
        c = c * (0.7f + (pit - 0.78f) * 2.f);
    }

    // Edge weathering: darken bricks near mortar joints
    float edge_u = smoothstep(0.f, 0.2f, std::fmin(col_f, 1.f - col_f) / mortar_w - 0.5f);
    float edge_v = smoothstep(0.f, 0.2f, std::fmin(row_f, 1.f - row_f) / mortar_w - 0.5f);
    float edge_ao = std::fmin(edge_u, edge_v);
    c = c * (0.82f + edge_ao * 0.18f);

    // Side face: geometric shading (simulates light hitting side at different angle)
    if (side) c = c * 0.72f;

    return col_clamp(c);
}

// ── Floor texture ───────────────────────────────────────────────────────────

static Col3 floor_texture(float wx, float wy) {
    float iu = std::floor(wx), iv = std::floor(wy);
    float fu = wx - iu, fv = wy - iv;

    // Grout lines
    float grout_w = 0.05f;
    float gu = std::fmin(fu, 1.f - fu) / grout_w;
    float gv = std::fmin(fv, 1.f - fv) / grout_w;
    float grout_edge = clampf(std::fmin(gu, gv), 0.f, 1.f);

    if (grout_edge < 0.5f) {
        float depth = (1.f - grout_edge) * 0.015f;
        return {0.03f - depth, 0.03f - depth, 0.04f - depth};
    }

    // Checkerboard tint
    int check = (static_cast<int>(iu) + static_cast<int>(iv)) & 1;
    float base = check ? 0.14f : 0.11f;

    // Per-tile identity
    float tid = hash(iu, iv);
    float tid2 = hash(iu + 37.f, iv + 91.f);
    float tid3 = hash(iu + 73.f, iv + 17.f);
    float variation = (tid - 0.5f) * 0.03f;

    // Cool stone tones
    Col3 c;
    if (tid < 0.33f)
        c = {base * 0.90f + variation, base * 0.85f + variation, base * 0.80f + variation};
    else if (tid < 0.66f)
        c = {base * 0.82f + variation, base * 0.82f + variation, base * 0.90f + variation};
    else
        c = {base * 0.85f + variation, base * 0.80f + variation, base * 0.88f + variation};

    // Stone grain
    float grain = fbm(wx * 14.f + tid * 100.f, wy * 14.f + tid2 * 100.f, 3);
    float fine = (grain - 0.5f) * 0.04f;
    c.r = clampf(c.r + fine, 0, 1);
    c.g = clampf(c.g + fine * 0.9f, 0, 1);
    c.b = clampf(c.b + fine * 0.85f, 0, 1);

    // Mineral veins
    float vein = value_noise(wx * 6.f + tid * 30.f, wy * 4.f + tid2 * 30.f);
    float vein_mask = smoothstep(0.47f, 0.53f, vein);
    c = c * (1.0f - (1.0f - vein_mask) * 0.05f);

    // Edge AO
    if (grout_edge < 1.0f) {
        float ao = 0.72f + grout_edge * 0.28f;
        c = c * ao;
    }

    return col_clamp(c);
}

// ── Render column ───────────────────────────────────────────────────────────

static void render_column(const Model& m, Frame& f, int col, int pixel_h) {
    float angle = m.pa - m.fov / 2.f + m.fov * (static_cast<float>(col) / f.w);
    RayHit hit = cast_ray(m, angle);

    float perp = hit.dist * std::cos(angle - m.pa);
    f.zbuf[col] = perp;

    float wall_h = static_cast<float>(pixel_h) / (perp + 0.001f);
    int wall_top = static_cast<int>((pixel_h - wall_h) / 2.f);
    int wall_bot = static_cast<int>((pixel_h + wall_h) / 2.f);

    // Wall hit world position for lighting
    float hit_wx, hit_wy;
    if (!hit.side) {
        hit_wx = static_cast<float>(hit.map_x) + (std::cos(angle) > 0 ? 0.f : 1.f);
        hit_wy = m.py + hit.dist * std::sin(angle);
    } else {
        hit_wx = m.px + hit.dist * std::cos(angle);
        hit_wy = static_cast<float>(hit.map_y) + (std::sin(angle) > 0 ? 0.f : 1.f);
    }

    // Fog color — very dark blue-black
    Col3 fog_color = {0.02f, 0.02f, 0.04f};

    for (int y = 0; y < pixel_h; ++y) {
        Col3 color;

        if (y < wall_top) {
            // ── Ceiling: dark stone ──
            float row_dist = static_cast<float>(pixel_h) / (pixel_h - 2.f * y + 0.001f);
            float ceil_x = m.px + row_dist * std::cos(angle);
            float ceil_y = m.py + row_dist * std::sin(angle);

            // Simple dark stone ceiling
            float n = value_noise(ceil_x * 3.f, ceil_y * 3.f);
            Col3 ceil_col = {0.06f + n * 0.03f, 0.05f + n * 0.025f, 0.06f + n * 0.02f};

            Col3 light = compute_light(m, ceil_x, ceil_y);
            float fog = std::fmin(row_dist * 0.05f, 0.80f);

            Col3 lit = col_clamp(Col3{
                ceil_col.r * light.r * 1.8f,
                ceil_col.g * light.g * 1.8f,
                ceil_col.b * light.b * 1.8f
            });
            color = col_lerp(lit, fog_color, fog);

        } else if (y >= wall_bot) {
            // ── Floor ──
            float row_dist = static_cast<float>(pixel_h) / (2.f * y - pixel_h + 0.001f);
            float floor_x = m.px + row_dist * std::cos(angle);
            float floor_y = m.py + row_dist * std::sin(angle);

            Col3 tex = floor_texture(floor_x, floor_y);
            Col3 light = compute_light(m, floor_x, floor_y);
            float fog = std::fmin(row_dist * 0.05f, 0.80f);

            Col3 lit = col_clamp(Col3{
                tex.r * light.r * 2.0f,
                tex.g * light.g * 2.0f,
                tex.b * light.b * 2.0f
            });

            // Torch glow puddles on floor
            for (int ti = 0; ti < NUM_TORCHES; ++ti) {
                float tdx = floor_x - g_torches[ti].x, tdy = floor_y - g_torches[ti].y;
                float td2 = tdx * tdx + tdy * tdy;
                if (td2 < 2.5f) {
                    float glow = (1.0f - td2 / 2.5f) * 0.06f;
                    float flicker = 0.85f + 0.15f * std::sin(m.frame * 0.18f + ti * 1.5f);
                    lit = lit + g_torches[ti].color * (glow * flicker);
                }
            }

            color = col_lerp(col_clamp(lit), fog_color, fog);

        } else {
            // ── Wall ──
            if (hit.wall_type == 0) {
                color = {0, 0, 0};
            } else {
                // V coordinate: 0 at top of wall, 1 at bottom
                float v_coord = static_cast<float>(y - wall_top) / (wall_bot - wall_top);
                Col3 tex = wall_texture(hit.wall_x, v_coord, hit.wall_type, hit.side);
                Col3 light = compute_light(m, hit_wx, hit_wy);

                float fog = std::fmin(perp * 0.04f, 0.75f);

                // Light the textured surface — moderate multiplier, no blowout
                Col3 lit = col_clamp(Col3{
                    tex.r * light.r * 2.0f,
                    tex.g * light.g * 2.0f,
                    tex.b * light.b * 2.0f,
                });

                color = col_lerp(lit, fog_color, fog);

                // Door glow — subtle
                if (hit.wall_type == 6) {
                    float glow = 0.5f + 0.5f * std::sin(m.frame * 0.15f);
                    color = color + Col3{glow * 0.06f, glow * 0.04f, 0.f};
                }
                // Exit pulse
                if (hit.wall_type == 9) {
                    float glow = 0.5f + 0.5f * std::sin(m.frame * 0.2f);
                    color = color + Col3{glow * 0.10f, glow * 0.02f, 0.f};
                }
            }
        }

        color = col_clamp(color);
        px_set(f, col, y,
               static_cast<uint8_t>(color.r * 255.f),
               static_cast<uint8_t>(color.g * 255.f),
               static_cast<uint8_t>(color.b * 255.f));
    }
}

// ── Render enemy sprite ─────────────────────────────────────────────────────

static void render_sprite(const Model& m, Frame& f, float sx, float sy, int type, int hp, int max_hp,
                          float sprite_scale, int pixel_h) {
    float dx = sx - m.px, dy = sy - m.py;
    float dist = std::sqrt(dx * dx + dy * dy);
    if (dist < 0.1f || dist > 20.f) return;

    float cos_a = std::cos(m.pa), sin_a = std::sin(m.pa);
    float inv_det = 1.0f / (std::cos(m.pa + PI / 2.f) * sin_a - std::sin(m.pa + PI / 2.f) * cos_a);
    float tx = inv_det * (sin_a * dx - cos_a * dy);
    float ty = inv_det * (-std::sin(m.pa + PI / 2.f) * dx + std::cos(m.pa + PI / 2.f) * dy);
    if (ty < 0.2f) return;

    int screen_x = static_cast<int>((f.w / 2.f) * (1.f + tx / ty));
    float h = sprite_scale * static_cast<float>(pixel_h) / ty;
    int s_top = static_cast<int>((pixel_h - h) / 2.f);
    int s_bot = static_cast<int>((pixel_h + h) / 2.f);
    int s_left = screen_x - static_cast<int>(h / 2.f);
    int s_right = screen_x + static_cast<int>(h / 2.f);

    float fog = std::fmin(ty * 0.05f, 0.75f);
    Col3 light = compute_light(m, sx, sy);

    Col3 body_col, head_col, eye_col;
    switch (type) {
        case 0: body_col = {0.65f, 0.18f, 0.14f}; head_col = {0.72f, 0.25f, 0.20f}; break;
        case 1: body_col = {0.16f, 0.55f, 0.22f}; head_col = {0.22f, 0.65f, 0.30f}; break;
        case 2: body_col = {0.22f, 0.22f, 0.60f}; head_col = {0.30f, 0.30f, 0.70f}; break;
        default: body_col = {0.4f, 0.4f, 0.4f}; head_col = {0.5f, 0.5f, 0.5f};
    }
    eye_col = {0.90f, 0.85f, 0.40f};

    Col3 fog_color = {0.02f, 0.02f, 0.04f};

    for (int col = std::max(0, s_left); col < std::min(f.w, s_right); ++col) {
        if (ty >= f.zbuf[col]) continue;

        float u = static_cast<float>(col - s_left) / (s_right - s_left);
        float cx = u - 0.5f;

        for (int row = std::max(0, s_top); row < std::min(pixel_h, s_bot); ++row) {
            float v = static_cast<float>(row - s_top) / (s_bot - s_top);
            float cy = v - 0.5f;

            Col3 c;
            bool draw = false;

            float body_r = 0.35f - std::fabs(cy) * 0.15f;
            if (v > 0.25f && v < 0.95f && std::fabs(cx) < body_r) {
                float shade = 1.0f - std::fabs(cx) / body_r * 0.3f;
                c = body_col * shade;
                draw = true;
            }

            float hdx = cx, hdy = cy - 0.15f;
            float hd = std::sqrt(hdx * hdx + hdy * hdy);
            if (hd < 0.18f) {
                float shade = 1.0f - hd / 0.18f * 0.25f;
                c = head_col * shade;
                draw = true;
                if (v > 0.28f && v < 0.36f) {
                    if (std::fabs(cx + 0.07f) < 0.03f || std::fabs(cx - 0.07f) < 0.03f)
                        c = eye_col;
                }
                if (v > 0.36f && v < 0.40f && std::fabs(cx) < 0.06f)
                    c = {0.15f, 0.06f, 0.06f};
            }

            float arm_w = (type == 2) ? 0.12f : 0.08f;
            if (v > 0.35f && v < 0.65f) {
                if (std::fabs(cx + 0.30f) < arm_w || std::fabs(cx - 0.30f) < arm_w) {
                    c = body_col * 0.80f;
                    draw = true;
                }
            }

            if (!draw) continue;

            Col3 lit = col_clamp(Col3{
                c.r * light.r * 1.8f,
                c.g * light.g * 1.8f,
                c.b * light.b * 1.8f,
            });
            Col3 final_c = col_clamp(col_lerp(lit, fog_color, fog));

            px_set(f, col, row,
                   static_cast<uint8_t>(final_c.r * 255.f),
                   static_cast<uint8_t>(final_c.g * 255.f),
                   static_cast<uint8_t>(final_c.b * 255.f));
        }
    }

    // Health bar
    if (hp < max_hp && hp > 0) {
        int bar_w = std::max(4, (s_right - s_left) / 2);
        int bar_y = std::max(0, s_top - 3);
        int bar_x = screen_x - bar_w / 2;
        float pct = static_cast<float>(hp) / max_hp;
        for (int bx = 0; bx < bar_w; ++bx) {
            int px = bar_x + bx;
            if (px < 0 || px >= f.w || ty >= f.zbuf[px]) continue;
            bool filled = (static_cast<float>(bx) / bar_w) < pct;
            if (filled) px_set(f, px, bar_y, 60, 200, 80);
            else        px_set(f, px, bar_y, 80, 30, 30);
        }
    }
}

// ── Render pickup sprite ────────────────────────────────────────────────────

static void render_pickup(const Model& m, Frame& f, float sx, float sy, int type, int pixel_h) {
    float dx = sx - m.px, dy = sy - m.py;
    float dist = std::sqrt(dx * dx + dy * dy);
    if (dist < 0.1f || dist > 15.f) return;

    float cos_a = std::cos(m.pa), sin_a = std::sin(m.pa);
    float inv_det = 1.0f / (std::cos(m.pa + PI / 2.f) * sin_a - std::sin(m.pa + PI / 2.f) * cos_a);
    float tx = inv_det * (sin_a * dx - cos_a * dy);
    float ty = inv_det * (-std::sin(m.pa + PI / 2.f) * dx + std::cos(m.pa + PI / 2.f) * dy);
    if (ty < 0.2f) return;

    int screen_x = static_cast<int>((f.w / 2.f) * (1.f + tx / ty));
    float h = 0.4f * static_cast<float>(pixel_h) / ty;
    int s_top = static_cast<int>((pixel_h - h) / 2.f);
    int s_bot = static_cast<int>((pixel_h + h) / 2.f);
    int s_left = screen_x - static_cast<int>(h / 2.f);
    int s_right = screen_x + static_cast<int>(h / 2.f);

    float pulse = 0.6f + 0.4f * std::sin(m.frame * 0.12f);
    Col3 color = (type == 0) ? Col3{0.15f, 0.70f * pulse, 0.22f}
                             : Col3{0.22f, 0.45f * pulse, 0.80f * pulse};
    Col3 fog_color = {0.02f, 0.02f, 0.04f};
    float fog = std::fmin(ty * 0.05f, 0.75f);

    for (int col = std::max(0, s_left); col < std::min(f.w, s_right); ++col) {
        if (ty >= f.zbuf[col]) continue;
        float u = static_cast<float>(col - s_left) / (s_right - s_left);
        for (int row = std::max(0, s_top); row < std::min(pixel_h, s_bot); ++row) {
            float v = static_cast<float>(row - s_top) / (s_bot - s_top);
            float cx = u - 0.5f, cy = v - 0.5f;
            float d = std::sqrt(cx * cx + cy * cy);
            if (d > 0.35f) continue;

            float glow = 1.0f - d / 0.35f;
            glow = glow * glow;
            Col3 c = color * glow;

            if (d < 0.12f) {
                float core = 1.0f - d / 0.12f;
                c = c + Col3{core * 0.3f, core * 0.3f, core * 0.3f};
            }

            Col3 final_c = col_clamp(col_lerp(c, fog_color, fog));
            px_set(f, col, row,
                   static_cast<uint8_t>(final_c.r * 255.f),
                   static_cast<uint8_t>(final_c.g * 255.f),
                   static_cast<uint8_t>(final_c.b * 255.f));
        }
    }
}

// ── Weapon sprite ───────────────────────────────────────────────────────────

static void draw_weapon(const Model& m, Frame& f, int pixel_h) {
    int w = f.w;
    int base_x = w / 2 + 4;
    int base_y = pixel_h + 2;

    float bob_x = std::sin(m.weapon_bob) * 4.f;
    float bob_y = std::fabs(std::cos(m.weapon_bob * 2.f)) * 3.f;
    float recoil = m.flash > 0 ? static_cast<float>(m.flash) * 4.f : 0.f;

    int ox = base_x + static_cast<int>(bob_x);
    int oy = base_y + static_cast<int>(bob_y + recoil);

    // Gun barrel — dark metal
    for (int dy = -22; dy <= 0; ++dy) {
        for (int dx = -4; dx <= 4; ++dx) {
            float t = 1.0f - static_cast<float>(-dy) / 22.f;
            float r = std::fabs(dx) / 4.5f;
            float shade = 0.18f + t * 0.08f;
            shade *= (1.0f - r * r * 0.4f);
            float highlight = (dx == -2 || dx == -3) ? 0.05f : 0.0f;

            float cr = clampf((shade + highlight) * 0.80f, 0, 1);
            float cg = clampf((shade + highlight) * 0.82f, 0, 1);
            float cb = clampf((shade + highlight) * 0.90f, 0, 1);
            px_set(f, ox + dx, oy + dy,
                   static_cast<uint8_t>(cr * 255.f),
                   static_cast<uint8_t>(cg * 255.f),
                   static_cast<uint8_t>(cb * 255.f));
        }
    }

    // Muzzle ring
    for (int dx = -3; dx <= 3; ++dx) {
        float r = std::fabs(dx) / 3.5f;
        float shade = 0.22f * (1.0f - r * 0.4f);
        px_set(f, ox + dx, oy - 22,
               static_cast<uint8_t>(shade * 200.f),
               static_cast<uint8_t>(shade * 205.f),
               static_cast<uint8_t>(shade * 220.f));
    }

    // Receiver
    for (int dy = 0; dy <= 5; ++dy) {
        for (int dx = -6; dx <= 6; ++dx) {
            float r = std::fabs(dx) / 7.f;
            float shade = 0.20f - r * 0.04f;
            px_set(f, ox + dx, oy + dy,
                   static_cast<uint8_t>(clampf(shade * 0.80f, 0, 1) * 255.f),
                   static_cast<uint8_t>(clampf(shade * 0.80f, 0, 1) * 255.f),
                   static_cast<uint8_t>(clampf(shade * 0.88f, 0, 1) * 255.f));
        }
    }

    // Grip — dark wood
    for (int dy = 5; dy <= 14; ++dy) {
        for (int dx = -4; dx <= 4; ++dx) {
            float t = static_cast<float>(dy - 5) / 9.f;
            float r = std::fabs(dx) / 5.f;
            float shade = 0.16f + t * 0.04f;
            shade *= (1.0f - r * 0.2f);
            float grain = std::sin(dy * 1.5f + dx * 0.3f) * 0.015f;
            shade += grain;
            px_set(f, ox + dx, oy + dy,
                   static_cast<uint8_t>(clampf(shade * 1.2f, 0, 1) * 255.f),
                   static_cast<uint8_t>(clampf(shade * 0.80f, 0, 1) * 255.f),
                   static_cast<uint8_t>(clampf(shade * 0.50f, 0, 1) * 255.f));
        }
    }

    // Muzzle flash
    if (m.flash > 0) {
        int flash_y = oy - 24;
        float strength = static_cast<float>(m.flash) / 4.f;

        int r1 = 4;
        for (int dy = -r1; dy <= r1; ++dy)
            for (int dx = -r1; dx <= r1; ++dx) {
                float d = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                if (d > r1) continue;
                float t = (1.0f - d / r1);
                px_blend(f, ox + dx, flash_y + dy, {1.0f, 0.95f, 0.7f}, t * t * strength);
            }

        int r2 = 7 + m.flash;
        for (int dy = -r2; dy <= r2; ++dy)
            for (int dx = -r2; dx <= r2; ++dx) {
                float d = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                if (d > r2) continue;
                float t = (1.0f - d / r2);
                px_blend(f, ox + dx, flash_y + dy, {1.0f, 0.70f, 0.25f}, t * t * strength * 0.35f);
            }
    }
}

// ── Crosshair ───────────────────────────────────────────────────────────────

static void draw_crosshair(const Model& m, Frame& f, int pixel_h) {
    int cx = f.w / 2, cy = pixel_h / 2;
    int gap = 2, len = 5;
    Col3 cc = {0.8f, 0.8f, 0.8f};
    float alpha = 0.7f;

    for (int i = gap; i < gap + len; ++i) {
        px_blend(f, cx + i, cy, cc, alpha);
        px_blend(f, cx - i, cy, cc, alpha);
        px_blend(f, cx, cy + i, cc, alpha);
        px_blend(f, cx, cy - i, cc, alpha);
    }
    px_blend(f, cx, cy, cc, 0.8f);
}

// ── Post-processing ─────────────────────────────────────────────────────────

static void post_process(const Model& m, Frame& f, int pixel_h) {
    int w = f.w, h = pixel_h;
    float center_x = w * 0.5f, center_y = h * 0.5f;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            Rgb& px = f.img(x, y);
            float r = px.r / 255.f, g = px.g / 255.f, b = px.b / 255.f;

            // Gentle vignette only — no bloom, no color grading blowout
            float dx = (x - center_x) / center_x, dy = (y - center_y) / center_y;
            float vig = 1.0f - (dx * dx * 0.5f + dy * dy * 0.3f) * 0.18f;
            vig = clampf(vig, 0.6f, 1.0f);
            r *= vig; g *= vig; b *= vig;

            // Damage flash — red tint
            if (m.hit_flash > 0) {
                float alpha = static_cast<float>(m.hit_flash) / 6.f * 0.30f;
                r = clampf(r + alpha * 0.4f, 0, 1);
                g *= (1.f - alpha * 0.4f);
                b *= (1.f - alpha * 0.4f);
            }

            // 4 bits a channel: the textures are noisy enough that finer steps
            // only multiply the distinct colours the terminal has to be sent.
            px.r = static_cast<uint8_t>(static_cast<int>(clampf(r, 0, 1) * 255.f) & 0xF0);
            px.g = static_cast<uint8_t>(static_cast<int>(clampf(g, 0, 1) * 255.f) & 0xF0);
            px.b = static_cast<uint8_t>(static_cast<int>(clampf(b, 0, 1) * 255.f) & 0xF0);
        }
    }
}

// ── Minimap ─────────────────────────────────────────────────────────────────
// Drawn as whole-cell pixels in the top right of the picture: 2 pixel rows
// per map row keep its cells square-ish.

static void draw_minimap(const Model& m, Frame& f) {
    const int map_size = std::min(12, std::min(f.w / 4, f.h / 6));
    if (map_size < 4) return;
    const int ox = f.w - map_size - 1, oy = 2;
    const int view_r = map_size / 2;
    const int pmx = int(m.px), pmy = int(m.py);
    for (int dy = -view_r; dy < view_r; ++dy)
        for (int dx = -view_r; dx < view_r; ++dx) {
            const int mx = pmx + dx, my = pmy + dy;
            Rgb c{20, 18, 25};
            if (mx < 0 || mx >= MAP_W || my < 0 || my >= MAP_H || g_map[my][mx] > 0) c = {80, 50, 45};
            for (const auto& e : m.enemies)
                if (e.active && int(e.x) == mx && int(e.y) == my) c = {220, 50, 50};
            if (mx == pmx && my == pmy) c = {230, 210, 60};
            const int sx = ox + dx + view_r, sy = oy + 2 * (dy + view_r);
            px_set(f, sx, sy, c.r, c.g, c.b);
            px_set(f, sx, sy + 1, c.r, c.g, c.b);
        }
}

// ── Program ─────────────────────────────────────────────────────────────────

struct Tick {};
struct Resize { int cols, rows; };
struct Move   { KeyAction k; };
struct Shoot {};
struct ToggleMap {};
struct Reset {};
struct Quit {};
using Msg = std::variant<Tick, Resize, Move, Shoot, ToggleMap, Reset, Quit>;

// A key held down arrives as repeats; each marks its action "held" for a
// few frames (tick reads key_last_seen), and cancels the opposite one.
constexpr KeyAction opposite(KeyAction k) {
    switch (k) {
        case K_FWD: return K_BACK;     case K_BACK: return K_FWD;
        case K_LEFT: return K_RIGHT;   case K_RIGHT: return K_LEFT;
        case K_TURN_L: return K_TURN_R; case K_TURN_R: return K_TURN_L;
        default: return K_COUNT;
    }
}

struct Fps {
    using Model = ::Model;
    using Msg   = ::Msg;
    using Cmd   = jaal::Cmd<Msg>;
    using Sub   = jaal::Sub<Msg, on_key, on_resize>;

    static Cmd update(Model& m, Tick)      { tick(m, 1.0f / 30.0f); return {}; }
    static Cmd update(Model& m, Resize r)  { m.w = r.cols; m.h = r.rows; return {}; }
    static Cmd update(Model& m, ToggleMap) { m.show_map = !m.show_map; return {}; }
    static Cmd update(Model& m, Reset)     { reset_game(m); return {}; }
    static Cmd update(Model&, Quit)        { return Cmd::quit(0); }
    static Cmd update(Model& m, Shoot)     { if (!m.dead && !m.won) shoot(m); return {}; }
    static Cmd update(Model& m, Move mv) {
        if (m.dead || m.won) return {};
        m.key_last_seen[mv.k] = m.frame;
        if (const KeyAction o = opposite(mv.k); o != K_COUNT) m.key_last_seen[o] = -100;
        return {};
    }

    static Image render(const Model& m) {
        const int pixel_h = (m.h - 1) * 2;
        Frame f(m.w, pixel_h);
        // Columns are independent (each writes only its own pixels and zbuf
        // slot), so they're spread over the cores, interleaved.
        const int n = std::max(1, std::min(static_cast<int>(std::thread::hardware_concurrency()), f.w / 8));
        {
            std::vector<std::jthread> pool;
            for (int t = 1; t < n; ++t)
                pool.emplace_back([&, t] { for (int x = t; x < f.w; x += n) render_column(m, f, x, pixel_h); });
            for (int x = 0; x < f.w; x += n) render_column(m, f, x, pixel_h);
        }

        std::vector<std::pair<float, const Enemy*>> sorted;             // far to near
        for (const auto& e : m.enemies) if (e.active) sorted.emplace_back(e.dist, &e);
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& [d, e] : sorted) {
            const float scale = (e->type == 1) ? 0.65f : (e->type == 2) ? 1.0f : 0.8f;
            render_sprite(m, f, e->x, e->y, e->type, e->hp, e->max_hp, scale, pixel_h);
        }
        for (const auto& p : m.pickups) if (!p.taken) render_pickup(m, f, p.x, p.y, p.type, pixel_h);

        draw_weapon(m, f, pixel_h);
        draw_crosshair(m, f, pixel_h);
        post_process(m, f, pixel_h);
        if (m.show_map) draw_minimap(m, f);
        return std::move(f.img);
    }

    static Element banner(const Model& m) {
        const auto black = Color::rgb(0, 0, 0);
        return center()(v(text(m.dead ? "YOU DIED" : "ESCAPE!") | Bold
                              | fgc(m.dead ? Color::rgb(220, 60, 60) : Color::rgb(80, 220, 120)),
                          text("[r] restart  [q] quit") | fgc(Color::rgb(150, 150, 160)),
                          text("SCORE: " + std::to_string(m.score) + "  KILLS: " + std::to_string(m.kills))
                              | Bold | fgc(Color::rgb(220, 180, 60)))
                        | pad<1> | bgc(black));
    }

    static Element status_bar(const Model& m) {
        const auto bg = Color::rgb(15, 12, 18);
        return h(text(" FPS") | Bold | fgc(Color::rgb(220, 180, 70)),
                 text(" │ [wasd] move │ [,/.] turn │ [space] shoot │ [m] map │ [q] quit") | fgc(Color::rgb(80, 75, 90)),
                 spacer(),
                 text("♥" + std::to_string(m.health) + " ") | Bold | fgc(Color::rgb(70, 210, 90)),
                 text("▪" + std::to_string(m.ammo) + " ") | Bold | fgc(Color::rgb(90, 180, 230)),
                 text(" K:" + std::to_string(m.kills) + "  ★" + std::to_string(m.score) + " ") | Bold | fgc(Color::rgb(220, 180, 70)))
               | bgc(bg);
    }

    static Element view(const Model& m) {
        if (m.w < 16 || m.h < 6) return text("");
        Element game = pixels(render(m));
        if (m.dead || m.won) game = zstack({std::move(game), banner(m)});
        return v(std::move(game), status_bar(m));
    }

    static Sub subscribe(const Model&) {
        return Sub::batch(
            Sub::every(33ms, Tick{}),
            Sub::on(on_resize{}, [](const ResizeEvent& r) -> std::optional<Msg> {
                return Resize{r.width.value, r.height.value};
            }),
            keys<Sub>({
                {'w', Move{K_FWD}}, {'s', Move{K_BACK}}, {'a', Move{K_LEFT}}, {'d', Move{K_RIGHT}},
                {SpecialKey::Up, Move{K_FWD}}, {SpecialKey::Down, Move{K_BACK}},
                {SpecialKey::Left, Move{K_TURN_L}}, {SpecialKey::Right, Move{K_TURN_R}},
                {',', Move{K_TURN_L}}, {'.', Move{K_TURN_R}},
                {' ', Shoot{}}, {'m', ToggleMap{}}, {'r', Reset{}}, {'q', Quit{}}, {SpecialKey::Escape, Quit{}},
            }));
    }
    static bool subs_key(const Model&) { return true; }
};

static_assert(Program<Fps>);

}  // namespace

int main() { return run<Fps>({.title = "fps"}); }
