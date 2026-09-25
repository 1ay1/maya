// examples/jaal_life.cpp — Conway's Game of Life, as a jaal program.
//
// A toroidal (wrap-around) grid drawn two cells per terminal row with half
// blocks. Live cells are coloured by age: newborns bright green, then cyan,
// blue, purple, and a dim purple for long-lived still lifes.
//
//   Model      the grid, per-cell ages, generation counter, speed, the RNG.
//   update()   Tick advances the clock and steps generations; keys change
//              speed, pause, or load a pattern.
//   view()     the grid as an Image (pixels()) plus a one-row status bar.
//   subscribe  a 30 Hz clock, the keyboard, and resizes.
//
// Keys: q/Esc quit   space pause   Enter single step   +/- speed   c clear
//       r random   g glider gun   p pulsar   s spaceship fleet

#include <maya/jaal/host.hpp>
#include <maya/element/pixels.hpp>
#include <maya/maya.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace maya;
using namespace maya::dsl;
using namespace std::chrono_literals;

namespace {

constexpr int kFrameMs = 33;   // 30 fps, as the original

// ── Colours ──────────────────────────────────────────────────────────────────

constexpr Rgb kDead{0, 0, 0};
constexpr std::array<Rgb, 5> kAgeColors = {{
    {100, 255, 120},   // age 1       — bright green
    {  0, 220, 220},   // age 2-5     — cyan
    { 40, 120, 255},   // age 6-20    — blue
    {140,  60, 220},   // age 21-80   — purple
    { 80,  40, 120},   // age 80+     — dim purple
}};

constexpr int age_bucket(std::uint16_t a) {
    if (a <= 1)  return 0;
    if (a <= 5)  return 1;
    if (a <= 20) return 2;
    if (a <= 80) return 3;
    return 4;
}

// ── Patterns ─────────────────────────────────────────────────────────────────

enum class Pattern { Random, GliderGun, Pulsar, Spaceships, Empty };

constexpr const char* pattern_name(Pattern p) {
    switch (p) {
        case Pattern::Random:     return "random";
        case Pattern::GliderGun:  return "glider gun";
        case Pattern::Pulsar:     return "pulsar";
        case Pattern::Spaceships: return "spaceships";
        case Pattern::Empty:      return "empty";
    }
    return "";
}

using Cell = std::pair<int, int>;

constexpr std::array<Cell, 36> kGliderGun = {{
    {1,5},{1,6},{2,5},{2,6},
    {11,5},{11,6},{11,7},{12,4},{12,8},{13,3},{13,9},{14,3},{14,9},
    {15,6},{16,4},{16,8},{17,5},{17,6},{17,7},{18,6},
    {21,3},{21,4},{21,5},{22,3},{22,4},{22,5},{23,2},{23,6},
    {25,1},{25,2},{25,6},{25,7},
    {35,3},{35,4},{36,3},{36,4},
}};

constexpr std::array<Cell, 48> kPulsar = {{   // period-3 oscillator
    {2,0},{3,0},{4,0},{8,0},{9,0},{10,0},
    {0,2},{5,2},{7,2},{12,2},
    {0,3},{5,3},{7,3},{12,3},
    {0,4},{5,4},{7,4},{12,4},
    {2,5},{3,5},{4,5},{8,5},{9,5},{10,5},
    {2,7},{3,7},{4,7},{8,7},{9,7},{10,7},
    {0,8},{5,8},{7,8},{12,8},
    {0,9},{5,9},{7,9},{12,9},
    {0,10},{5,10},{7,10},{12,10},
    {2,12},{3,12},{4,12},{8,12},{9,12},{10,12},
}};

constexpr std::array<Cell, 9> kLwss = {{      // lightweight spaceship
    {1,0},{4,0},{0,1},{0,2},{4,2},{0,3},{1,3},{2,3},{3,3},
}};

// ── The model ────────────────────────────────────────────────────────────────

struct Model {
    // The grid in pixels (two per terminal row), sized by Resize.
    int w = 0, h = 0;
    std::vector<std::uint8_t>  cur, nxt;   // current / next generation (0/1)
    std::vector<std::uint16_t> age;        // generations each cell has lived

