// examples/jaal_doom_fire.cpp — the Doom PSX fire, as a jaal program.
//
//   Model      the heat field (twice the rows, for half blocks), the embers
//              floating out of it, the source switch, wind, intensity,
//              palette and the RNG.
//   update()   Tick propagates heat upward (each pixel copies a jittered,
//              wind-blown neighbour from below, minus a random decay),
//              spawns embers from hot spots and moves them; keys toggle the
//              source, blow the wind, change the heat and the palette.
//   view()     the field through the palette into an Image, embers drawn
//              over it as whole bright cells, and a status bar.
//
// Keys: space source   left/right wind   +/- heat   1-3 palette   q quit

#include <maya/element/pixels.hpp>
#include <maya/jaal/host.hpp>
#include <maya/maya.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <variant>
#include <vector>

using namespace maya;
using namespace maya::dsl;
using namespace std::chrono_literals;

namespace {

constexpr int kMaxHeat = 48;

// ── palettes: heat 0..kMaxHeat -> colour, piecewise-linear ramps ────────────

struct Stop { float at; float r, g, b; };
template <std::size_t N> using Ramp = std::array<Stop, N>;

constexpr Ramp<5> kClassic = {{{0, 0, 0, 0}, {.15f, 180, 0, 0}, {.4f, 255, 100, 0}, {.7f, 255, 255, 0}, {1, 255, 255, 255}}};
constexpr Ramp<5> kInferno = {{{0, 0, 0, 0}, {.2f, 60, 0, 100}, {.45f, 220, 0, 160}, {.7f, 255, 140, 0}, {1, 255, 255, 200}}};
constexpr Ramp<5> kToxic   = {{{0, 0, 0, 0}, {.2f, 0, 60, 0}, {.5f, 0, 255, 30}, {.75f, 200, 255, 0}, {1, 255, 255, 255}}};
constexpr std::array<const char*, 3> kPaletteName = {"CLASSIC", "INFERNO", "TOXIC"};

template <std::size_t N>
Rgb sample(const Ramp<N>& ramp, float t) {
    std::size_t i = 1;
    while (i + 1 < N && t > ramp[i].at) ++i;
    const Stop &a = ramp[i - 1], &b = ramp[i];
    const float u = std::clamp((t - a.at) / (b.at - a.at), 0.f, 1.f);
    auto mix = [u](float x, float y) { return static_cast<std::uint8_t>(x + (y - x) * u); };
    return {mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b)};
}

// All three palettes, precomputed per heat level: the view is a table lookup.
using Lut = std::array<std::array<Rgb, kMaxHeat + 1>, 3>;
const Lut kLut = [] {
    Lut l{};
    for (int h = 0; h <= kMaxHeat; ++h) {
        const float t = static_cast<float>(h) / kMaxHeat;
        l[0][h] = sample(kClassic, t);
        l[1][h] = sample(kInferno, t);
        l[2][h] = sample(kToxic, t);
    }
    return l;
}();

// ── model ────────────────────────────────────────────────────────────────────

struct Ember { float x, y, vx, vy; int heat; float life; };   // x, y in pixels

struct Model {
    int w = 0, h = 0;                      // the heat field, in pixels (h = 2 * terminal rows)
    std::vector<std::uint8_t> heat;
    std::vector<Ember> embers;
    bool source = true;
    int wind = 0;                          // -5..5
    int intensity = 3;                     // 1..5, higher = taller flames
    int palette = 0;
    std::mt19937 rng{42};

    std::uint8_t& at(int x, int y) { return heat[static_cast<std::size_t>(y * w + x)]; }
    [[nodiscard]] int at(int x, int y) const { return heat[static_cast<std::size_t>(y * w + x)]; }

    void set_source_row() {
        for (int x = 0; x < w; ++x) at(x, h - 1) = source ? kMaxHeat : 0;
    }
};

// ── messages ─────────────────────────────────────────────────────────────────

struct Tick {};
struct Resize       { int cols, rows; };
struct ToggleSource {};
struct Wind         { int by; };
struct Heat         { int by; };
struct SetPalette   { int p; };
struct Quit         {};
using Msg = std::variant<Tick, Resize, ToggleSource, Wind, Heat, SetPalette, Quit>;

// ── program ──────────────────────────────────────────────────────────────────

struct DoomFire {
    using Model = ::Model;
    using Msg   = ::Msg;
    using Cmd   = jaal::Cmd<Msg>;
    using Sub   = jaal::Sub<Msg, on_key, on_resize>;

