// examples/particles.cpp — five particle systems, as a jaal program.
//
//   1 fireworks   rockets rise, burst into coloured sparks that sparkle out
//   2 galaxy      a three-armed spiral held by gravity, coloured by speed
//   3 fountain    a spray that falls, bounces and splashes
//   4 vortex      two drifting whirlpools, warm and cool
//   5 starfield   stars stream out from the centre with speed streaks
//
//   Model      every particle (position, velocity, acceleration, life,
//              colour, size, a short trail), the mode, simulated time,
//              the frame count and the RNG.
//   update()   Tick spawns what the mode spawns and steps every particle;
//              keys switch mode, reset, or throw a burst.
//   view()     the particles accumulated into a light buffer (additive,
//              with glows, trails, sparkle and streaks), tone-mapped into
//              an Image; and a status bar.
//
// Keys: 1-5 mode   space burst   r reset   q quit

#include <maya/element/pixels.hpp>
#include <maya/app.hpp>
#include <maya/maya.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <random>
#include <string>
#include <variant>
#include <vector>

using namespace maya;
using namespace maya::dsl;
using namespace std::chrono_literals;

namespace {

constexpr float kTau = 6.28318530f;
constexpr float kDt = 1.0f / 60.0f;           // one Tick of simulated time
constexpr int   kTrail = 4, kMaxParticles = 5000;

enum class Mode { Fireworks, Galaxy, Fountain, Vortex, Starfield };
constexpr std::array<const char*, 5> kModeName = {"FIREWORKS", "GALAXY", "FOUNTAIN", "VORTEX", "STARFIELD"};

struct Colour { float r = 0, g = 0, b = 0; };

Colour hsv(float h, float s, float v) {
    h = std::fmod(h, 1.f); if (h < 0) h += 1.f;
    const int i = static_cast<int>(h * 6.f);
    const float f = h * 6.f - i, p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    switch (i % 6) {
        case 0: return {v, t, p}; case 1: return {q, v, p}; case 2: return {p, v, t};
        case 3: return {p, q, v}; case 4: return {t, p, v}; default: return {v, p, q};
    }
}

// ── model ────────────────────────────────────────────────────────────────────

struct Particle {
    float x = 0, y = 0, vx = 0, vy = 0, ax = 0, ay = 0;
    float life = 1, max_life = 1;
    Colour c;
    float size = 1;
    bool  rocket = false, spark = false;     // fireworks: a rising rocket / a burst spark
    bool  second = false;                    // vortex: which whirlpool
    std::array<std::pair<float, float>, kTrail> trail{};
    int trail_n = 0;
};

struct Model {
    int w = 0, h = 0;                        // the field, in pixels
    std::vector<Particle> ps;
    Mode mode = Mode::Fireworks;
    float time = 0;
    int frame = 0;
    std::mt19937 rng{42};

    float uni(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); }
    float gauss(float sd)         { return std::normal_distribution<float>(0.f, sd)(rng); }
};

// ── messages ─────────────────────────────────────────────────────────────────

struct Tick {};
struct Resize  { int cols, rows; };
struct SetMode { Mode m; };
struct Burst   {};
struct Reset   {};
struct Quit    {};
using Msg = std::variant<Tick, Resize, SetMode, Burst, Reset, Quit>;

// ── spawning ─────────────────────────────────────────────────────────────────

Particle& spawn(Model& m, float x, float y, float vx, float vy, float life, Colour c, float size) {
    Particle p;
    p.x = x; p.y = y; p.vx = vx; p.vy = vy; p.life = p.max_life = life; p.c = c; p.size = size;
    m.ps.push_back(p);
    return m.ps.back();
}

void burst_at(Model& m, float x, float y, int n, float speed_hi, float gravity, float life_lo, float life_hi) {
    const float hue = m.uni(0, 1);
    for (int i = 0; i < n; ++i) {
        const float a = m.uni(0, kTau), s = m.uni(1, speed_hi);
        auto& p = spawn(m, x, y, std::cos(a) * s, std::sin(a) * s, m.uni(life_lo, life_hi),
                        hsv(hue + m.uni(-0.08f, 0.08f), m.uni(0.6f, 1.f), 1.f), m.uni(0.6f, 1.2f));
        p.ay = gravity; p.spark = true;
    }
}