    int     generation  = 0;
    int     population  = 0;
    Pattern pattern     = Pattern::Random;
    bool    paused      = false;
    int     interval_ms = 100;   // time between generations: 10..1000
    int     accum_ms    = 0;     // clock time banked towards the next step

    std::mt19937 rng{42};

    std::size_t idx(int x, int y) const { return static_cast<std::size_t>(y * w + x); }
};

// ── Messages ─────────────────────────────────────────────────────────────────

struct Tick {};
struct Resize { int cols, rows; };
struct Pause  {};
struct Step   {};
struct Speed  { int d; };   // +1 faster, -1 slower
struct Load   { Pattern p; };
struct Quit   {};
using Msg = std::variant<Tick, Resize, Pause, Step, Speed, Load, Quit>;

// ── The simulation: functions of the model ───────────────────────────────────

int neighbors(const Model& m, int x, int y) {
    int n = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        const int yy = (y + dy + m.h) % m.h;          // toroidal wrap
        for (int dx = -1; dx <= 1; ++dx) {
            if (!(dx | dy)) continue;
            const int xx = (x + dx + m.w) % m.w;
            n += m.cur[m.idx(xx, yy)];
        }
    }
    return n;
}

void step_generation(Model& m) {
    m.population = 0;
    for (int y = 0; y < m.h; ++y) {
        for (int x = 0; x < m.w; ++x) {
            const auto i = m.idx(x, y);
            const int  n = neighbors(m, x, y);
            const bool live = m.cur[i] != 0;
            const bool next = live ? (n == 2 || n == 3) : (n == 3);
            m.nxt[i] = next;
            if (next) {
                m.age[i] = live ? std::min<std::uint16_t>(m.age[i] + 1, 9999) : 1;
                ++m.population;
            } else {
                m.age[i] = 0;
            }
        }
    }
    std::swap(m.cur, m.nxt);
    ++m.generation;
}

void clear_grid(Model& m) {
    std::fill(m.cur.begin(), m.cur.end(), 0);
    std::fill(m.age.begin(), m.age.end(), 0);
    m.generation = 0;
    m.population = 0;
}

template <std::size_t N>
void stamp(Model& m, const std::array<Cell, N>& cells, int ox, int oy) {
    for (auto [dx, dy] : cells) {
        const int x = ox + dx, y = oy + dy;
        if (x >= 0 && x < m.w && y >= 0 && y < m.h) {
            m.cur[m.idx(x, y)] = 1;
            m.age[m.idx(x, y)] = 1;
        }
    }
}

void load(Model& m, Pattern p) {
    clear_grid(m);
    m.pattern = p;
    switch (p) {
        case Pattern::Random: {
            std::uniform_int_distribution<int> dist(0, 3);
            for (std::size_t i = 0; i < m.cur.size(); ++i) {
                m.cur[i] = dist(m.rng) == 0;
                m.age[i] = m.cur[i];
            }
            break;
        }
        case Pattern::GliderGun:   // one gun per 40 pixel rows
            for (int i = 0; i < std::max(1, m.h / 40); ++i)
                stamp(m, kGliderGun, 4, 4 + i * 30);
            break;
        case Pattern::Pulsar:      // tiled across the grid
            for (int py = 0; py * 16 < m.h; ++py)
                for (int px = 0; px * 16 < m.w; ++px)
                    stamp(m, kPulsar, 1 + px * 16, 1 + py * 16);
            break;
        case Pattern::Spaceships: {
            const int cx = m.w / 2 - 10;
            for (int i = 0; i < 3; ++i) {
                stamp(m, kLwss, cx,      m.h / 4 + i * 8);
                stamp(m, kLwss, cx + 20, m.h / 4 + i * 8 + 4);
            }
            break;
        }
        case Pattern::Empty:
            break;
    }
    m.population = static_cast<int>(std::count(m.cur.begin(), m.cur.end(), 1));
}

// ── The program ──────────────────────────────────────────────────────────────

