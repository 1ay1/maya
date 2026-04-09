// maya — Conway's Game of Life
//
// Beautiful half-block rendering with heat-gradient aging. Each terminal cell
// shows two vertical pixels using ▀/▄ characters, doubling vertical resolution.
// Toroidal grid (wraps around). Pre-built patterns and smooth color palette.
//
// Keys: q/Esc=quit  space=pause  Enter=step  +/-=speed  c=clear
//       r=random  g=glider gun  p=pulsar  s=spaceship fleet

#include <maya/internal.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace maya;

// ── State ────────────────────────────────────────────────────────────────────

static std::mt19937 g_rng{42};
static int g_pw = 0, g_ph = 0;           // pixel grid dimensions
static std::vector<bool> g_cur, g_nxt;    // current / next generation
static std::vector<uint16_t> g_age;       // how long each cell has been alive
static int g_generation = 0;
static int g_population = 0;
static bool g_paused = false;
static bool g_step = false;               // single-step request
static float g_interval = 0.1f;           // seconds between generations
static float g_accum = 0.f;
static const char* g_pattern_name = "random";

// ── Style IDs ────────────────────────────────────────────────────────────────

static uint16_t S_DEFAULT;
// Age gradient: 0=dead, indexed 1..5 for age buckets
static constexpr int NUM_AGE_STYLES = 5;
static uint16_t S_AGE_FG[NUM_AGE_STYLES];  // foreground-only (top alive)
static uint16_t S_AGE_BG[NUM_AGE_STYLES];  // background-only (bottom alive)
// For "both alive" we need fg=top, bg=bottom — too many combos to pre-intern,
// so we use a small cache indexed by (top_bucket * NUM_AGE_STYLES + bot_bucket).
static uint16_t S_BOTH[NUM_AGE_STYLES * NUM_AGE_STYLES];
static uint16_t S_BAR;
static uint16_t S_BAR_DIM;
static uint16_t S_BAR_ACC;
static uint16_t S_BAR_KEY;

struct AgeColor { uint8_t r, g, b; };

static constexpr AgeColor g_gradient[NUM_AGE_STYLES] = {
    {100, 255, 120},  // age 1       — bright green
    {  0, 220, 220},  // age 2-5     — cyan
    { 40, 120, 255},  // age 6-20    — blue
    {140,  60, 220},  // age 21-80   — purple
    { 80,  40, 120},  // age 80+     — dim purple
};

static int age_bucket(uint16_t a) {
    if (a <= 1)  return 0;
    if (a <= 5)  return 1;
    if (a <= 20) return 2;
    if (a <= 80) return 3;
    return 4;
}

// ── Grid helpers ─────────────────────────────────────────────────────────────

static inline int idx(int x, int y) {
    // toroidal wrap
    x = ((x % g_pw) + g_pw) % g_pw;
    y = ((y % g_ph) + g_ph) % g_ph;
    return y * g_pw + x;
}

static inline bool alive(int x, int y) { return g_cur[idx(x, y)]; }

static int neighbors(int x, int y) {
    int n = 0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            if ((dx | dy) && alive(x + dx, y + dy)) ++n;
    return n;
}

static void step_generation() {
    g_population = 0;
    for (int y = 0; y < g_ph; ++y) {
        for (int x = 0; x < g_pw; ++x) {
            int n = neighbors(x, y);
            int i = y * g_pw + x;
            bool live = g_cur[i];
            bool next = live ? (n == 2 || n == 3) : (n == 3);
            g_nxt[i] = next;
            if (next) {
                g_age[i] = live ? std::min<uint16_t>(g_age[i] + 1, 9999) : 1;
                ++g_population;
            } else {
                g_age[i] = 0;
            }
        }
    }
    std::swap(g_cur, g_nxt);
    ++g_generation;
}

// ── Pattern loading ──────────────────────────────────────────────────────────

static void clear_grid() {
    std::fill(g_cur.begin(), g_cur.end(), false);
    std::fill(g_age.begin(), g_age.end(), uint16_t(0));
    g_generation = 0;
    g_population = 0;
}

static void set_cell(int x, int y) {
    if (x >= 0 && x < g_pw && y >= 0 && y < g_ph) {
        int i = y * g_pw + x;
        g_cur[i] = true;
        g_age[i] = 1;
    }
}

