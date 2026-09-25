// examples/doom_fire.cpp — the PSX Doom fire, as a jaal program.
//
// The reference for how maya apps are written now. Everything the program
// is lives in four places, and each has one job:
//
//   Model      all the state: the heat field, embers, controls, the RNG.
//              A value: it can be copied, compared, replayed.
//   update()   one function per message. Tick advances the simulation;
//              keys change controls. Nothing else mutates anything.
//   view()     the model as an Element: the fire as an Image (pixels()),
//              the status bar as text. Pure: same model, same screen.
//   subscribe  where messages come from: a 60 Hz clock and the keyboard.
//
// No globals, no paint callback, no style ids, no quit flag, no loop. The
// runtime (jaal) owns time, input and exit; maya owns the terminal.
//
// Keys: q/Esc quit   space toggle source   ←/→ wind   +/- heat   1-3 palette

#include <maya/jaal/host.hpp>
#include <maya/element/pixels.hpp>
#include <maya/maya.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <variant>
#include <vector>

using namespace maya;
using namespace maya::dsl;
using namespace std::chrono_literals;

namespace {

constexpr int kMaxHeat = 48;

// ── Palettes: heat -> colour, precomputed once ───────────────────────────────

struct Palette {
    const char* name;
    std::array<Rgb, kMaxHeat + 1> ramp;
};

// Piecewise-linear ramp through `stops` (heat fraction, colour).
template <std::size_t N>
constexpr Palette make_palette(const char* name, std::array<std::pair<float, Rgb>, N> stops) {
    Palette p{name, {}};
    for (int h = 0; h <= kMaxHeat; ++h) {
        const float t = static_cast<float>(h) / kMaxHeat;
        std::size_t i = 0;
        while (i + 2 < N && t > stops[i + 1].first) ++i;
        const auto [t0, c0] = stops[i];
        const auto [t1, c1] = stops[i + 1];
        const float u = t1 > t0 ? std::clamp((t - t0) / (t1 - t0), 0.f, 1.f) : 0.f;
        auto mix = [u](std::uint8_t a, std::uint8_t b) {
            return static_cast<std::uint8_t>(a + (b - a) * u);
        };
        p.ramp[static_cast<std::size_t>(h)] = {mix(c0.r, c1.r), mix(c0.g, c1.g), mix(c0.b, c1.b)};
    }
    return p;
}

const std::array<Palette, 3> kPalettes = {
    make_palette<5>("CLASSIC", {{{0.00f, {0, 0, 0}},     {0.15f, {180, 0, 0}},  {0.40f, {255, 100, 0}},
                                 {0.70f, {255, 255, 0}}, {1.00f, {255, 255, 255}}}}),
    make_palette<5>("INFERNO", {{{0.00f, {0, 0, 0}},     {0.20f, {60, 0, 100}}, {0.45f, {220, 0, 160}},
                                 {0.70f, {255, 140, 0}}, {1.00f, {255, 255, 200}}}}),
    make_palette<5>("TOXIC",   {{{0.00f, {0, 0, 0}},     {0.20f, {0, 60, 0}},   {0.50f, {0, 255, 30}},
                                 {0.75f, {200, 255, 0}}, {1.00f, {255, 255, 255}}}}),
};

// ── The model ────────────────────────────────────────────────────────────────

struct Ember { float x, y, vx, vy, life; int heat; };

struct Model {
    // The heat field, in pixels (two per terminal row). Sized to the screen
    // by Resize; a Tick before the first Resize does nothing.
    int w = 0, h = 0;
    std::vector<std::uint8_t> heat;
    std::vector<Ember>        embers;

    bool source    = true;
    int  wind      = 0;       // -3..3
    int  intensity = 3;       // 1..5: higher = taller flames
    int  palette   = 0;

    std::mt19937 rng{42};     // in the model: a replay reproduces the fire exactly