void spawn_mode(Model& m, int n) {
    const float cx = m.w * 0.5f, cy = m.h * 0.5f;
    for (int i = 0; i < n; ++i) switch (m.mode) {
        case Mode::Fireworks: {
            auto& p = spawn(m, m.uni(m.w * 0.1f, m.w * 0.9f), static_cast<float>(m.h - 1),
                            m.uni(-1.5f, 1.5f), m.uni(-14.f, -9.f), m.uni(0.6f, 1.f), {1.f, 0.9f, 0.7f}, 1.5f);
            p.ay = 0.15f; p.rocket = true;
            break;
        }
        case Mode::Galaxy: {
            const float arm = static_cast<float>(static_cast<int>(m.uni(0, 3))) * kTau / 3.f;
            const float d = m.uni(5.f, std::min(m.w, m.h) * 0.45f), a = arm + d * 0.04f + m.uni(-0.3f, 0.3f);
            const float x = cx + std::cos(a) * d + m.gauss(3), y = cy + std::sin(a) * d + m.gauss(3);
            const float dx = x - cx, dy = y - cy, r = std::sqrt(dx * dx + dy * dy) + 0.1f, orbit = 30.f / std::sqrt(r);
            spawn(m, x, y, -dy / r * orbit + m.gauss(0.5f), dx / r * orbit + m.gauss(0.5f), 999.f,
                  {1.f, 0.8f, 0.6f}, m.uni(0.4f, 1.f));
            break;
        }
        case Mode::Fountain: {
            auto& p = spawn(m, cx + m.gauss(3), m.h * 0.85f, m.gauss(3), m.uni(-12.f, -7.f), m.uni(1.f, 2.5f),
                            hsv(m.uni(0.5f, 0.65f), m.uni(0.5f, 0.9f), m.uni(0.7f, 1.f)), m.uni(0.5f, 1.f));
            p.ay = 0.18f;
            break;
        }
        case Mode::Vortex: {
            const bool second = i % 2 == 1;
            const float vx = m.w * (second ? 0.65f : 0.35f), a = m.uni(0, kTau), d = m.uni(2.f, std::min(m.w, m.h) * 0.25f);
            auto& p = spawn(m, vx + std::cos(a) * d, cy + std::sin(a) * d, m.gauss(1), m.gauss(1), m.uni(2.f, 5.f),
                            hsv(second ? m.uni(0.55f, 0.75f) : m.uni(0.f, 0.1f), m.uni(0.6f, 1.f), m.uni(0.7f, 1.f)),
                            m.uni(0.4f, 0.9f));
            p.second = second;
            break;
        }
        case Mode::Starfield: {
            const float a = m.uni(0, kTau), d = m.uni(1.f, 15.f), s = m.uni(2.f, 6.f), t = m.uni(0, 1);
            const Colour c = t < 0.6f ? Colour{0.9f, 0.9f, 1.f} : t < 0.8f ? Colour{1.f, 0.85f, 0.6f} : Colour{0.6f, 0.7f, 1.f};
            spawn(m, cx + std::cos(a) * d, cy + std::sin(a) * d, std::cos(a) * s, std::sin(a) * s, m.uni(2.f, 5.f),
                  c, m.uni(0.3f, 1.2f));
            break;
        }
    }
}

void reset(Model& m) {
    m.ps.clear(); m.time = 0;
    if (m.mode == Mode::Galaxy) spawn_mode(m, 2500);
    if (m.mode == Mode::Vortex) spawn_mode(m, 2000);
    if (m.mode == Mode::Starfield) spawn_mode(m, 1500);
}

// ── one step of simulated time ───────────────────────────────────────────────

