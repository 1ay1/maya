// maya -- Snake: half-block pixels, gradient body, particle effects, ghost trails
// arrows/wasd/hjkl=move  space=pause  W=wrap  r=restart  q/Esc=quit

#include <maya/maya.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <random>
#include <vector>

using namespace maya;

static constexpr int SNAKE_GRAD = 32, PARTICLE_LIFE = 15, TRAIL_LIFE = 10;
static constexpr int INIT_TICK = 7, MIN_TICK = 3;

enum class FoodKind : uint8_t { Normal, Speed, Mega };
struct Particle { float x, y, vx, vy; int life; int style_idx; };
struct Trail    { int x, y, fade; };

static std::mt19937 g_rng{std::random_device{}()};
static int g_pw = 0, g_ph = 0;  // playfield pixel size
static int g_tw = 0, g_th = 0;  // terminal cell size
static std::deque<std::pair<int,int>> g_snake;
static int g_dx = 1, g_dy = 0, g_qdx = 1, g_qdy = 0;
static bool g_alive = true, g_paused = false, g_wrap = false;
static int g_score = 0, g_high = 0, g_eaten = 0;
static int g_tick_rate = INIT_TICK, g_frame = 0;

struct Food { int x, y; FoodKind kind; };
static Food g_food;
static std::vector<Particle> g_particles;
static std::vector<Trail>    g_trails;

static uint16_t s_bg, s_border;
static uint16_t s_snake[SNAKE_GRAD];
static uint16_t s_food_normal[8], s_food_speed[8], s_food_mega[8];
static uint16_t s_particle[PARTICLE_LIFE], s_trail[TRAIL_LIFE];
static uint16_t s_bar_bg, s_bar_dim, s_bar_accent, s_bar_score;
static uint16_t s_gameover, s_gameover_dim;

static Color lerp_color(Color a, Color b, float t) {
    auto ar = a.r(), ag = a.g(), ab = a.b();
    auto br = b.r(), bg = b.g(), bb = b.b();
    return Color::rgb(
        uint8_t(ar + (br - ar) * t),
        uint8_t(ag + (bg - ag) * t),
        uint8_t(ab + (bb - ab) * t));
}

static Color snake_gradient(int i) {
    // green -> cyan -> blue -> purple  (4-stop gradient)
    static constexpr Color stops[] = {
        Color::rgb(57, 255, 20),   // neon green
        Color::rgb(0, 255, 200),   // cyan
        Color::rgb(30, 100, 255),  // blue
        Color::rgb(160, 40, 220),  // purple
    };
    float t = float(i) / float(SNAKE_GRAD - 1) * 3.0f;
    int seg = std::min(int(t), 2);
    return lerp_color(stops[seg], stops[seg + 1], t - seg);
}

static void spawn_food() {
    auto occupied = [](int x, int y) {
        for (auto& [sx, sy] : g_snake) if (sx == x && sy == y) return true;
        return false;
    };
    std::uniform_int_distribution<int> dx(1, g_pw - 2), dy(1, g_ph - 2);
    do { g_food.x = dx(g_rng); g_food.y = dy(g_rng); } while (occupied(g_food.x, g_food.y));

    std::uniform_int_distribution<int> kind_dist(0, 9);
    int k = kind_dist(g_rng);
    g_food.kind = (k < 6) ? FoodKind::Normal : (k < 9) ? FoodKind::Speed : FoodKind::Mega;
}

static void spawn_particles(int x, int y, Color base) {
    std::uniform_real_distribution<float> angle(0.0f, 6.2832f);
    std::uniform_real_distribution<float> speed(0.5f, 2.5f);
    int n = 8 + int(g_rng() % 9);
    for (int i = 0; i < n; ++i) {
        float a = angle(g_rng), s = speed(g_rng);
        g_particles.push_back({float(x), float(y),
                               std::cos(a) * s, std::sin(a) * s,
                               PARTICLE_LIFE, 0});
    }
}

static void reset_game() {
    g_snake.clear();
    int cx = g_pw / 2, cy = g_ph / 2;
    for (int i = 0; i < 5; ++i) g_snake.push_back({cx - i, cy});
    g_dx = 1; g_dy = 0; g_qdx = 1; g_qdy = 0;
    g_alive = true; g_paused = false;
    g_score = 0; g_eaten = 0;
    g_tick_rate = INIT_TICK; g_frame = 0;
    g_particles.clear(); g_trails.clear();
    spawn_food();
}

