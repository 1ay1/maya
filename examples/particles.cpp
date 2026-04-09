// maya -- Particle physics simulation
//
// Real-time particle system with thousands of particles rendered via
// half-block pixels (2x vertical resolution). Five modes with different
// physics: fireworks, galaxy, fountain, vortex, starfield.
//
// Keys: 1-5=mode  space=burst  r=reset  q/Esc=quit

#include <maya/maya.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace maya;

// -- Math helpers -----------------------------------------------------------

static constexpr float PI  = 3.14159265f;
static constexpr float TAU = 6.28318530f;

struct Vec2 { float x, y; };

static float length(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

// -- Particle ---------------------------------------------------------------

struct Particle {
    float x, y;           // position (pixel coords)
    float vx, vy;         // velocity
    float ax, ay;         // acceleration
    float life;           // remaining lifetime [0,1]
    float max_life;       // initial lifetime
    float r, g, b;        // base color
    float size;           // brightness multiplier
    uint8_t type;         // sub-type for mode-specific behavior
    // trail: previous positions
    static constexpr int TRAIL_LEN = 4;
    float trail_x[TRAIL_LEN];
    float trail_y[TRAIL_LEN];
    int trail_count;
};

// -- State ------------------------------------------------------------------

static std::mt19937 g_rng{42};
static std::vector<Particle> g_particles;
static int g_mode = 0;       // 0=fireworks, 1=galaxy, 2=fountain, 3=vortex, 4=starfield
static int g_pw = 0, g_ph = 0; // pixel dimensions
static float g_time = 0.f;
static int g_frame = 0;
static float g_fps_accum = 0.f;
static int g_fps_frames = 0;
static int g_fps = 0;

// Pixel buffer: RGB per pixel
struct Pixel { float r, g, b; };
static std::vector<Pixel> g_pixels;

// Style LUT: Q=6 quantization -> 216 colors, 216*216 fg/bg combos
static constexpr int Q = 6;
static uint16_t g_styles[Q * Q * Q][Q * Q * Q];
static uint16_t g_bar_bg, g_bar_text, g_bar_accent;

// -- Random helpers ---------------------------------------------------------

static float randf(float lo, float hi) {
    std::uniform_real_distribution<float> d(lo, hi);
    return d(g_rng);
}

static float randf01() { return randf(0.f, 1.f); }

static float gauss(float mean, float stddev) {
    std::normal_distribution<float> d(mean, stddev);
    return d(g_rng);
}

// -- Color helpers ----------------------------------------------------------

static void hsv_to_rgb(float h, float s, float v, float& r, float& g, float& b) {
    h = std::fmod(h, 1.f);
    if (h < 0.f) h += 1.f;
    int hi = static_cast<int>(h * 6.f);
    float f = h * 6.f - hi;
    float p = v * (1.f - s);
    float q = v * (1.f - f * s);
    float t = v * (1.f - (1.f - f) * s);
    switch (hi % 6) {
        case 0: r=v; g=t; b=p; break;
        case 1: r=q; g=v; b=p; break;
        case 2: r=p; g=v; b=t; break;
        case 3: r=p; g=q; b=v; break;
        case 4: r=t; g=p; b=v; break;
        default:r=v; g=p; b=q; break;
    }
}

// -- Pixel buffer -----------------------------------------------------------

static void clear_pixels() {
    std::fill(g_pixels.begin(), g_pixels.end(), Pixel{0.f, 0.f, 0.f});
}

static void plot(float fx, float fy, float r, float g, float b, float alpha = 1.f) {
    int px = static_cast<int>(fx);
    int py = static_cast<int>(fy);
    if (px < 0 || px >= g_pw || py < 0 || py >= g_ph) return;
    auto& p = g_pixels[py * g_pw + px];
    p.r += r * alpha;
    p.g += g * alpha;
    p.b += b * alpha;
}

// Plot with a soft glow (3x3 kernel)
static void plot_glow(float fx, float fy, float r, float g, float b, float brightness) {
    plot(fx, fy, r, g, b, brightness);
    float dim = brightness * 0.3f;
    plot(fx - 1, fy, r, g, b, dim);
    plot(fx + 1, fy, r, g, b, dim);
    plot(fx, fy - 1, r, g, b, dim);
    plot(fx, fy + 1, r, g, b, dim);
    float corner = brightness * 0.1f;
    plot(fx - 1, fy - 1, r, g, b, corner);
    plot(fx + 1, fy - 1, r, g, b, corner);
    plot(fx - 1, fy + 1, r, g, b, corner);
    plot(fx + 1, fy + 1, r, g, b, corner);
}

// -- Particle spawning per mode ---------------------------------------------

static void spawn_firework_rocket() {
    Particle p{};
    p.x = randf(g_pw * 0.1f, g_pw * 0.9f);
    p.y = static_cast<float>(g_ph - 1);
    p.vx = randf(-1.5f, 1.5f);
    p.vy = randf(-14.f, -9.f);
    p.ax = 0.f;
    p.ay = 0.15f; // gravity
    p.life = 1.f;
    p.max_life = randf(0.6f, 1.0f);
    p.life = p.max_life;
    p.r = 1.f; p.g = 0.9f; p.b = 0.7f;
    p.size = 1.5f;
    p.type = 0; // rocket
    p.trail_count = 0;
    g_particles.push_back(p);
}

static void spawn_firework_burst(float cx, float cy) {
    float hue = randf01();
    int count = static_cast<int>(randf(40, 80));
    for (int i = 0; i < count; ++i) {
        Particle p{};
        p.x = cx;
        p.y = cy;
        float angle = randf(0.f, TAU);
        float speed = randf(1.f, 8.f);
        p.vx = std::cos(angle) * speed;
        p.vy = std::sin(angle) * speed;
        p.ax = 0.f;
        p.ay = 0.12f;
        float lt = randf(0.5f, 1.2f);
        p.life = lt;
        p.max_life = lt;
        // Color variation around hue
        float h = hue + randf(-0.08f, 0.08f);
        hsv_to_rgb(h, randf(0.6f, 1.f), 1.f, p.r, p.g, p.b);
        p.size = randf(0.6f, 1.2f);
        p.type = 1; // burst particle
        p.trail_count = 0;
        g_particles.push_back(p);
    }
}

static void spawn_galaxy_particles(int count) {
    for (int i = 0; i < count; ++i) {
        Particle p{};
        float cx = g_pw * 0.5f;
        float cy = g_ph * 0.5f;
        // Spiral arm placement
        float arm = static_cast<float>(static_cast<int>(randf(0, 3))) * TAU / 3.f;
        float dist = randf(5.f, std::min(g_pw, g_ph) * 0.45f);
        float angle = arm + dist * 0.04f + randf(-0.3f, 0.3f);
        p.x = cx + std::cos(angle) * dist + gauss(0, 3.f);
        p.y = cy + std::sin(angle) * dist + gauss(0, 3.f);
        // Orbital velocity (perpendicular to radius)
        float dx = p.x - cx;
        float dy = p.y - cy;
        float r = std::sqrt(dx * dx + dy * dy) + 0.1f;
        float orbital_speed = 30.f / std::sqrt(r);
        p.vx = -dy / r * orbital_speed + gauss(0, 0.5f);
        p.vy =  dx / r * orbital_speed + gauss(0, 0.5f);
        p.ax = 0.f; p.ay = 0.f;
        p.life = 999.f; p.max_life = 999.f;
        p.r = 1.f; p.g = 0.8f; p.b = 0.6f;
        p.size = randf(0.4f, 1.0f);
        p.type = 0;
        p.trail_count = 0;
        g_particles.push_back(p);
    }
}

static void spawn_fountain_particles(int count) {
    float cx = g_pw * 0.5f;
    float base_y = g_ph * 0.85f;
    for (int i = 0; i < count; ++i) {
        Particle p{};
        p.x = cx + gauss(0, 3.f);
        p.y = base_y;
        p.vx = gauss(0, 3.f);
        p.vy = randf(-12.f, -7.f);
        p.ax = gauss(0.f, 0.02f); // slight wind
        p.ay = 0.18f; // gravity
        float lt = randf(1.0f, 2.5f);
        p.life = lt; p.max_life = lt;
        // Water-like blue-cyan
        float hue = randf(0.5f, 0.65f);
        hsv_to_rgb(hue, randf(0.5f, 0.9f), randf(0.7f, 1.f), p.r, p.g, p.b);
        p.size = randf(0.5f, 1.0f);
        p.type = 0;
        p.trail_count = 0;
        g_particles.push_back(p);
    }
}

static void spawn_vortex_particles(int count) {
    // Two vortex centers
    float v1x = g_pw * 0.35f, v1y = g_ph * 0.5f;
    float v2x = g_pw * 0.65f, v2y = g_ph * 0.5f;
    for (int i = 0; i < count; ++i) {
        Particle p{};
        // Spawn near one of the two vortices
        bool left = (i % 2 == 0);
        float cx = left ? v1x : v2x;
        float cy = left ? v1y : v2y;
        float angle = randf(0.f, TAU);
        float dist = randf(2.f, std::min(g_pw, g_ph) * 0.25f);
        p.x = cx + std::cos(angle) * dist;
        p.y = cy + std::sin(angle) * dist;
        p.vx = gauss(0, 1.f);
        p.vy = gauss(0, 1.f);
        p.ax = 0.f; p.ay = 0.f;
        float lt = randf(2.f, 5.f);
        p.life = lt; p.max_life = lt;
        float hue = left ? randf(0.0f, 0.1f) : randf(0.55f, 0.75f);
        hsv_to_rgb(hue, randf(0.6f, 1.f), randf(0.7f, 1.f), p.r, p.g, p.b);
        p.size = randf(0.4f, 0.9f);
        p.type = left ? 0 : 1;
        p.trail_count = 0;
        g_particles.push_back(p);
    }
}

static void spawn_starfield(int count) {
    float cx = g_pw * 0.5f;
    float cy = g_ph * 0.5f;
    for (int i = 0; i < count; ++i) {
        Particle p{};
        // Start near center with outward velocity
        float angle = randf(0.f, TAU);
        float dist = randf(1.f, 15.f);
        p.x = cx + std::cos(angle) * dist;
        p.y = cy + std::sin(angle) * dist;
        float dx = p.x - cx;
        float dy = p.y - cy;
        float r = std::sqrt(dx * dx + dy * dy) + 0.1f;
        float speed = randf(2.f, 6.f);
        p.vx = dx / r * speed;
        p.vy = dy / r * speed;
        p.ax = 0.f; p.ay = 0.f;
        float lt = randf(2.f, 5.f);
        p.life = lt; p.max_life = lt;
        // Mostly white/blue stars
        float temp = randf(0.f, 1.f);
        if (temp < 0.6f) {
            p.r = 0.9f; p.g = 0.9f; p.b = 1.0f;
        } else if (temp < 0.8f) {
            p.r = 1.0f; p.g = 0.85f; p.b = 0.6f;
        } else {
            p.r = 0.6f; p.g = 0.7f; p.b = 1.0f;
        }
        p.size = randf(0.3f, 1.2f);
        p.type = 0;
        p.trail_count = 0;
        g_particles.push_back(p);
    }
}

// -- Reset ------------------------------------------------------------------

static void reset_particles() {
    g_particles.clear();
    g_time = 0.f;
    switch (g_mode) {
        case 1: spawn_galaxy_particles(2500); break;
        case 2: break; // fountain spawns continuously
        case 3: spawn_vortex_particles(2000); break;
        case 4: spawn_starfield(1500); break;
        default: break; // fireworks spawn periodically
    }
}

// -- Physics update ---------------------------------------------------------

static void update_particles(float dt) {
    g_time += dt;

    // Mode-specific spawning
    switch (g_mode) {
        case 0: // Fireworks: periodic rocket launches
            if (g_frame % 15 == 0) spawn_firework_rocket();
            break;
        case 1: // Galaxy: maintain population
            if (g_particles.size() < 2500)
                spawn_galaxy_particles(5);
            break;
        case 2: // Fountain: continuous spray
            spawn_fountain_particles(8);
            break;
        case 3: // Vortex: maintain population
            if (g_particles.size() < 2000)
                spawn_vortex_particles(10);
            break;
        case 4: // Starfield: continuous spawning
            spawn_starfield(10);
            break;
    }

    float cx = g_pw * 0.5f;
    float cy = g_ph * 0.5f;

    for (auto& p : g_particles) {
        // Save trail
        if (p.trail_count < Particle::TRAIL_LEN) {
            p.trail_x[p.trail_count] = p.x;
            p.trail_y[p.trail_count] = p.y;
            p.trail_count++;
        } else {
            for (int i = 0; i < Particle::TRAIL_LEN - 1; ++i) {
                p.trail_x[i] = p.trail_x[i + 1];
                p.trail_y[i] = p.trail_y[i + 1];
            }
            p.trail_x[Particle::TRAIL_LEN - 1] = p.x;
            p.trail_y[Particle::TRAIL_LEN - 1] = p.y;
        }

        // Mode-specific forces
        switch (g_mode) {
            case 1: { // Galaxy: gravitational attraction to center
                float dx = cx - p.x;
                float dy = cy - p.y;
                float r = std::sqrt(dx * dx + dy * dy) + 1.f;
                float force = 800.f / (r * r);
                force = std::min(force, 5.f);
                p.ax = dx / r * force;
                p.ay = dy / r * force;
                // Damping
                p.vx *= 0.998f;
                p.vy *= 0.998f;
                break;
            }
            case 2: { // Fountain: slight wind oscillation
                p.ax = std::sin(g_time * 0.5f) * 0.05f;
                // Ground bounce
                float ground = g_ph * 0.85f;
                if (p.y >= ground && p.vy > 0) {
                    p.vy *= -0.3f;
                    p.vx *= 0.8f;
                    p.y = ground - 1;
                    // Splash: spawn tiny droplets
                    if (std::fabs(p.vy) > 1.f && g_particles.size() < 4000) {
                        for (int s = 0; s < 2; ++s) {
                            Particle sp{};
                            sp.x = p.x + randf(-2, 2);
                            sp.y = ground - 1;
                            sp.vx = randf(-2, 2);
                            sp.vy = randf(-3, -1);
                            sp.ax = 0; sp.ay = 0.18f;
                            float lt = randf(0.2f, 0.5f);
                            sp.life = lt; sp.max_life = lt;
                            sp.r = p.r * 0.8f; sp.g = p.g * 0.8f; sp.b = p.b * 0.8f;
                            sp.size = 0.3f;
                            sp.type = 1;
                            sp.trail_count = 0;
                            g_particles.push_back(sp);
                        }
                    }
                }
                break;
            }
            case 3: { // Vortex: two attractors with tangential force
                float v1x = g_pw * 0.35f + std::sin(g_time * 0.3f) * g_pw * 0.05f;
                float v1y = g_ph * 0.5f + std::cos(g_time * 0.4f) * g_ph * 0.05f;
                float v2x = g_pw * 0.65f + std::cos(g_time * 0.35f) * g_pw * 0.05f;
                float v2y = g_ph * 0.5f + std::sin(g_time * 0.45f) * g_ph * 0.05f;
                float atx = (p.type == 0) ? v1x : v2x;
                float aty = (p.type == 0) ? v1y : v2y;
                float dx = atx - p.x;
                float dy = aty - p.y;
                float r = std::sqrt(dx * dx + dy * dy) + 1.f;
                float force = 200.f / (r + 10.f);
                // Radial + tangential
                p.ax = dx / r * force + (-dy / r) * force * 0.8f;
                p.ay = dy / r * force + ( dx / r) * force * 0.8f;
                p.vx *= 0.99f;
                p.vy *= 0.99f;
                break;
            }
            case 4: { // Starfield: accelerate outward
                float dx = p.x - cx;
                float dy = p.y - cy;
                float r = std::sqrt(dx * dx + dy * dy) + 0.1f;
                float accel = 1.5f + r * 0.03f; // faster further out
                p.ax = dx / r * accel;
                p.ay = dy / r * accel;
                break;
            }
            default: break;
        }

        // Integrate
        p.vx += p.ax * dt;
        p.vy += p.ay * dt;
        p.x += p.vx;
        p.y += p.vy;
        p.life -= dt;

        // Fireworks: rocket explodes when velocity reverses or near apex
        if (g_mode == 0 && p.type == 0 && p.vy >= -1.f) {
            spawn_firework_burst(p.x, p.y);
            p.life = 0.f;
        }
    }

    // Remove dead particles and out-of-bounds
    std::erase_if(g_particles, [](const Particle& p) {
        if (p.life <= 0.f) return true;
        float margin = 20.f;
        return p.x < -margin || p.x >= g_pw + margin ||
               p.y < -margin || p.y >= g_ph + margin;
    });

    // Cap particle count
    while (g_particles.size() > 5000)
        g_particles.erase(g_particles.begin());
}

// -- Render particles into pixel buffer -------------------------------------

static void render_particles() {
    clear_pixels();

    for (const auto& p : g_particles) {
        float age = 1.f - (p.life / p.max_life); // 0=new, 1=dead
        float fade = std::max(0.f, p.life / p.max_life);

        // Color based on mode
        float cr = p.r, cg = p.g, cb = p.b;

        if (g_mode == 1) {
            // Galaxy: color by velocity magnitude
            float speed = length({p.vx, p.vy});
            float t = std::clamp(speed / 15.f, 0.f, 1.f);
            // slow=red, mid=yellow, fast=blue
            if (t < 0.5f) {
                float s = t * 2.f;
                cr = 1.f - s * 0.5f;
                cg = s;
                cb = s * 0.3f;
            } else {
                float s = (t - 0.5f) * 2.f;
                cr = 0.5f - s * 0.5f;
                cg = 1.f - s * 0.5f;
                cb = 0.3f + s * 0.7f;
            }
        } else if (g_mode == 4) {
            // Starfield: brighter further from center
            float dx = p.x - g_pw * 0.5f;
            float dy = p.y - g_ph * 0.5f;
            float dist = std::sqrt(dx * dx + dy * dy);
            float maxd = std::sqrt(float(g_pw * g_pw + g_ph * g_ph)) * 0.5f;
            float t = std::clamp(dist / maxd, 0.f, 1.f);
            fade *= (0.3f + t * 0.7f);
        }

        float brightness = fade * p.size;

        // Draw trails
        for (int t = 0; t < p.trail_count; ++t) {
            float trail_fade = static_cast<float>(t + 1) / static_cast<float>(p.trail_count + 1);
            trail_fade *= 0.3f * fade;
            plot(p.trail_x[t], p.trail_y[t], cr, cg, cb, trail_fade * p.size);
        }

        // Draw particle with glow
        if (p.size > 0.8f) {
            plot_glow(p.x, p.y, cr, cg, cb, brightness);
        } else {
            plot(p.x, p.y, cr, cg, cb, brightness);
        }

        // Firework sparkle effect
        if (g_mode == 0 && p.type == 1 && age > 0.5f) {
            float sparkle = (std::sin(g_time * 30.f + p.x * 7.f + p.y * 11.f) + 1.f) * 0.5f;
            if (sparkle > 0.7f) {
                plot(p.x, p.y, 1.f, 1.f, 1.f, sparkle * fade * 0.5f);
            }
        }

        // Starfield streak lines
        if (g_mode == 4) {
            float speed = length({p.vx, p.vy});
            if (speed > 3.f) {
                float streak_len = std::min(speed * 0.4f, 8.f);
                float nx = -p.vx / speed;
                float ny = -p.vy / speed;
                for (float s = 1.f; s < streak_len; s += 1.f) {
                    float sf = 1.f - s / streak_len;
                    plot(p.x + nx * s, p.y + ny * s, cr, cg, cb, brightness * sf * 0.5f);
                }
            }
        }
    }
}

// -- Style interning --------------------------------------------------------

static uint8_t to8(int qi) {
    return static_cast<uint8_t>(qi * 255 / (Q - 1));
}

static int quantize(float v) {
    v = std::clamp(v, 0.f, 1.f);
    return std::clamp(static_cast<int>(v * (Q - 0.01f)), 0, Q - 1);
}

static int color_idx(float r, float g, float b) {
    return quantize(r) * Q * Q + quantize(g) * Q + quantize(b);
}

static void intern_styles(StylePool& pool, int w, int h) {
    g_pw = w;
    g_ph = h * 2; // half-block: 2 pixels per cell row
    g_pixels.assign(static_cast<size_t>(g_pw) * g_ph, Pixel{0, 0, 0});

    for (int fi = 0; fi < Q * Q * Q; ++fi) {
        int fr = fi / (Q * Q), fg = (fi / Q) % Q, fb = fi % Q;
        for (int bi = 0; bi < Q * Q * Q; ++bi) {
            int br = bi / (Q * Q), bg_ = (bi / Q) % Q, bb = bi % Q;
            g_styles[fi][bi] = pool.intern(
                Style{}.with_fg(Color::rgb(to8(fr), to8(fg), to8(fb)))
                       .with_bg(Color::rgb(to8(br), to8(bg_), to8(bb))));
        }
    }
    g_bar_bg     = pool.intern(Style{}.with_fg(Color::rgb(120, 120, 120)).with_bg(Color::rgb(18, 18, 24)));
    g_bar_text   = pool.intern(Style{}.with_fg(Color::rgb(180, 180, 180)).with_bg(Color::rgb(18, 18, 24)));
    g_bar_accent = pool.intern(Style{}.with_fg(Color::rgb(80, 200, 255)).with_bg(Color::rgb(18, 18, 24)).with_bold());

    reset_particles();
}

// -- Event ------------------------------------------------------------------

static bool handle_event(const Event& ev) {
    if (key(ev, 'q') || key(ev, SpecialKey::Escape)) return false;

    on(ev, '1', [] { g_mode = 0; reset_particles(); });
    on(ev, '2', [] { g_mode = 1; reset_particles(); });
    on(ev, '3', [] { g_mode = 2; reset_particles(); });
    on(ev, '4', [] { g_mode = 3; reset_particles(); });
    on(ev, '5', [] { g_mode = 4; reset_particles(); });
    on(ev, 'r', [] { reset_particles(); });

    on(ev, ' ', [] {
        // Burst at random position
        float bx = randf(g_pw * 0.15f, g_pw * 0.85f);
        float by = randf(g_ph * 0.15f, g_ph * 0.85f);
        if (g_mode == 0) {
            spawn_firework_burst(bx, by);
        } else {
            // Generic colorful burst
            float hue = randf01();
            int count = 60;
            for (int i = 0; i < count; ++i) {
                Particle p{};
                p.x = bx; p.y = by;
                float angle = randf(0.f, TAU);
                float speed = randf(2.f, 10.f);
                p.vx = std::cos(angle) * speed;
                p.vy = std::sin(angle) * speed;
                p.ax = 0.f;
                p.ay = (g_mode == 2) ? 0.15f : 0.f;
                float lt = randf(0.5f, 1.5f);
                p.life = lt; p.max_life = lt;
                float h = hue + randf(-0.1f, 0.1f);
                hsv_to_rgb(h, 0.9f, 1.f, p.r, p.g, p.b);
                p.size = randf(0.5f, 1.2f);
                p.type = 1;
                p.trail_count = 0;
                g_particles.push_back(p);
            }
        }
    });

    return true;
}

// -- Paint ------------------------------------------------------------------

static void paint(Canvas& canvas, int W, int H) {
    if (W != g_pw || H < 3) return;

    using Clock = std::chrono::steady_clock;
    static auto last = Clock::now();
    auto now = Clock::now();
    float dt = std::chrono::duration<float>(now - last).count();
    last = now;
    dt = std::min(dt, 0.05f);

    // FPS counter
    g_fps_accum += dt;
    g_fps_frames++;
    if (g_fps_accum >= 0.5f) {
        g_fps = static_cast<int>(g_fps_frames / g_fps_accum);
        g_fps_accum = 0.f;
        g_fps_frames = 0;
    }
    g_frame++;

    // Physics step
    update_particles(dt);

    // Render to pixel buffer
    render_particles();

    // Convert pixel buffer to half-block cells
    int canvas_h = H - 1; // reserve last row for status bar
    for (int cy = 0; cy < canvas_h; ++cy) {
        int py_top = cy * 2;
        int py_bot = cy * 2 + 1;
        for (int cx = 0; cx < W; ++cx) {
            auto clamp01 = [](float v) { return std::clamp(v, 0.f, 1.f); };

            Pixel top = (py_top < g_ph) ? g_pixels[py_top * g_pw + cx] : Pixel{0,0,0};
            Pixel bot = (py_bot < g_ph) ? g_pixels[py_bot * g_pw + cx] : Pixel{0,0,0};

            // Tone map (simple clamp with slight gamma)
            auto tm = [&](float v) { return clamp01(std::sqrt(clamp01(v))); };

            int fi = color_idx(tm(top.r), tm(top.g), tm(top.b));
            int bi = color_idx(tm(bot.r), tm(bot.g), tm(bot.b));
            canvas.set(cx, cy, U'\u2580', g_styles[fi][bi]);
        }
    }

    // Status bar
    int bar_y = H - 1;
    for (int x = 0; x < W; ++x)
        canvas.set(x, bar_y, U' ', g_bar_bg);

    static const char* mode_names[] = {
        "FIREWORKS", "GALAXY", "FOUNTAIN", "VORTEX", "STARFIELD"
    };

    char buf[256];
    std::snprintf(buf, sizeof(buf),
        " PARTICLES \xe2\x94\x82 %s \xe2\x94\x82 %d particles \xe2\x94\x82 %d fps \xe2\x94\x82 [1-5] mode [spc] burst [r] reset [q] quit",
        mode_names[g_mode],
        static_cast<int>(g_particles.size()),
        g_fps);

    canvas.write_text(0, bar_y, buf, g_bar_bg);
    canvas.write_text(1, bar_y, "PARTICLES", g_bar_accent);

    // Highlight mode name
    char mode_pos_buf[64];
    std::snprintf(mode_pos_buf, sizeof(mode_pos_buf), "%s", mode_names[g_mode]);
    // Find position after "PARTICLES | "
    canvas.write_text(13, bar_y, mode_pos_buf, g_bar_accent);
}

// -- Main -------------------------------------------------------------------

int main() {
    (void)canvas_run(
        CanvasConfig{.fps = 60, .mouse = false, .mode = Mode::Fullscreen, .title = "particles"},
        intern_styles,
        handle_event,
        paint
    );
}