static void stamp(const std::vector<std::pair<int,int>>& cells, int ox, int oy) {
    for (auto [dx, dy] : cells) set_cell(ox + dx, oy + dy);
}

static void load_random() {
    clear_grid();
    g_pattern_name = "random";
    std::uniform_int_distribution<int> dist(0, 3);
    for (int i = 0; i < g_pw * g_ph; ++i) {
        g_cur[i] = (dist(g_rng) == 0);
        g_age[i] = g_cur[i] ? 1 : 0;
    }
    g_population = static_cast<int>(std::count(g_cur.begin(), g_cur.end(), true));
}

static void load_glider_gun() {
    clear_grid();
    g_pattern_name = "glider gun";
    // Gosper Glider Gun — place a few copies if grid is large enough
    static const std::vector<std::pair<int,int>> gun = {
        {1,5},{1,6},{2,5},{2,6},
        {11,5},{11,6},{11,7},{12,4},{12,8},{13,3},{13,9},{14,3},{14,9},
        {15,6},{16,4},{16,8},{17,5},{17,6},{17,7},{18,6},
        {21,3},{21,4},{21,5},{22,3},{22,4},{22,5},{23,2},{23,6},
        {25,1},{25,2},{25,6},{25,7},
        {35,3},{35,4},{36,3},{36,4},
    };
    int copies = std::max(1, g_ph / 40);
    for (int i = 0; i < copies; ++i) {
        int ox = 4, oy = 4 + i * 30;
        stamp(gun, ox, oy);
    }
    g_population = static_cast<int>(std::count(g_cur.begin(), g_cur.end(), true));
}

static void load_pulsar() {
    clear_grid();
    g_pattern_name = "pulsar";
    // Period-3 oscillator — tile across the grid
    static const std::vector<std::pair<int,int>> pulsar = {
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
    };
    int sx = 16, sy = 16;
    for (int py = 0; py * sy < g_ph; ++py)
        for (int px = 0; px * sx < g_pw; ++px)
            stamp(pulsar, 1 + px * sx, 1 + py * sy);
    g_population = static_cast<int>(std::count(g_cur.begin(), g_cur.end(), true));
}