static void rebuild(StylePool& pool, int w, int h) {
    g_tw = w; g_th = h;
    g_pw = w; g_ph = (h - 1) * 2;  // half-block: 2 pixel rows per cell, minus status

    // Background & border
    s_bg     = pool.intern(Style{}.with_bg(Color::rgb(10, 10, 18)));
    s_border = pool.intern(Style{}.with_fg(Color::rgb(50, 50, 65)).with_bg(Color::rgb(10, 10, 18)));

    // Snake gradient
    for (int i = 0; i < SNAKE_GRAD; ++i)
        s_snake[i] = pool.intern(Style{}.with_fg(snake_gradient(i)).with_bg(Color::rgb(10, 10, 18)));

    // Food pulsing styles (8 brightness levels)
    for (int i = 0; i < 8; ++i) {
        float b = 0.5f + 0.5f * std::sin(float(i) * 3.14159f / 4.0f);
        uint8_t v = uint8_t(120 + 135 * b);
        s_food_normal[i] = pool.intern(Style{}.with_fg(Color::rgb(v, uint8_t(30 * b), uint8_t(30 * b)))
                                               .with_bg(Color::rgb(10, 10, 18)));
        s_food_speed[i]  = pool.intern(Style{}.with_fg(Color::rgb(v, v, uint8_t(30 * b)))
                                               .with_bg(Color::rgb(10, 10, 18)));
        // Rainbow: cycle hue
        float hue = float(i) / 8.0f * 6.2832f;
        s_food_mega[i] = pool.intern(Style{}.with_fg(Color::rgb(
            uint8_t(128 + 127 * std::sin(hue)),
            uint8_t(128 + 127 * std::sin(hue + 2.094f)),
            uint8_t(128 + 127 * std::sin(hue + 4.189f))))
            .with_bg(Color::rgb(10, 10, 18)));
    }

    // Particle fade
    for (int i = 0; i < PARTICLE_LIFE; ++i) {
        float t = float(i) / float(PARTICLE_LIFE - 1);
        uint8_t v = uint8_t(255 * (1.0f - t));
        s_particle[i] = pool.intern(Style{}.with_fg(Color::rgb(v, v, uint8_t(v / 2)))
                                            .with_bg(Color::rgb(10, 10, 18)));
    }

    // Trail fade
    for (int i = 0; i < TRAIL_LIFE; ++i) {
        float t = float(i) / float(TRAIL_LIFE - 1);
        uint8_t v = uint8_t(60 * (1.0f - t));
        s_trail[i] = pool.intern(Style{}.with_fg(Color::rgb(uint8_t(20 + v / 2), uint8_t(40 + v), uint8_t(20 + v / 3)))
                                         .with_bg(Color::rgb(10, 10, 18)));
    }

    // Status bar
    s_bar_bg     = pool.intern(Style{}.with_bg(Color::rgb(20, 20, 28)).with_fg(Color::rgb(100, 100, 120)));
    s_bar_dim    = pool.intern(Style{}.with_bg(Color::rgb(20, 20, 28)).with_fg(Color::rgb(60, 60, 75)));
    s_bar_accent = pool.intern(Style{}.with_bg(Color::rgb(20, 20, 28)).with_fg(Color::rgb(57, 255, 20)).with_bold());
    s_bar_score  = pool.intern(Style{}.with_bg(Color::rgb(20, 20, 28)).with_fg(Color::rgb(255, 200, 60)).with_bold());

    // Game over
    s_gameover     = pool.intern(Style{}.with_fg(Color::rgb(255, 60, 60)).with_bg(Color::rgb(10, 10, 18)).with_bold());
    s_gameover_dim = pool.intern(Style{}.with_fg(Color::rgb(140, 140, 160)).with_bg(Color::rgb(10, 10, 18)));

    reset_game();
}