    std::uint8_t& at(int x, int y) { return heat[static_cast<std::size_t>(y * w + x)]; }
    std::uint8_t  at(int x, int y) const { return heat[static_cast<std::size_t>(y * w + x)]; }
};

// ── Messages ─────────────────────────────────────────────────────────────────

struct Tick {};
struct Resize  { int cols, rows; };
struct Toggle  {};
struct Wind    { int d; };
struct Heat    { int d; };
struct UsePalette { int i; };
struct Quit    {};
using Msg = std::variant<Tick, Resize, Toggle, Wind, Heat, UsePalette, Quit>;

// ── The simulation: pure functions of the model ──────────────────────────────

void set_source_row(Model& m) {
    for (int x = 0; x < m.w; ++x) m.at(x, m.h - 1) = m.source ? kMaxHeat : 0;
}

// One step of the classic propagation: every pixel takes the heat of a
// neighbour from the row below, jittered sideways (and blown by the wind),
// minus a random decay. Higher intensity = smaller decay = taller flames.
void propagate(Model& m) {
    std::uniform_int_distribution<int> decay(0, 6 - m.intensity);
    std::uniform_int_distribution<int> spread(-1, 1);
    std::uniform_int_distribution<int> gust(0, std::abs(m.wind));
    for (int y = 0; y < m.h - 1; ++y)
        for (int x = 0; x < m.w; ++x) {
            const int drift = m.wind == 0 ? 0 : (m.wind > 0 ? gust(m.rng) : -gust(m.rng));
            const int sx = std::clamp(x + drift + spread(m.rng), 0, m.w - 1);
            m.at(x, y) = static_cast<std::uint8_t>(std::max(0, m.at(sx, y + 1) - decay(m.rng)));
        }
}

// Sparks: born in the hot middle of the fire, float up, cool and fade.
void step_embers(Model& m) {
    if (m.h < 6 || m.embers.size() > 256) {
    } else if (std::uniform_int_distribution<int>(0, 3)(m.rng) == 0) {
        const int x = std::uniform_int_distribution<int>(0, m.w - 1)(m.rng);
        const int y = m.h / 3 + std::uniform_int_distribution<int>(0, m.h / 3)(m.rng);
        if (const int heat = m.at(x, y); heat > kMaxHeat / 2) {
            std::uniform_real_distribution<float> jitter(-0.25f, 0.25f), rise(0.15f, 0.65f);
            m.embers.push_back({static_cast<float>(x), static_cast<float>(y),
                                jitter(m.rng) + m.wind * 0.05f, -rise(m.rng), 1.0f, heat});
        }
    }
    for (auto& e : m.embers) {
        e.x += e.vx; e.y += e.vy; e.vy -= 0.003f;
        e.life -= 0.02f;
        e.heat = static_cast<int>(e.heat * 0.97f);
    }
    std::erase_if(m.embers, [&](const Ember& e) {
        return e.life <= 0.f || e.heat <= 1 || e.y < 0 || e.x < 0 || e.x >= m.w;
    });
}

// ── update: one function per message ─────────────────────────────────────────

struct Fire {
    using Model = ::Model;
    using Msg   = ::Msg;
    using Cmd   = jaal::Cmd<Msg>;
    using Sub   = jaal::Sub<Msg, on_key, on_resize>;

    static Cmd update(Model& m, Resize r) {
        // One status row; the rest is fire, two pixels per row.
        m.w = std::max(1, r.cols);
        m.h = std::max(2, (r.rows - 1) * 2);
        m.heat.assign(static_cast<std::size_t>(m.w * m.h), 0);
        m.embers.clear();
        set_source_row(m);
        return {};
    }
    static Cmd update(Model& m, Tick) {
        if (m.heat.empty()) return {};
        propagate(m);
        set_source_row(m);
        step_embers(m);
        return {};
    }
    static Cmd update(Model& m, Toggle)       { m.source = !m.source; if (!m.heat.empty()) set_source_row(m); return {}; }
    static Cmd update(Model& m, Wind d)       { m.wind = std::clamp(m.wind + d.d, -3, 3); return {}; }
    static Cmd update(Model& m, Heat d)       { m.intensity = std::clamp(m.intensity + d.d, 1, 5); return {}; }
    static Cmd update(Model& m, UsePalette p) { m.palette = p.i; return {}; }
    static Cmd update(Model&, Quit)           { return Cmd::quit(0); }

    // ── view: the model as an Element ───────────────────────────────────────

    static Element view(const Model& m) {
        const auto& ramp = kPalettes[static_cast<std::size_t>(m.palette)].ramp;
        Image img(m.w, m.h);
        for (int y = 0; y < m.h; ++y)
            for (int x = 0; x < m.w; ++x)
                img(x, y) = ramp[m.at(x, y)];
        for (const auto& e : m.embers) {
            const int x = static_cast<int>(e.x), y = static_cast<int>(e.y);
            if (x >= 0 && x < m.w && y >= 0 && y < m.h)
                img(x, y) = ramp[static_cast<std::size_t>(std::clamp(e.heat, 0, kMaxHeat))];
        }
        return v(pixels(std::move(img)), status_bar(m));
    }

    static Element status_bar(const Model& m) {
        const Color bar = Color::rgb(20, 20, 20);
        auto key   = [&](std::string s) { return text(std::move(s)) | fgc(Color::rgb(255, 140, 40)) | Bold; };
        auto label = [&](std::string s) { return text(std::move(s)) | fgc(Color::rgb(120, 120, 120)); };
        return h(key(" DOOM FIRE "), label("│ "),
                 key("space"), label(m.source ? " on " : " off "),
                 key("←→"), label(" wind " + std::to_string(m.wind) + " "),
                 key("+/-"), label(" heat " + std::to_string(m.intensity) + " "),
                 key("1-3"), label(std::string(" ") + kPalettes[static_cast<std::size_t>(m.palette)].name + " "),
                 key("q"), label(" quit"),
                 spacer(),
                 label(std::to_string(m.embers.size()) + " embers ")) | bgc(bar);
    }

    // ── subscribe: where messages come from ─────────────────────────────────

    static Sub subscribe(const Model&) {
        return Sub::batch(
            Sub::every(16ms, Tick{}),
            Sub::on(on_resize{}, [](const ResizeEvent& r) -> std::optional<Msg> {
                return Resize{r.width.value, r.height.value};
            }),
            jaal_key_map<Sub>({
                {'q', Quit{}}, {SpecialKey::Escape, Quit{}},
                {' ', Toggle{}},
                {SpecialKey::Left, Wind{-1}}, {SpecialKey::Right, Wind{+1}},
                {'+', Heat{+1}}, {'=', Heat{+1}}, {'-', Heat{-1}},
                {'1', UsePalette{0}}, {'2', UsePalette{1}}, {'3', UsePalette{2}},
            }));
    }
    static bool subs_key(const Model&) { return true; }   // the same sources, always
};

static_assert(JaalView<Fire>);

}  // namespace

int main() { return run_jaal<Fire>({.title = "doom fire"}); }