static void load_spaceships() {
    clear_grid();
    g_pattern_name = "spaceships";
    // Lightweight spaceship
    static const std::vector<std::pair<int,int>> lwss = {
        {1,0},{4,0},{0,1},{0,2},{4,2},{0,3},{1,3},{2,3},{3,3},
    };
    int fleet = 3;
    int cx = g_pw / 2 - 10;
    for (int i = 0; i < fleet; ++i) {
        stamp(lwss, cx, g_ph / 4 + i * 8);
        stamp(lwss, cx + 20, g_ph / 4 + i * 8 + 4);
    }
    g_population = static_cast<int>(std::count(g_cur.begin(), g_cur.end(), true));
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main() {
    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();

    auto rebuild_styles = [&](StylePool& pool, int w, int h) {
        (void)h;
        S_DEFAULT = pool.intern(Style{});
        for (int i = 0; i < NUM_AGE_STYLES; ++i) {
            auto& c = g_gradient[i];
            S_AGE_FG[i] = pool.intern(Style{}.with_fg(Color::rgb(c.r, c.g, c.b)));
            S_AGE_BG[i] = pool.intern(Style{}.with_fg(Color::rgb(c.r, c.g, c.b)));
        }
        for (int t = 0; t < NUM_AGE_STYLES; ++t) {
            for (int b = 0; b < NUM_AGE_STYLES; ++b) {
                auto& tc = g_gradient[t];
                auto& bc = g_gradient[b];
                S_BOTH[t * NUM_AGE_STYLES + b] = pool.intern(
                    Style{}.with_fg(Color::rgb(tc.r, tc.g, tc.b))
                           .with_bg(Color::rgb(bc.r, bc.g, bc.b)));
            }
        }
        S_BAR     = pool.intern(Style{}.with_fg(Color::rgb(180, 180, 180)).with_bg(Color::rgb(20, 20, 30)));
        S_BAR_DIM = pool.intern(Style{}.with_fg(Color::rgb(80, 80, 100)).with_bg(Color::rgb(20, 20, 30)));
        S_BAR_ACC = pool.intern(Style{}.with_fg(Color::rgb(100, 255, 180)).with_bg(Color::rgb(20, 20, 30)).with_bold());
        S_BAR_KEY = pool.intern(Style{}.with_fg(Color::rgb(255, 200, 80)).with_bg(Color::rgb(20, 20, 30)).with_bold());

        // Reallocate pixel grids: w columns, h*2 pixel rows (half-block doubling)
        g_pw = w;
        g_ph = h * 2;  // status bar row handled by reserving last terminal row
        int total = g_pw * g_ph;
        g_cur.assign(total, false);
        g_nxt.assign(total, false);
        g_age.assign(total, 0);
        load_random();
    };

    (void)canvas_run(
        CanvasConfig{.fps = 30, .mouse = false, .mode = Mode::Fullscreen, .title = "life"},

        rebuild_styles,

        [&](const Event& ev) -> bool {
            if (key(ev, 'q') || key(ev, SpecialKey::Escape)) return false;
            on(ev, ' ', [&] { g_paused = !g_paused; });
            on(ev, SpecialKey::Enter, [&] { g_step = true; });
            on(ev, '+', '=', [&] { g_interval = std::max(0.01f, g_interval - 0.02f); });
            on(ev, '-', [&] { g_interval = std::min(1.0f, g_interval + 0.02f); });
            on(ev, 'c', [&] { clear_grid(); g_pattern_name = "empty"; });
            on(ev, 'r', [&] { load_random(); });
            on(ev, 'g', [&] { load_glider_gun(); });
            on(ev, 'p', [&] { load_pulsar(); });
            on(ev, 's', [&] { load_spaceships(); });
            return true;
        },

        [&](Canvas& canvas, int w, int h) {
            auto now = Clock::now();
            float dt = std::chrono::duration<float>(now - last).count();
            last = now;

            // Advance simulation
            if (!g_paused || g_step) {
                g_accum += dt;
                int steps = g_step ? 1 : static_cast<int>(g_accum / g_interval);
                if (steps > 0) {
                    for (int i = 0; i < std::min(steps, 8); ++i)
                        step_generation();
                    g_accum = 0.f;
                }
                g_step = false;
            }

            int bar_y = h - 1;
            int grid_rows = h - 1;  // terminal rows for the grid

            // ── Paint cells ──────────────────────────────────────────────
            for (int ty = 0; ty < grid_rows; ++ty) {
                int py_top = ty * 2;
                int py_bot = ty * 2 + 1;
                for (int x = 0; x < w; ++x) {
                    bool top = (py_top < g_ph && x < g_pw) && g_cur[py_top * g_pw + x];
                    bool bot = (py_bot < g_ph && x < g_pw) && g_cur[py_bot * g_pw + x];

                    if (top && bot) {
                        int tb = age_bucket(g_age[py_top * g_pw + x]);
                        int bb = age_bucket(g_age[py_bot * g_pw + x]);
                        canvas.set(x, ty, U'\u2580', S_BOTH[tb * NUM_AGE_STYLES + bb]);
                    } else if (top) {
                        int tb = age_bucket(g_age[py_top * g_pw + x]);
                        canvas.set(x, ty, U'\u2580', S_AGE_FG[tb]);
                    } else if (bot) {
                        int bb = age_bucket(g_age[py_bot * g_pw + x]);
                        canvas.set(x, ty, U'\u2584', S_AGE_FG[bb]);
                    }
                    // else: leave as cleared (space)
                }
            }

            // ── Status bar ───────────────────────────────────────────────
            // Fill bar background
            for (int x = 0; x < w; ++x)
                canvas.set(x, bar_y, U' ', S_BAR);

            char buf[256];
            int speed_pct = static_cast<int>(0.1f / g_interval * 100.f);
            std::snprintf(buf, sizeof(buf),
                " Gen %d  Pop %d  Speed %d%%  [%s]%s ",
                g_generation, g_population, speed_pct, g_pattern_name,
                g_paused ? "  PAUSED" : "");
            canvas.write_text(0, bar_y, buf, S_BAR_ACC);

            // Right-aligned keybindings
            const char* keys = " r/g/p/s=pattern  spc=pause  +/-=speed  c=clear  q=quit ";
            int klen = static_cast<int>(std::strlen(keys));
            if (klen < w)
                canvas.write_text(w - klen, bar_y, keys, S_BAR_DIM);
        }
    );
}