static bool handle(const Event& ev) {
    if (key(ev, 'q') || key(ev, SpecialKey::Escape)) return false;

    if (!g_alive) {
        on(ev, 'r', [] { reset_game(); });
        return true;
    }

    on(ev, ' ', [] { g_paused = !g_paused; });

    auto set_dir = [](int dx, int dy) {
        if (g_dx != -dx || g_dy != -dy) { g_qdx = dx; g_qdy = dy; }
    };

    on(ev, SpecialKey::Up,    [&] { set_dir(0, -1); });
    on(ev, SpecialKey::Down,  [&] { set_dir(0,  1); });
    on(ev, SpecialKey::Left,  [&] { set_dir(-1, 0); });
    on(ev, SpecialKey::Right, [&] { set_dir( 1, 0); });
    on(ev, 'k', [&] { set_dir(0, -1); });
    on(ev, 'j', [&] { set_dir(0,  1); });
    on(ev, 'h', [&] { set_dir(-1, 0); });
    on(ev, 'l', [&] { set_dir( 1, 0); });
    on(ev, 'w', [&] { set_dir(0, -1); });
    on(ev, 's', [&] { set_dir(0,  1); });
    on(ev, 'a', [&] { set_dir(-1, 0); });
    on(ev, 'd', [&] { set_dir( 1, 0); });

    if (auto* ke = as_key(ev)) {  // Shift+W toggles wrap
        if (auto* ck = std::get_if<CharKey>(&ke->key))
            if (ck->codepoint == U'W' || (ck->codepoint == U'w' && ke->mods.shift))
                g_wrap = !g_wrap;
    }

    return true;
}

static std::vector<uint16_t> g_pixels;  // style per pixel (0 = background)

static void px_clear() {
    g_pixels.assign(g_pw * g_ph, 0);
}

static void px_set(int x, int y, uint16_t style) {
    if (x >= 0 && x < g_pw && y >= 0 && y < g_ph)
        g_pixels[y * g_pw + x] = style;
}

static uint16_t px_get(int x, int y) {
    if (x >= 0 && x < g_pw && y >= 0 && y < g_ph)
        return g_pixels[y * g_pw + x];
    return 0;
}

static void px_composite(Canvas& canvas, StylePool& pool, int rows) {
    for (int row = 0; row < rows && row < g_th - 1; ++row) {
        int y_top = row * 2;
        int y_bot = row * 2 + 1;
        for (int x = 0; x < g_pw && x < g_tw; ++x) {
            uint16_t st = px_get(x, y_top);
            uint16_t sb = px_get(x, y_bot);
            if (st == 0 && sb == 0) {
                canvas.set(x, row, U' ', s_bg);
            } else {
                // Get fg color from top pixel's style, bg color from bottom pixel's style
                Color fg_c = (st != 0) ? pool.get(st).fg.value_or(Color::rgb(255,255,255))
                                       : Color::rgb(10, 10, 18);
                Color bg_c = (sb != 0) ? pool.get(sb).fg.value_or(Color::rgb(255,255,255))
                                       : Color::rgb(10, 10, 18);
                uint16_t combined = pool.intern(Style{}.with_fg(fg_c).with_bg(bg_c));
                canvas.set(x, row, U'\u2580', combined);  // upper half block
            }
        }
    }
}

