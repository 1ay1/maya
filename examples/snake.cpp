// examples/snake.cpp — Snake, as a jaal program.
//
// Half-block pixels, a gradient body, food that pulses (red / yellow "speed" /
// rainbow "mega"), bursts of sparks when you eat, and a fading ghost trail.
//
//   Model      the snake (a deque of cells), its direction and the queued
//              turn, the food, sparks, trail, score/high/speed, wrap mode,
//              a Phase (Playing / Paused / Over), the frame count and the RNG.
//   update()   Tick advances the animation every frame and the snake every
//              `tick_rate` frames; keys are intents (Turn, Pause, Wrap...).
//   view()     the field as an Image (pixels()), a centred card over it when
//              paused or over, and a status bar.
//
// Keys: arrows / wasd / hjkl move   space pause   W wrap   r restart   q quit

#include <maya/element/pixels.hpp>
#include <maya/host/run.hpp>
#include <maya/maya.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <random>
#include <string>
#include <variant>
#include <vector>

using namespace maya;
using namespace maya::dsl;
using namespace std::chrono_literals;

namespace {

constexpr int kSparkLife = 15, kTrailLife = 10;
constexpr int kStartTick = 7, kFastestTick = 3;     // frames per snake step
constexpr Rgb kField{10, 10, 18}, kWall{50, 50, 65};

// ── colours ──────────────────────────────────────────────────────────────────

constexpr Rgb lerp(Rgb a, Rgb b, float t) {
    return {static_cast<std::uint8_t>(a.r + (b.r - a.r) * t),
            static_cast<std::uint8_t>(a.g + (b.g - a.g) * t),
            static_cast<std::uint8_t>(a.b + (b.b - a.b) * t)};
}

// Head to tail: neon green -> cyan -> blue -> purple.
Rgb body_colour(float t) {
    constexpr std::array<Rgb, 4> stops{{{57, 255, 20}, {0, 255, 200}, {30, 100, 255}, {160, 40, 220}}};
    const float s = std::clamp(t, 0.f, 1.f) * 3.f;
    const int i = std::min(static_cast<int>(s), 2);
    return lerp(stops[static_cast<std::size_t>(i)], stops[static_cast<std::size_t>(i + 1)], s - i);
}

// ── model ────────────────────────────────────────────────────────────────────

enum class Food  { Normal, Speed, Mega };
enum class Phase { Playing, Paused, Over };

struct Cell  { int x, y; bool operator==(const Cell&) const = default; };
struct Spark { float x, y, vx, vy; int life; };
struct Ghost { Cell at; int fade; };

struct Model {
    int w = 0, h = 0;                   // the field, in pixels
    std::deque<Cell> snake;             // front() is the head
    Cell dir{1, 0}, queued{1, 0};       // queued: the turn applied at the next step
    Cell food{};
    Food food_kind = Food::Normal;
    std::vector<Spark> sparks;
    std::vector<Ghost> trail;
    int score = 0, high = 0, eaten = 0;
    int tick_rate = kStartTick;
    int frame = 0;
    bool wrap = false;
    Phase phase = Phase::Playing;
    std::mt19937 rng{std::random_device{}()};

