// maya — interactive fireworks with physics-based particles
//
// Click anywhere to launch. Auto-launches when idle.
// Physics: gravity, drag, sparkle trails, color aging.
//
// Mouse: click = launch at cursor
// Keys:  q/ESC = quit   space = launch   1-6 = palette   p = pause

#include <maya/maya.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

using namespace maya;

// ── Random ───────────────────────────────────────────────────────────────────

static std::mt19937 g_rng{std::random_device{}()};

static float randf(float lo, float hi) {
    return std::uniform_real_distribution<float>{lo, hi}(g_rng);
}
static int randi(int lo, int hi) {
    return std::uniform_int_distribution<int>{lo, hi}(g_rng);
}

// ── Constants ────────────────────────────────────────────────────────────────

static constexpr float kDt      = 1.f / 60.f;
static constexpr float kGravity = 28.f;       // cells/s² downward
static constexpr float kDrag    = 0.975f;      // velocity decay per frame
static constexpr int   kPalCount = 6;
static constexpr int   kGrad     = 12;         // gradient steps per palette

// Particle glyphs — large→small as life fades
static constexpr char32_t kBurst[]  = {U'✦', U'✦', U'●', U'•', U'·', U'·'};
static constexpr char32_t kSpark[]  = {U'*', U'·', U'.', U'.', U'.', U'.'};
static constexpr int kGlyphs = 6;

// ── Palette definitions (RGB endpoints) ──────────────────────────────────────

struct RGB { uint8_t r, g, b; };

static constexpr RGB lerp_rgb(RGB a, RGB b, float t) {
    return {
        static_cast<uint8_t>(static_cast<float>(a.r) + (static_cast<float>(b.r) - static_cast<float>(a.r)) * t),
        static_cast<uint8_t>(static_cast<float>(a.g) + (static_cast<float>(b.g) - static_cast<float>(a.g)) * t),
        static_cast<uint8_t>(static_cast<float>(a.b) + (static_cast<float>(b.b) - static_cast<float>(a.b)) * t),
    };
}

// Each palette: white flash → bright color → dim color → near-black
struct PalDef { RGB bright, mid, dim; };

static constexpr PalDef kPals[kPalCount] = {
    {{255, 80, 60},  {220, 30, 15},  {80, 8, 4}},      // Ruby
    {{70, 140, 255}, {30, 70, 220},  {8, 15, 80}},      // Sapphire
    {{60, 240, 120}, {20, 180, 60},  {5, 60, 15}},      // Emerald
    {{210, 80, 255}, {150, 30, 200}, {50, 8, 70}},      // Amethyst
    {{255, 200, 50}, {240, 150, 10}, {80, 45, 2}},      // Gold
    {{255, 100, 170},{220, 40, 100}, {80, 10, 35}},     // Rose
};
static constexpr RGB kWhite = {255, 255, 255};

static RGB palette_color(int pal, float life) {
    // life: 1.0 = just born (white flash), 0.0 = dead (dim)
    const auto& p = kPals[pal];
    if (life > 0.85f) {
        float t = (life - 0.85f) / 0.15f;     // 1→0 as life drops from 1→0.85
        return lerp_rgb(p.bright, kWhite, t);
    }
    if (life > 0.45f) {
        float t = (life - 0.45f) / 0.40f;     // 1→0
        return lerp_rgb(p.mid, p.bright, t);
    }
    float t = life / 0.45f;                    // 1→0
    return lerp_rgb(p.dim, p.mid, t);
}

// ── Styles ───────────────────────────────────────────────────────────────────

struct Styles {
    uint16_t pal[kPalCount][kGrad];   // particle gradients
    uint16_t rocket;                   // ascending rocket
    uint16_t trail;                    // rocket trail sparks
    uint16_t star[3];                  // background stars (dim levels)
    uint16_t ground;                   // ground shimmer
    uint16_t text;                     // UI overlay
    uint16_t text_dim;                 // dim UI text
};

static Styles g_sty{};

static Styles intern_all(StylePool& pool) {
    Styles s{};

    for (int p = 0; p < kPalCount; ++p) {
        for (int g = 0; g < kGrad; ++g) {
            float life = 1.f - static_cast<float>(g) / static_cast<float>(kGrad - 1);
            auto c = palette_color(p, life);
            Style st = Style{}.with_fg(Color::rgb(c.r, c.g, c.b));
            if (g == 0) st = st.with_bold();
            s.pal[p][g] = pool.intern(st);
        }
    }

    s.rocket   = pool.intern(Style{}.with_bold().with_fg(Color::rgb(255, 240, 200)));
    s.trail    = pool.intern(Style{}.with_fg(Color::rgb(255, 180, 80)));
    s.star[0]  = pool.intern(Style{}.with_fg(Color::rgb(60, 60, 75)));
    s.star[1]  = pool.intern(Style{}.with_fg(Color::rgb(40, 40, 55)));
    s.star[2]  = pool.intern(Style{}.with_dim().with_fg(Color::rgb(25, 25, 38)));
    s.ground   = pool.intern(Style{}.with_fg(Color::rgb(60, 50, 30)));
    s.text     = pool.intern(Style{}.with_bold().with_fg(Color::rgb(200, 200, 210)));
    s.text_dim = pool.intern(Style{}.with_fg(Color::rgb(70, 70, 85)));

    return s;
}