static void paint(Canvas& canvas, int w, int h) {
    if (w != g_tw || h != g_th) return;

    int cell_rows = h - 1;

    // -- Simulation tick --
    if (g_alive && !g_paused && (g_frame % g_tick_rate) == 0) {
        g_dx = g_qdx; g_dy = g_qdy;
        auto [hx, hy] = g_snake.front();
        int nx = hx + g_dx, ny = hy + g_dy;

        // Wall / wrap
        if (g_wrap) {
            if (nx < 1) nx = g_pw - 2; else if (nx >= g_pw - 1) nx = 1;
            if (ny < 1) ny = g_ph - 2; else if (ny >= g_ph - 1) ny = 1;
        } else {
            if (nx < 1 || nx >= g_pw - 1 || ny < 1 || ny >= g_ph - 1) {
                g_alive = false; g_high = std::max(g_high, g_score);
            }
        }

        // Self collision
        if (g_alive) {
            for (auto& [sx, sy] : g_snake)
                if (sx == nx && sy == ny) { g_alive = false; g_high = std::max(g_high, g_score); break; }
        }

        if (g_alive) {
            g_snake.push_front({nx, ny});
            bool ate = (nx == g_food.x && ny == g_food.y);
            if (ate) {
                int pts = (g_food.kind == FoodKind::Mega) ? 50 : (g_food.kind == FoodKind::Speed) ? 15 : 10;
                g_score += pts;
                g_eaten++;
                if (g_eaten % 5 == 0) g_tick_rate = std::max(MIN_TICK, g_tick_rate - 1);
                if (g_food.kind == FoodKind::Speed) g_tick_rate = std::max(MIN_TICK, g_tick_rate - 1);
                spawn_particles(g_food.x, g_food.y, Color::rgb(255, 100, 50));
                // Grow: don't pop tail (add extra segments for mega)
                int extra = (g_food.kind == FoodKind::Mega) ? 4 : 0;
                for (int i = 0; i < extra; ++i) g_snake.push_back(g_snake.back());
                spawn_food();
            } else {
                // Trail from old tail position
                auto [tx, ty] = g_snake.back();
                g_trails.push_back({tx, ty, TRAIL_LIFE});
                g_snake.pop_back();
            }
        }
    }

    // Update particles
    for (auto& p : g_particles) {
        p.x += p.vx; p.y += p.vy;
        p.vx *= 0.92f; p.vy *= 0.92f;
        p.life--;
    }
    std::erase_if(g_particles, [](auto& p) { return p.life <= 0; });

    // Update trails
    for (auto& t : g_trails) t.fade--;
    std::erase_if(g_trails, [](auto& t) { return t.fade <= 0; });

    g_frame++;

    // -- Render to pixel buffer --
    px_clear();

    // 1. Border
    for (int x = 0; x < g_pw; ++x) { px_set(x, 0, s_border); px_set(x, g_ph - 1, s_border); }
    for (int y = 0; y < g_ph; ++y) { px_set(0, y, s_border); px_set(g_pw - 1, y, s_border); }

    // 2. Trails
    for (auto& t : g_trails) {
        int idx = TRAIL_LIFE - t.fade;
        if (idx >= 0 && idx < TRAIL_LIFE) px_set(t.x, t.y, s_trail[idx]);
    }

    // 3. Food (pulsing)
    {
        int pulse = (g_frame / 4) % 8;
        uint16_t fs;
        switch (g_food.kind) {
            case FoodKind::Normal: fs = s_food_normal[pulse]; break;
            case FoodKind::Speed:  fs = s_food_speed[pulse];  break;
            case FoodKind::Mega:   fs = s_food_mega[(g_frame / 2) % 8]; break;
        }
        px_set(g_food.x, g_food.y, fs);
    }

    // 4. Snake (gradient)
    {
        int len = int(g_snake.size());
        for (int i = 0; i < len; ++i) {
            int grad = (len > 1) ? i * (SNAKE_GRAD - 1) / (len - 1) : 0;
            px_set(g_snake[i].first, g_snake[i].second, s_snake[grad]);
        }
    }

    // 5. Particles
    for (auto& p : g_particles) {
        int idx = PARTICLE_LIFE - p.life;
        if (idx >= 0 && idx < PARTICLE_LIFE)
            px_set(int(p.x + 0.5f), int(p.y + 0.5f), s_particle[idx]);
    }

    // -- Composite pixels into canvas cells --
    auto* pool = canvas.style_pool();
    px_composite(canvas, *pool, cell_rows);

    // -- Game over overlay --
    if (!g_alive) {
        int cx = g_tw / 2, cy = cell_rows / 2;
        auto center_text = [&](int y, const char* text, uint16_t style) {
            int len = int(std::strlen(text));
            canvas.write_text(cx - len / 2, y, text, style);
        };
        center_text(cy - 1, "GAME OVER", s_gameover);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Score: %d  |  High: %d", g_score, g_high);
        center_text(cy + 1, buf, s_gameover_dim);
        center_text(cy + 3, "press R to restart", s_gameover_dim);
    }

    if (g_paused) {
        int cx = g_tw / 2, cy = cell_rows / 2;
        canvas.write_text(cx - 4, cy, "PAUSED", s_gameover_dim);
    }

    // -- Status bar --
    int bar_y = h - 1;
    for (int x = 0; x < w; ++x) canvas.set(x, bar_y, U' ', s_bar_bg);

    int speed_level = INIT_TICK - g_tick_rate + 1;
    char bar[256];
    std::snprintf(bar, sizeof(bar),
        "SNAKE \xe2\x94\x82 Score: %d \xe2\x94\x82 High: %d \xe2\x94\x82 Speed: %d \xe2\x94\x82 %s \xe2\x94\x82 [wasd] move \xe2\x94\x82 [space] pause \xe2\x94\x82 [W] wrap \xe2\x94\x82 [q] quit",
        g_score, g_high, speed_level, g_wrap ? "WRAP" : "WALL");

    canvas.write_text(1, bar_y, bar, s_bar_dim);
    canvas.write_text(1, bar_y, "SNAKE", s_bar_accent);

    // Highlight score value
    char score_str[16];
    std::snprintf(score_str, sizeof(score_str), "%d", g_score);
    canvas.write_text(16, bar_y, score_str, s_bar_score);
}

int main() {
    (void)canvas_run(
        CanvasConfig{.fps = 60, .title = "snake"},
        rebuild,
        handle,
        paint
    );
}