struct Life {
    using Model = ::Model;
    using Msg   = ::Msg;
    using Cmd   = jaal::Cmd<Msg>;
    using Sub   = jaal::Sub<Msg, on_key, on_resize>;

    static Cmd update(Model& m, Resize r) {
        // One status row; the rest is grid, two cells per row. A new size
        // starts a fresh random soup.
        m.w = std::max(1, r.cols);
        m.h = std::max(2, (r.rows - 1) * 2);
        const auto n = static_cast<std::size_t>(m.w * m.h);
        m.cur.assign(n, 0);
        m.nxt.assign(n, 0);
        m.age.assign(n, 0);
        load(m, Pattern::Random);
        return {};
    }
    static Cmd update(Model& m, Tick) {
        if (m.paused || m.cur.empty()) return {};
        // Bank frame time; run as many generations as fit (at most 8 a frame).
        m.accum_ms += kFrameMs;
        const int steps = m.accum_ms / m.interval_ms;
        if (steps > 0) {
            for (int i = 0; i < std::min(steps, 8); ++i) step_generation(m);
            m.accum_ms = 0;
        }
        return {};
    }
    static Cmd update(Model& m, Step) {
        if (!m.cur.empty()) step_generation(m);
        return {};
    }
    static Cmd update(Model& m, Pause)   { m.paused = !m.paused; return {}; }
    static Cmd update(Model& m, Speed s) {
        m.interval_ms = std::clamp(m.interval_ms - s.d * 20, 10, 1000);
        return {};
    }
    static Cmd update(Model& m, Load l) { load(m, l.p); return {}; }
    static Cmd update(Model&, Quit)     { return Cmd::quit(0); }

    // ── view ────────────────────────────────────────────────────────────────

    static Element view(const Model& m) {
        Image img(m.w, m.h, kDead);
        for (int y = 0; y < m.h; ++y)
            for (int x = 0; x < m.w; ++x) {
                const auto i = m.idx(x, y);
                if (m.cur[i]) img(x, y) = kAgeColors[static_cast<std::size_t>(age_bucket(m.age[i]))];
            }
        return v(pixels(std::move(img)), status_bar(m));
    }

    static Element status_bar(const Model& m) {
        const Color bar = Color::rgb(20, 20, 30);
        const int speed_pct = 10000 / m.interval_ms;
        std::string info = " Gen " + std::to_string(m.generation) +
                           "  Pop " + std::to_string(m.population) +
                           "  Speed " + std::to_string(speed_pct) + "%" +
                           "  [" + pattern_name(m.pattern) + "]" +
                           (m.paused ? "  PAUSED " : " ");
        return h(text(std::move(info)) | fgc(Color::rgb(100, 255, 180)) | Bold,
                 spacer(),
                 text(" r/g/p/s=pattern  spc=pause  +/-=speed  c=clear  q=quit ")
                     | fgc(Color::rgb(80, 80, 100))) | bgc(bar);
    }

    // ── subscribe ───────────────────────────────────────────────────────────

    static Sub subscribe(const Model&) {
        return Sub::batch(
            Sub::every(std::chrono::milliseconds(kFrameMs), Tick{}),
            Sub::on(on_resize{}, [](const ResizeEvent& r) -> std::optional<Msg> {
                return Resize{r.width.value, r.height.value};
            }),
            jaal_key_map<Sub>({
                {'q', Quit{}}, {SpecialKey::Escape, Quit{}},
                {' ', Pause{}}, {SpecialKey::Enter, Step{}},
                {'+', Speed{+1}}, {'=', Speed{+1}}, {'-', Speed{-1}},
                {'c', Load{Pattern::Empty}},
                {'r', Load{Pattern::Random}},
                {'g', Load{Pattern::GliderGun}},
                {'p', Load{Pattern::Pulsar}},
                {'s', Load{Pattern::Spaceships}},
            }));
    }
    static bool subs_key(const Model&) { return true; }
};

static_assert(JaalView<Life>);

}  // namespace

int main() { return run_jaal<Life>({.title = "life"}); }