    static Cmd update(Model& m, Resize r) {
        m.w = std::max(1, r.cols);
        m.h = std::max(2, r.rows * 2);
        m.heat.assign(static_cast<std::size_t>(m.w * m.h), 0);
        m.embers.clear();
        m.set_source_row();
        return {};
    }
    static Cmd update(Model& m, ToggleSource) { m.source = !m.source; if (m.w) m.set_source_row(); return {}; }
    static Cmd update(Model& m, Wind d)       { m.wind = std::clamp(m.wind + d.by, -5, 5); return {}; }
    static Cmd update(Model& m, Heat d)       { m.intensity = std::clamp(m.intensity + d.by, 1, 5); return {}; }
    static Cmd update(Model& m, SetPalette s) { m.palette = s.p; return {}; }
    static Cmd update(Model&, Quit)           { return Cmd::quit(0); }

    static Cmd update(Model& m, Tick) {
        if (m.w == 0) return {};
        propagate(m);
        spawn_ember(m);
        for (auto& e : m.embers) {
            e.x += e.vx; e.y += e.vy;
            e.vy -= 0.006f;                                   // buoyancy
            e.life -= 0.02f;
            e.heat = static_cast<int>(static_cast<float>(e.heat) * 0.97f);
        }
        std::erase_if(m.embers, [](const Ember& e) { return e.life <= 0.f || e.heat <= 1; });
        return {};
    }

    // Each pixel takes the heat of a jittered, wind-blown pixel below it,
    // minus a random decay. Higher intensity = smaller decay range.
    static void propagate(Model& m) {
        std::uniform_int_distribution<int> decay(0, 6 - m.intensity), gust(0, std::abs(m.wind)), spread(-1, 1);
        for (int y = 0; y < m.h - 1; ++y)
            for (int x = 0; x < m.w; ++x) {
                const int blow = m.wind == 0 ? 0 : (m.wind > 0 ? gust(m.rng) : -gust(m.rng));
                const int sx = std::clamp(x + blow + spread(m.rng), 0, m.w - 1);
                m.at(x, y) = static_cast<std::uint8_t>(std::max(0, m.at(sx, y + 1) - decay(m.rng)));
            }
        m.set_source_row();
    }

    static void spawn_ember(Model& m) {
        if (!m.source || m.rng() % 3 != 0 || m.h < 3) return;
        const int x = static_cast<int>(m.rng() % static_cast<unsigned>(m.w));
        const int y = m.h / 3 + static_cast<int>(m.rng() % static_cast<unsigned>(m.h / 3));
        const int heat = m.at(x, y);
        if (heat <= kMaxHeat / 2) return;
        const float vx = (static_cast<float>(m.rng() % 100) - 50.f) / 200.f + static_cast<float>(m.wind) * 0.05f;
        const float vy = -(static_cast<float>(m.rng() % 100) + 30.f) / 100.f;
        m.embers.push_back({static_cast<float>(x), static_cast<float>(y), vx, vy, heat, 1.f});
    }

    // ── view ────────────────────────────────────────────────────────────────

    static Image render(const Model& m) {
        const int shown = m.h - 2;                            // the bottom row of cells is the status bar
        const auto& lut = kLut[static_cast<std::size_t>(m.palette)];
        Image img(m.w, shown);
        for (int y = 0; y < shown; ++y)
            for (int x = 0; x < m.w; ++x) img(x, y) = lut[static_cast<std::size_t>(m.at(x, y))];
        for (const auto& e : m.embers) {                     // a whole bright cell
            const int x = static_cast<int>(e.x), top = static_cast<int>(e.y) & ~1;
            if (x < 0 || x >= m.w || top < 0 || top + 1 >= shown) continue;
            const Rgb c = lut[static_cast<std::size_t>(std::clamp(e.heat, 0, kMaxHeat))];
            img(x, top) = img(x, top + 1) = c;
        }
        return img;
    }

    static Element status_bar(const Model& m) {
        const auto bg = Color::rgb(20, 20, 20);
        auto accent = [](std::string s) { return text(std::move(s)) | fgc(Color::rgb(255, 140, 40)) | Bold; };
        char stats[64];
        std::snprintf(stats, sizeof stats, " h:%d w:%+d e:%d ", m.intensity, m.wind, static_cast<int>(m.embers.size()));
        return h(text(" "), accent("DOOM FIRE"),
                 text(" │ [1-3] palette │ [+/-] heat │ [←→] wind │ [space] toggle │ [q] quit") | fgc(Color::rgb(100, 100, 110)),
                 spacer(), accent(std::string(kPaletteName[static_cast<std::size_t>(m.palette)]) + stats)) | bgc(bg);
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
            jaal_key_map<Sub>({
                {' ', ToggleSource{}},
                {SpecialKey::Left, Wind{-1}}, {SpecialKey::Right, Wind{1}},
                {'+', Heat{1}}, {'=', Heat{1}}, {'-', Heat{-1}},
                {'1', SetPalette{0}}, {'2', SetPalette{1}}, {'3', SetPalette{2}},
                {'q', Quit{}}, {SpecialKey::Escape, Quit{}},
            }));
    }
    static bool subs_key(const Model&) { return true; }
};

static_assert(JaalView<DoomFire>);

}  // namespace

int main() { return run_jaal<DoomFire>({.title = "doom fire"}); }