    [[nodiscard]] bool on_snake(Cell c) const { return std::ranges::find(snake, c) != snake.end(); }
};

// ── messages ─────────────────────────────────────────────────────────────────

struct Tick {};
struct Resize  { int cols, rows; };
struct Turn    { int dx, dy; };
struct Pause   {};
struct Wrap    {};
struct Restart {};
struct Quit    {};
using Msg = std::variant<Tick, Resize, Turn, Pause, Wrap, Restart, Quit>;

// ── rules ────────────────────────────────────────────────────────────────────

void place_food(Model& m) {
    std::uniform_int_distribution<int> x(1, m.w - 2), y(1, m.h - 2), kind(0, 9);
    do { m.food = {x(m.rng), y(m.rng)}; } while (m.on_snake(m.food));
    const int k = kind(m.rng);
    m.food_kind = k < 6 ? Food::Normal : k < 9 ? Food::Speed : Food::Mega;
}

void new_game(Model& m) {
    m.snake.clear();
    for (int i = 0; i < 5; ++i) m.snake.push_back({m.w / 2 - i, m.h / 2});
    m.dir = m.queued = {1, 0};
    m.phase = Phase::Playing;
    m.score = m.eaten = 0;
    m.tick_rate = kStartTick;
    m.frame = 0;
    m.sparks.clear(); m.trail.clear();
    place_food(m);
}

void burst(Model& m, Cell at) {
    std::uniform_real_distribution<float> angle(0.f, 6.2832f), speed(0.5f, 2.5f);
    const int n = 8 + static_cast<int>(m.rng() % 9);
    for (int i = 0; i < n; ++i) {
        const float a = angle(m.rng), s = speed(m.rng);
        m.sparks.push_back({static_cast<float>(at.x), static_cast<float>(at.y),
                            std::cos(a) * s, std::sin(a) * s, kSparkLife});
    }
}

// One step of the snake. Walls kill unless wrap is on; so does the body.
void step_snake(Model& m) {
    m.dir = m.queued;
    Cell next{m.snake.front().x + m.dir.x, m.snake.front().y + m.dir.y};
    if (m.wrap) {
        if (next.x < 1) next.x = m.w - 2; else if (next.x >= m.w - 1) next.x = 1;
        if (next.y < 1) next.y = m.h - 2; else if (next.y >= m.h - 1) next.y = 1;
    }
    const bool wall = next.x < 1 || next.x >= m.w - 1 || next.y < 1 || next.y >= m.h - 1;
    if (wall || m.on_snake(next)) {
        m.phase = Phase::Over;
        m.high = std::max(m.high, m.score);
        return;
    }
    m.snake.push_front(next);
    if (next == m.food) {
        m.score += m.food_kind == Food::Mega ? 50 : m.food_kind == Food::Speed ? 15 : 10;
        if (++m.eaten % 5 == 0)          m.tick_rate = std::max(kFastestTick, m.tick_rate - 1);
        if (m.food_kind == Food::Speed)  m.tick_rate = std::max(kFastestTick, m.tick_rate - 1);
        if (m.food_kind == Food::Mega)   for (int i = 0; i < 4; ++i) m.snake.push_back(m.snake.back());
        burst(m, m.food);
        place_food(m);
    } else {
        m.trail.push_back({m.snake.back(), kTrailLife});   // the tail leaves a ghost
        m.snake.pop_back();
    }
}

// ── program ──────────────────────────────────────────────────────────────────

struct Snake {
    using Model = ::Model;
    using Msg   = ::Msg;
    using Cmd   = jaal::Cmd<Msg>;
    using Sub   = jaal::Sub<Msg, on_key, on_resize>;

    static Cmd update(Model& m, Resize r) {
        m.w = std::max(8, r.cols);
        m.h = std::max(8, (r.rows - 1) * 2);   // one status row; two pixels per row
        new_game(m);
        return {};
    }

    static Cmd update(Model& m, Tick) {
        if (m.w == 0) return {};
        if (m.phase == Phase::Playing && m.frame % m.tick_rate == 0) step_snake(m);
        for (auto& s : m.sparks) { s.x += s.vx; s.y += s.vy; s.vx *= 0.92f; s.vy *= 0.92f; --s.life; }
        std::erase_if(m.sparks, [](const Spark& s) { return s.life <= 0; });
        for (auto& g : m.trail) --g.fade;
        std::erase_if(m.trail, [](const Ghost& g) { return g.fade <= 0; });
        ++m.frame;
        return {};
    }

    // A turn is queued (applied at the next step) and can't reverse onto the
    // body: pressing Left while moving Right is ignored.
    static Cmd update(Model& m, Turn t) {
        if (m.phase == Phase::Playing && (m.dir.x != -t.dx || m.dir.y != -t.dy)) m.queued = {t.dx, t.dy};
        return {};
    }
    static Cmd update(Model& m, Pause) {
        if (m.phase != Phase::Over) m.phase = m.phase == Phase::Paused ? Phase::Playing : Phase::Paused;
        return {};
    }
    static Cmd update(Model& m, Wrap)    { m.wrap = !m.wrap; return {}; }
    static Cmd update(Model& m, Restart) { if (m.phase == Phase::Over) new_game(m); return {}; }
    static Cmd update(Model&, Quit)      { return Cmd::quit(0); }

    // ── view ─────────────────────────────────────────────────────────────────