// ── Particles ────────────────────────────────────────────────────────────────

struct Particle {
    float x, y, vx, vy;
    float life;       // 1 → 0
    float decay;      // life lost per frame
    int   pal;
    bool  spark;      // true = small trail spark, false = main burst particle
};

struct Rocket {
    float x, y, vy;
    float target_y;
    int   pal;
    float trail_acc;  // fractional trail emission accumulator
};

static std::vector<Particle> g_parts;
static std::vector<Rocket>   g_rockets;
static float g_auto_timer = 0.8f;
static bool  g_paused     = false;

// Background stars
struct Star { int x, y; int level; }; // level 0=bright, 1=mid, 2=dim
static std::vector<Star> g_stars;

static void gen_stars(int W, int H) {
    g_stars.clear();
    int count = W * H / 40; // ~2.5% density
    g_stars.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        g_stars.push_back({randi(0, W - 1), randi(0, H - 2), randi(0, 2)});
    }
}

// ── Explosion shapes ─────────────────────────────────────────────────────────

enum class BurstShape { Sphere, Ring, Willow, Chrysanthemum };

static void explode(float x, float y, int pal) {
    auto shape = static_cast<BurstShape>(randi(0, 3));
    int count = randi(80, 160);

    for (int i = 0; i < count; ++i) {
        float angle = randf(0, 6.2831853f);
        float speed;

        switch (shape) {
        case BurstShape::Sphere:
            speed = randf(4.f, 22.f);
            break;
        case BurstShape::Ring:
            speed = randf(14.f, 22.f); // only outer ring
            break;
        case BurstShape::Willow:
            speed = randf(2.f, 14.f);
            break;
        case BurstShape::Chrysanthemum:
            speed = randf(8.f, 28.f);
            break;
        }

        float decay;
        switch (shape) {
        case BurstShape::Willow:
            decay = randf(0.004f, 0.010f); // very long lived
            break;
        case BurstShape::Chrysanthemum:
            decay = randf(0.006f, 0.014f);
            break;
        default:
            decay = randf(0.008f, 0.022f);
            break;
        }

        float cos_a = std::cos(angle);
        float sin_a = std::sin(angle);

        g_parts.push_back({
            x, y,
            cos_a * speed,
            sin_a * speed - randf(0.f, 4.f), // slight upward bias
            1.f,
            decay,
            pal,
            false,
        });
    }

    // Center flash particles — bright, short-lived
    for (int i = 0; i < 12; ++i) {
        float angle = randf(0, 6.2831853f);
        float speed = randf(1.f, 5.f);
        g_parts.push_back({
            x, y,
            std::cos(angle) * speed,
            std::sin(angle) * speed,
            1.f,
            0.06f,  // fast decay = flash
            pal,
            true,
        });
    }
}

static void launch_rocket(float x, float target_y, int pal) {
    g_rockets.push_back({
        x,
        0.f,    // set to screen bottom in update
        randf(-35.f, -22.f), // upward velocity
        target_y,
        pal,
        0.f,
    });
}

// ── Physics ──────────────────────────────────────────────────────────────────

static void update(int W, int H) {
    if (g_paused) return;

    const float bottom = static_cast<float>(H - 1);

    // Update rockets
    for (auto& r : g_rockets) {
        if (r.y == 0.f) r.y = bottom; // first frame: start at bottom
        r.y += r.vy * kDt;

        // Emit trail sparks (2-3 per frame)
        r.trail_acc += 2.5f;
        while (r.trail_acc >= 1.f) {
            r.trail_acc -= 1.f;
            g_parts.push_back({
                r.x + randf(-0.3f, 0.3f),
                r.y + randf(0.f, 1.f),
                randf(-1.5f, 1.5f),
                randf(2.f, 6.f),   // downward drift
                randf(0.4f, 0.8f),
                randf(0.04f, 0.08f),
                r.pal,
                true,
            });
        }
    }

    // Explode rockets that reached target
    std::erase_if(g_rockets, [](const Rocket& r) {
        if (r.y <= r.target_y) {
            explode(r.x, r.y, r.pal);
            return true;
        }
        return false;
    });

    // Update particles
    for (auto& p : g_parts) {
        p.vx *= kDrag;
        p.vy *= kDrag;
        p.vy += kGravity * kDt; // gravity
        p.x += p.vx * kDt;
        p.y += p.vy * kDt;
        p.life -= p.decay;
    }
    std::erase_if(g_parts, [bottom](const Particle& p) {
        return p.life <= 0.f || p.y > bottom + 2.f;
    });

    // Auto-launch
    g_auto_timer -= kDt;
    if (g_auto_timer <= 0.f) {
        int count = randi(1, 3); // sometimes launch volleys
        for (int i = 0; i < count; ++i) {
            float x = randf(static_cast<float>(W) * 0.1f,
                            static_cast<float>(W) * 0.9f);
            float target = randf(static_cast<float>(H) * 0.1f,
                                 static_cast<float>(H) * 0.45f);
            launch_rocket(x, target, randi(0, kPalCount - 1));
        }
        g_auto_timer = randf(0.6f, 2.2f);
    }
}