void step(Model& m) {
    m.time += kDt; ++m.frame;
    switch (m.mode) {                                     // what each mode spawns per tick
        case Mode::Fireworks: if (m.frame % 15 == 0) spawn_mode(m, 1); break;
        case Mode::Galaxy:    if (m.ps.size() < 2500) spawn_mode(m, 5); break;
        case Mode::Fountain:  spawn_mode(m, 8); break;
        case Mode::Vortex:    if (m.ps.size() < 2000) spawn_mode(m, 10); break;
        case Mode::Starfield: spawn_mode(m, 10); break;
    }
    const float cx = m.w * 0.5f, cy = m.h * 0.5f;
    std::vector<std::pair<float, float>> bursts;
    const std::size_t n = m.ps.size();                    // splashes appended below aren't stepped this tick
    for (std::size_t i = 0; i < n; ++i) {
        Particle& p = m.ps[i];
        if (p.trail_n < kTrail) p.trail[static_cast<std::size_t>(p.trail_n++)] = {p.x, p.y};
        else { std::rotate(p.trail.begin(), p.trail.begin() + 1, p.trail.end()); p.trail.back() = {p.x, p.y}; }
        switch (m.mode) {
            case Mode::Galaxy: {
                const float dx = cx - p.x, dy = cy - p.y, r = std::sqrt(dx * dx + dy * dy) + 1.f;
                const float f = std::min(800.f / (r * r), 5.f);
                p.ax = dx / r * f; p.ay = dy / r * f; p.vx *= 0.998f; p.vy *= 0.998f;
                break;
            }
            case Mode::Fountain: {
                p.ax = std::sin(m.time * 0.5f) * 0.05f;
                const float ground = m.h * 0.85f;
                if (p.y >= ground && p.vy > 0) {
                    p.vy *= -0.3f; p.vx *= 0.8f; p.y = ground - 1;
                    if (std::fabs(p.vy) > 1.f && m.ps.size() < 4000)
                        for (int s = 0; s < 2; ++s) {
                            const Colour c{p.c.r * 0.8f, p.c.g * 0.8f, p.c.b * 0.8f};
                            const float x = p.x;
                            auto& sp = spawn(m, x + m.uni(-2, 2), ground - 1, m.uni(-2, 2), m.uni(-3, -1),
                                             m.uni(0.2f, 0.5f), c, 0.3f);
                            sp.ay = 0.18f; sp.spark = true;
                        }
                }
                break;
            }
            case Mode::Vortex: {
                const float ax = p.second ? m.w * 0.65f + std::cos(m.time * 0.35f) * m.w * 0.05f
                                          : m.w * 0.35f + std::sin(m.time * 0.3f)  * m.w * 0.05f;
                const float ay = p.second ? m.h * 0.5f + std::sin(m.time * 0.45f) * m.h * 0.05f
                                          : m.h * 0.5f + std::cos(m.time * 0.4f)  * m.h * 0.05f;
                const float dx = ax - p.x, dy = ay - p.y, r = std::sqrt(dx * dx + dy * dy) + 1.f, f = 200.f / (r + 10.f);
                p.ax = dx / r * f - dy / r * f * 0.8f; p.ay = dy / r * f + dx / r * f * 0.8f;
                p.vx *= 0.99f; p.vy *= 0.99f;
                break;
            }
            case Mode::Starfield: {
                const float dx = p.x - cx, dy = p.y - cy, r = std::sqrt(dx * dx + dy * dy) + 0.1f, a = 1.5f + r * 0.03f;
                p.ax = dx / r * a; p.ay = dy / r * a;
                break;
            }
            case Mode::Fireworks: break;
        }
        Particle& q = m.ps[i];                            // spawn() may have reallocated
        q.vx += q.ax * kDt; q.vy += q.ay * kDt;
        q.x += q.vx; q.y += q.vy;
        q.life -= kDt;
        if (m.mode == Mode::Fireworks && q.rocket && q.vy >= -1.f) { bursts.emplace_back(q.x, q.y); q.life = 0; }
    }
    for (auto [x, y] : bursts) burst_at(m, x, y, static_cast<int>(m.uni(40, 80)), 8.f, 0.12f, 0.5f, 1.2f);
    std::erase_if(m.ps, [&](const Particle& p) {
        return p.life <= 0 || p.x < -20 || p.x >= m.w + 20 || p.y < -20 || p.y >= m.h + 20;
    });
    if (m.ps.size() > kMaxParticles) m.ps.erase(m.ps.begin(), m.ps.begin() + static_cast<long>(m.ps.size() - kMaxParticles));
}

// ── program ──────────────────────────────────────────────────────────────────

struct Particles {
    using Model = ::Model;
    using Msg   = ::Msg;
    using Cmd   = jaal::Cmd<Msg>;
    using Sub   = jaal::Sub<Msg, on_key, on_resize>;

    static Cmd update(Model& m, Resize r) {
        m.w = std::max(4, r.cols);
        m.h = std::max(4, (r.rows - 1) * 2);
        reset(m);
        return {};
    }
    static Cmd update(Model& m, Tick)      { if (m.w) step(m); return {}; }
    static Cmd update(Model& m, SetMode s) { m.mode = s.m; reset(m); return {}; }
    static Cmd update(Model& m, Reset)     { reset(m); return {}; }
    static Cmd update(Model&, Quit)        { return Cmd::quit(0); }

    // A burst of sparks at a random spot (fireworks: a full firework burst).
    static Cmd update(Model& m, Burst) {
        const float x = m.uni(m.w * 0.15f, m.w * 0.85f), y = m.uni(m.h * 0.15f, m.h * 0.85f);
        if (m.mode == Mode::Fireworks) burst_at(m, x, y, static_cast<int>(m.uni(40, 80)), 8.f, 0.12f, 0.5f, 1.2f);
        else burst_at(m, x, y, 60, 10.f, m.mode == Mode::Fountain ? 0.15f : 0.f, 0.5f, 1.5f);
        return {};
    }

    // ── view: accumulate light, then tone-map into an image ─────────────────