    static Image field(const Model& m) {
        Image img(m.w, m.h, kField);
        auto put = [&](int x, int y, Rgb c) { if (x >= 0 && x < m.w && y >= 0 && y < m.h) img(x, y) = c; };
        for (int x = 0; x < m.w; ++x) { put(x, 0, kWall); put(x, m.h - 1, kWall); }
        for (int y = 0; y < m.h; ++y) { put(0, y, kWall); put(m.w - 1, y, kWall); }
        for (const auto& g : m.trail) {
            const float k = static_cast<float>(g.fade) / kTrailLife;
            put(g.at.x, g.at.y, {static_cast<std::uint8_t>(20 + 30 * k), static_cast<std::uint8_t>(40 + 60 * k),
                                 static_cast<std::uint8_t>(20 + 20 * k)});
        }
        // Food pulses: Normal red, Speed yellow, Mega cycles the rainbow.
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>((m.frame / 4) % 8) * 3.14159f / 4.f);
        const auto v = static_cast<std::uint8_t>(120 + 135 * pulse), lo = static_cast<std::uint8_t>(30 * pulse);
        Rgb food{v, lo, lo};
        if (m.food_kind == Food::Speed) food = {v, v, lo};
        if (m.food_kind == Food::Mega) {
            const float hue = static_cast<float>((m.frame / 2) % 8) / 8.f * 6.2832f;
            food = {static_cast<std::uint8_t>(128 + 127 * std::sin(hue)),
                    static_cast<std::uint8_t>(128 + 127 * std::sin(hue + 2.094f)),
                    static_cast<std::uint8_t>(128 + 127 * std::sin(hue + 4.189f))};
        }
        put(m.food.x, m.food.y, food);
        const auto n = m.snake.size();
        for (std::size_t i = 0; i < n; ++i)
            put(m.snake[i].x, m.snake[i].y, body_colour(n > 1 ? static_cast<float>(i) / (n - 1) : 0.f));
        for (const auto& s : m.sparks) {
            const auto b = static_cast<std::uint8_t>(255 * s.life / kSparkLife);
            put(static_cast<int>(s.x + 0.5f), static_cast<int>(s.y + 0.5f), {b, b, static_cast<std::uint8_t>(b / 2)});
        }
        return img;
    }

    // A small boxed card over the field (the field stays visible around
    // it): centred by the zstack layer, sized to its content.
    static Element card(const Model& m) {
        const Color dim = Color::rgb(140, 140, 160), panel = Color::rgb(10, 10, 18);
        if (m.phase == Phase::Paused)
            return center()(text("PAUSED") | fgc(dim) | pad<1> | border_<Round> | bgc(panel));
        return center()(v(text("GAME OVER") | Bold | fgc(Color::rgb(255, 60, 60)),
                          text("Score: " + std::to_string(m.score) + "  |  High: " + std::to_string(m.high)) | fgc(dim),
                          text("press R to restart") | fgc(dim))
                        | pad<1> | border_<Round> | bgc(panel));
    }

    static Element status_bar(const Model& m) {
        // Like the original, the bar is written from column 1 and clipped at
        // the right edge: cut the tail to the columns left after the head.
        const std::string score = std::to_string(m.score);
        std::string tail = " │ High: " + std::to_string(m.high)
                         + " │ Speed: " + std::to_string(kStartTick - m.tick_rate + 1)
                         + " │ " + (m.wrap ? "WRAP" : "WALL")
                         + " │ [wasd] move │ [space] pause │ [W] wrap │ [q] quit";
        int room = m.w - int(17 + score.size());  // " SNAKE" + " │ Score: " is 16 columns, +1 spare
        std::size_t cut = 0;
        for (int cols = 0; cut < tail.size() && cols < room; ++cols)
            do ++cut; while (cut < tail.size() && (static_cast<unsigned char>(tail[cut]) & 0xC0) == 0x80);
        tail.resize(room > 0 ? cut : 0);
        auto dim = [](std::string s) { return text(std::move(s)) | fgc(Color::rgb(60, 60, 75)); };
        return h(text(" SNAKE") | fgc(Color::rgb(57, 255, 20)) | Bold,
                 dim(" │ Score: "), text(score) | fgc(Color::rgb(255, 200, 60)) | Bold,
                 dim(std::move(tail)),
                 spacer()) | bgc(Color::rgb(20, 20, 28));
    }

    static Element view(const Model& m) {
        if (m.w == 0) return text("");
        Element game = pixels(field(m));
        if (m.phase != Phase::Playing) game = zstack({std::move(game), card(m)});
        return v(std::move(game), status_bar(m));
    }

    // ── subscribe ────────────────────────────────────────────────────────────

    static Sub subscribe(const Model&) {
        return Sub::batch(
            Sub::every(16ms, Tick{}),   // 60 fps; the snake steps every tick_rate frames
            Sub::on(on_resize{}, [](const ResizeEvent& r) -> std::optional<Msg> {
                return Resize{r.width.value, r.height.value};
            }),
            keys<Sub>({
                {SpecialKey::Up, Turn{0, -1}}, {SpecialKey::Down, Turn{0, 1}},
                {SpecialKey::Left, Turn{-1, 0}}, {SpecialKey::Right, Turn{1, 0}},
                {'k', Turn{0, -1}}, {'j', Turn{0, 1}}, {'h', Turn{-1, 0}}, {'l', Turn{1, 0}},
                {'w', Turn{0, -1}}, {'s', Turn{0, 1}}, {'a', Turn{-1, 0}}, {'d', Turn{1, 0}},
                {'W', Wrap{}}, {' ', Pause{}}, {'r', Restart{}},
                {'q', Quit{}}, {SpecialKey::Escape, Quit{}},
            }));
    }
    static bool subs_key(const Model&) { return true; }
};

static_assert(Program<Snake>);

}  // namespace

int main() { return run<Snake>({.title = "snake"}); }