// ── Paint ────────────────────────────────────────────────────────────────────

static void paint(Canvas& canvas, int W, int H) {
    // Background stars (twinkle: occasionally skip drawing)
    for (const auto& s : g_stars) {
        if (s.x < W && s.y < H - 1) {
            // 5% chance to not draw = twinkle
            if (randi(0, 19) == 0) continue;
            canvas.set(s.x, s.y, U'·', g_sty.star[s.level]);
        }
    }

    // Ground line — subtle shimmer
    for (int x = 0; x < W; ++x) {
        if (randi(0, 3) == 0)
            canvas.set(x, H - 2, U'▁', g_sty.ground);
    }

    // Rockets
    for (const auto& r : g_rockets) {
        int cx = static_cast<int>(r.x);
        int cy = static_cast<int>(r.y);
        if (cx >= 0 && cx < W && cy >= 0 && cy < H - 1)
            canvas.set(cx, cy, U'▮', g_sty.rocket);
        // Rocket head glow — dim the cells around it
        if (cy - 1 >= 0 && cy - 1 < H - 1 && cx >= 0 && cx < W)
            canvas.set(cx, cy - 1, U'⁎', g_sty.trail);
    }

    // Particles
    for (const auto& p : g_parts) {
        int cx = static_cast<int>(p.x);
        int cy = static_cast<int>(p.y);
        if (cx < 0 || cx >= W || cy < 0 || cy >= H - 1) continue;

        int gi = std::clamp(
            static_cast<int>((1.f - p.life) * static_cast<float>(kGrad)),
            0, kGrad - 1);

        int ci = std::clamp(
            static_cast<int>((1.f - p.life) * static_cast<float>(kGlyphs)),
            0, kGlyphs - 1);

        char32_t ch = p.spark ? kSpark[ci] : kBurst[ci];
        canvas.set(cx, cy, ch, g_sty.pal[p.pal][gi]);
    }

    // Status bar
    int bar_y = H - 1;
    canvas.write_text(1, bar_y, "click to launch", g_sty.text_dim);

    char buf[48];
    int n = std::snprintf(buf, sizeof(buf), "%zu particles",
                          g_parts.size());
    if (n > 0 && W > n + 2)
        canvas.write_text(W - n - 1, bar_y, {buf, static_cast<std::size_t>(n)}, g_sty.text_dim);

    if (g_paused)
        canvas.write_text(W / 2 - 3, bar_y, "PAUSED", g_sty.text);
}

// ── main ─────────────────────────────────────────────────────────────────────

int main()
{
    auto result = canvas_run(
        CanvasConfig{.fps = 60, .mouse = true, .title = "fireworks · maya"},

        // on_resize
        [&](StylePool& pool, int W, int H) {
            g_sty = intern_all(pool);
            gen_stars(W, H);
        },

        // on_event
        [&](const Event& ev) -> bool {
            if (key(ev, 'q') || key(ev, SpecialKey::Escape)) return false;

            on(ev, 'p', [&] { g_paused = !g_paused; });
            on(ev, ' ', [&] {
                // Manual launch at random position
                launch_rocket(randf(10.f, 200.f), randf(5.f, 30.f),
                              randi(0, kPalCount - 1));
            });

            // Click to launch at cursor
            if (mouse_clicked(ev)) {
                auto pos = mouse_pos(ev);
                if (pos) {
                    float x = static_cast<float>(pos->col);
                    float target = static_cast<float>(pos->row);
                    launch_rocket(x, std::max(3.f, target), randi(0, kPalCount - 1));
                    g_auto_timer = std::max(g_auto_timer, 1.5f); // delay auto after click
                }
            }

            return true;
        },

        // on_paint
        [&](Canvas& canvas, int W, int H) {
            update(W, H);
            paint(canvas, W, H);
        }
    );

    if (!result) {
        std::println(std::cerr, "maya: {}", result.error().message);
        return 1;
    }
}