    static Image render(const Model& m) {
        std::vector<Colour> light(static_cast<std::size_t>(m.w * m.h));
        auto plot = [&](float fx, float fy, Colour c, float a) {
            const int x = static_cast<int>(fx), y = static_cast<int>(fy);
            if (x < 0 || x >= m.w || y < 0 || y >= m.h) return;
            auto& l = light[static_cast<std::size_t>(y * m.w + x)];
            l.r += c.r * a; l.g += c.g * a; l.b += c.b * a;
        };
        const float maxd = std::sqrt(static_cast<float>(m.w * m.w + m.h * m.h)) * 0.5f;
        for (const auto& p : m.ps) {
            float fade = std::max(0.f, p.life / p.max_life);
            Colour c = p.c;
            if (m.mode == Mode::Galaxy) {                 // colour by orbital speed
                const float t = std::clamp(std::hypot(p.vx, p.vy) / 15.f, 0.f, 1.f);
                c = t < 0.5f ? Colour{1.f - t, t * 2, t * 0.6f}
                             : Colour{0.5f - (t - 0.5f), 1.f - (t - 0.5f), 0.3f + (t - 0.5f) * 1.4f};
            }
            if (m.mode == Mode::Starfield)                // brighter further out
                fade *= 0.3f + std::clamp(std::hypot(p.x - m.w * 0.5f, p.y - m.h * 0.5f) / maxd, 0.f, 1.f) * 0.7f;
            const float bright = fade * p.size;
            for (int t = 0; t < p.trail_n; ++t)
                plot(p.trail[static_cast<std::size_t>(t)].first, p.trail[static_cast<std::size_t>(t)].second, c,
                     static_cast<float>(t + 1) / (p.trail_n + 1) * 0.3f * fade * p.size);
            plot(p.x, p.y, c, bright);
            if (p.size > 0.8f) {                          // a glow around big particles
                for (auto [dx, dy] : {std::pair{-1, 0}, {1, 0}, {0, -1}, {0, 1}}) plot(p.x + dx, p.y + dy, c, bright * 0.3f);
                for (auto [dx, dy] : {std::pair{-1, -1}, {1, -1}, {-1, 1}, {1, 1}}) plot(p.x + dx, p.y + dy, c, bright * 0.1f);
            }
            if (m.mode == Mode::Fireworks && p.spark && fade < 0.5f) {
                const float s = (std::sin(m.time * 30.f + p.x * 7.f + p.y * 11.f) + 1.f) * 0.5f;
                if (s > 0.7f) plot(p.x, p.y, {1, 1, 1}, s * fade * 0.5f);
            }
            if (m.mode == Mode::Starfield) {              // speed streaks
                const float sp = std::hypot(p.vx, p.vy);
                if (sp > 3.f) {
                    const float len = std::min(sp * 0.4f, 8.f), nx = -p.vx / sp, ny = -p.vy / sp;
                    for (float s = 1; s < len; s += 1) plot(p.x + nx * s, p.y + ny * s, c, bright * (1 - s / len) * 0.5f);
                }
            }
        }
        // Tone-map: sqrt lifts the dim end so faint trails stay visible.
        Image img(m.w, m.h);
        auto tm = [](float v) { return static_cast<std::uint8_t>(std::sqrt(std::clamp(v, 0.f, 1.f)) * 255.f); };
        for (int y = 0; y < m.h; ++y)
            for (int x = 0; x < m.w; ++x) {
                const auto& l = light[static_cast<std::size_t>(y * m.w + x)];
                img(x, y) = {tm(l.r), tm(l.g), tm(l.b)};
            }
        return img;
    }

    static Element status_bar(const Model& m) {
        auto dim    = [](std::string s) { return text(std::move(s)) | fgc(Color::rgb(120, 120, 120)); };
        auto accent = [](std::string s) { return text(std::move(s)) | fgc(Color::rgb(80, 200, 255)) | Bold; };
        return h(text(" "), accent("PARTICLES"), dim(" │ "), accent(kModeName[static_cast<std::size_t>(m.mode)]),
                 dim(" │ " + std::to_string(m.ps.size()) + " particles │ [1-5] mode [spc] burst [r] reset [q] quit"),
                 spacer()) | bgc(Color::rgb(18, 18, 24));
    }

    static Element view(const Model& m) {
        if (m.w == 0) return text("");
        return v(pixels(render(m)), status_bar(m));
    }

    static Sub subscribe(const Model&) {
        return Sub::batch(
            Sub::every(16ms, Tick{}),
            Sub::on(on_resize{}, [](const ResizeEvent& r) -> std::optional<Msg> {
                return Resize{r.width.value, r.height.value};
            }),
            keys<Sub>({
                {'1', SetMode{Mode::Fireworks}}, {'2', SetMode{Mode::Galaxy}}, {'3', SetMode{Mode::Fountain}},
                {'4', SetMode{Mode::Vortex}}, {'5', SetMode{Mode::Starfield}},
                {' ', Burst{}}, {'r', Reset{}}, {'q', Quit{}}, {SpecialKey::Escape, Quit{}},
            }));
    }
    static bool subs_key(const Model&) { return true; }
};

static_assert(Program<Particles>);

}  // namespace

int main() { return run<Particles>({.title = "particles"}); }
