// examples/breakout.cpp — Breakout, as a jaal program.
//
// The second reference program (after the Doom fire), for what a GAME looks
// like the new way:
//
//   Model      the whole game as a value: bricks, balls, paddle, particles,
//              power-ups, score, and a Phase (Serving / Playing / Paused /
//              Over / Won) that makes illegal combinations unrepresentable.
//   update()   Tick advances physics one step; keys are intents (Move,
//              Launch, Restart). All rules live in one place per message.
//   view()     the board as an Image (pixels(): bricks, trail, balls,
//              paddle, sparks, drawn back to front into one picture),
//              power-up letters and the game-over card as ordinary
//              elements over it (zstack), and a status bar.
//   subscribe  a 60 Hz clock and the keys.
//
// Keys: ←/→ or h/l move   space launch / pause   r restart   q quit

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

// ── Rules ────────────────────────────────────────────────────────────────────

constexpr int   kRows = 8, kBrickW = 4, kBrickH = 2, kPaddleW = 6, kTrail = 8;
constexpr int   kTop = 4;                          // pixel row of the first brick row
constexpr float kSpeed = 0.38f, kPaddleSpeed = 1.4f;

constexpr std::array<Rgb, kRows> kRowColour = {{
    {255, 60, 60}, {255, 140, 30}, {255, 220, 40}, {50, 220, 80},
    {40, 210, 230}, {70, 100, 255}, {160, 80, 220}, {230, 70, 200},
}};
constexpr std::array<int, kRows> kRowPoints = {80, 70, 60, 50, 40, 30, 20, 10};
constexpr Rgb kBoard{10, 10, 20};

constexpr Rgb scale(Rgb c, float k) {
    return {static_cast<std::uint8_t>(c.r * k), static_cast<std::uint8_t>(c.g * k),
            static_cast<std::uint8_t>(c.b * k)};
}

// ── Model ────────────────────────────────────────────────────────────────────

enum class Phase { Serving, Playing, Paused, Over, Won };
enum class Power { Wide, Multi, Slow };

struct Ball { float x = 0, y = 0, vx = 0, vy = 0; std::array<std::pair<float, float>, kTrail> trail{}; int t = 0; };
struct Spark { float x, y, vx, vy; int life; Rgb colour; };
struct Drop  { float x, y; Power kind; };

struct Model {
    int w = 0, h = 0;                  // board in PIXELS (h = 2 x rows)
    int cols = 0;                      // bricks per row
    std::vector<int> bricks;           // hit points, kRows x cols
    std::vector<Ball> balls;           // balls[0] is the one you serve
    std::vector<Spark> sparks;
    std::vector<Drop> drops;
    float paddle = 0;                  // centre x
    int   paddle_w = kPaddleW;
    int   wide_ticks = 0, slow_ticks = 0;
    int   score = 0, lives = 3, level = 1;
    Phase phase = Phase::Serving;
    std::mt19937 rng{42};

    [[nodiscard]] int brick_x0() const { return (w - (cols * (kBrickW + 1) - 1)) / 2; }
    [[nodiscard]] float paddle_top() const { return h - 4.0f; }
    [[nodiscard]] int left() const { return static_cast<int>(std::count_if(bricks.begin(), bricks.end(), [](int hp) { return hp > 0; })); }
};

// ── Messages ─────────────────────────────────────────────────────────────────

struct Tick {};
struct Resize  { int cols, rows; };
struct Move    { int dir; };
struct Launch  {};
struct Restart {};
struct Quit    {};
using Msg = std::variant<Tick, Resize, Move, Launch, Restart, Quit>;

// ── Rules as functions of the model ──────────────────────────────────────────

void serve(Model& m) {
    m.balls.assign(1, Ball{});
    m.balls[0].x = m.paddle;
    m.balls[0].y = m.paddle_top() - 1;
    m.phase = Phase::Serving;
}

void start_level(Model& m) {
    m.bricks.assign(static_cast<std::size_t>(kRows * m.cols), 0);
    for (int r = 0; r < kRows; ++r)
        for (int c = (m.level > 1 && r % 2) ? 1 : 0; c < m.cols; ++c)
            m.bricks[static_cast<std::size_t>(r * m.cols + c)] = r < 2 ? 2 : 1;
    m.sparks.clear(); m.drops.clear();
    m.paddle_w = kPaddleW; m.wide_ticks = m.slow_ticks = 0;
    serve(m);
}

void new_game(Model& m) {
    m.score = 0; m.lives = 3; m.level = 1;
    m.paddle = m.w / 2.0f;
    start_level(m);
}

void burst(Model& m, float x, float y, Rgb colour) {
    std::uniform_real_distribution<float> v(-0.6f, 0.6f);
    for (int i = 0; i < 4 && m.sparks.size() < 64; ++i)
        m.sparks.push_back({x, y, v(m.rng), v(m.rng) - 0.3f, 12, colour});
    if (std::uniform_int_distribution<int>(0, 4)(m.rng) == 0 && m.drops.size() < 4)
        m.drops.push_back({x, y, static_cast<Power>(std::uniform_int_distribution<int>(0, 2)(m.rng))});
}

void step_ball(Model& m, Ball& b) {
    b.trail[static_cast<std::size_t>(b.t)] = {b.x, b.y};
    b.t = (b.t + 1) % kTrail;
    const float speed = kSpeed + (m.level - 1) * 0.04f;
    const float k = m.slow_ticks > 0 ? 0.6f : 1.0f;
    if (b.vx == 0 && b.vy == 0) b.vy = -speed;
    b.x += b.vx * k; b.y += b.vy * k;
    if (b.x < 0)        { b.x = 0;                           b.vx =  std::abs(b.vx); }
    if (b.x >= m.w - 1) { b.x = static_cast<float>(m.w - 1); b.vx = -std::abs(b.vx); }
    if (b.y < 0)        { b.y = 0;                           b.vy =  std::abs(b.vy); }
    // The paddle: the further from centre it hits, the steeper it leaves.
    const float half = m.paddle_w / 2.0f, top = m.paddle_top();
    if (b.vy > 0 && b.y >= top && b.y < top + 2 && std::abs(b.x - m.paddle) <= half + 0.5f) {
        const float rel = std::clamp((b.x - m.paddle) / half, -1.f, 1.f);
        const float sp  = std::hypot(b.vx, b.vy) + 0.005f;
        b.vx = sp * std::sin(rel * 1.1f); b.vy = -sp * std::cos(rel * 1.1f);
        b.y = top - 1;
    }
    // Bricks: bounce off the face it came through.
    const int col = static_cast<int>(b.x - m.brick_x0()) / (kBrickW + 1);
    const int row = static_cast<int>(b.y - kTop) / (kBrickH + 1);
    if (row >= 0 && row < kRows && col >= 0 && col < m.cols && b.y >= kTop && b.x >= m.brick_x0()) {
        int& hp = m.bricks[static_cast<std::size_t>(row * m.cols + col)];
        if (hp > 0) {
            const float cx = m.brick_x0() + col * (kBrickW + 1) + kBrickW / 2.0f;
            const float cy = kTop + row * (kBrickH + 1) + kBrickH / 2.0f;
            if (--hp == 0) { m.score += kRowPoints[static_cast<std::size_t>(row)]; burst(m, cx, cy, kRowColour[static_cast<std::size_t>(row)]); }
            if (std::abs(b.x - cx) * kBrickH > std::abs(b.y - cy) * kBrickW) b.vx = -b.vx; else b.vy = -b.vy;
        }
    }
}

void catch_drops(Model& m) {
    const float l = m.paddle - m.paddle_w / 2.0f, r = m.paddle + m.paddle_w / 2.0f, top = m.paddle_top();
    std::erase_if(m.drops, [&](Drop& d) {
        d.y += 0.3f;
        if (d.y >= top && d.y < top + 3 && d.x >= l && d.x <= r) {
            switch (d.kind) {
                case Power::Wide:  m.paddle_w = kPaddleW + 4; m.wide_ticks = 600; break;
                case Power::Slow:  m.slow_ticks = 300; break;
                case Power::Multi:
                    if (m.balls.size() < 3) { Ball nb = m.balls[0]; nb.vx = -nb.vx; nb.trail = {}; m.balls.push_back(nb); }
                    break;
            }
            return true;
        }
        return d.y >= m.h;
    });
}

// ── The program ──────────────────────────────────────────────────────────────

struct Breakout {
    using Model = ::Model;
    using Msg   = ::Msg;
    using Cmd   = jaal::Cmd<Msg>;
    using Sub   = jaal::Sub<Msg, on_key, on_resize>;

    static Cmd update(Model& m, Resize r) {
        const bool first = m.w == 0;
        m.w = std::max(8, r.cols);
        m.h = std::max(8, (r.rows - 1) * 2);
        m.cols = std::max(1, (m.w - 2) / (kBrickW + 1));
        if (first || m.bricks.size() != static_cast<std::size_t>(kRows * m.cols)) new_game(m);
        m.paddle = std::clamp(m.paddle, m.paddle_w / 2.0f, m.w - m.paddle_w / 2.0f);
        return {};
    }

    static Cmd update(Model& m, Move d) {
        if (m.phase == Phase::Playing || m.phase == Phase::Serving)
            m.paddle = std::clamp(m.paddle + d.dir * kPaddleSpeed * 2,
                                  m.paddle_w / 2.0f, m.w - m.paddle_w / 2.0f);
        return {};
    }

    static Cmd update(Model& m, Launch) {
        switch (m.phase) {
            case Phase::Serving: {
                const float speed = kSpeed + (m.level - 1) * 0.04f;
                m.balls[0].vx = speed * std::uniform_real_distribution<float>(-0.4f, 0.4f)(m.rng);
                m.balls[0].vy = -speed;
                m.phase = Phase::Playing;
                break;
            }
            case Phase::Playing: m.phase = Phase::Paused;  break;
            case Phase::Paused:  m.phase = Phase::Playing; break;
            case Phase::Over:
            case Phase::Won:     new_game(m);              break;
        }
        return {};
    }

    static Cmd update(Model& m, Restart) { new_game(m); return {}; }
    static Cmd update(Model&, Quit)      { return Cmd::quit(0); }

    static Cmd update(Model& m, Tick) {
        if (m.w == 0) return {};
        // Sparks and drops keep falling whatever the phase: the world doesn't
        // freeze when the ball waits on the paddle.
        for (auto& s : m.sparks) { s.x += s.vx; s.y += s.vy; s.vy += 0.04f; --s.life; }
        std::erase_if(m.sparks, [](const Spark& s) { return s.life <= 0; });
        if (m.phase == Phase::Serving) { m.balls[0].x = m.paddle; return {}; }
        if (m.phase != Phase::Playing) return {};

        if (m.wide_ticks > 0 && --m.wide_ticks == 0) m.paddle_w = kPaddleW;
        if (m.slow_ticks > 0) --m.slow_ticks;
        for (auto& b : m.balls) step_ball(m, b);
        // Lost balls: the last one costs a life.
        std::erase_if(m.balls, [&](const Ball& b) { return b.y >= m.h; });
        if (m.balls.empty()) {
            if (--m.lives <= 0) m.phase = Phase::Over;
            else serve(m);
            return {};
        }
        catch_drops(m);
        if (m.left() == 0) { if (++m.level > 5) m.phase = Phase::Won; else start_level(m); }
        return {};
    }

    // ── view ─────────────────────────────────────────────────────────────────

    // The board, drawn back to front into one picture.
    static Image board(const Model& m) {
        Image img(m.w, m.h, kBoard);
        auto rect = [&](int x0, int y0, int w, int h, Rgb c) {
            for (int y = std::max(0, y0); y < std::min(m.h, y0 + h); ++y)
                for (int x = std::max(0, x0); x < std::min(m.w, x0 + w); ++x) img(x, y) = c;
        };
        auto dot = [&](float x, float y, Rgb c) { rect(static_cast<int>(x), static_cast<int>(y), 1, 1, c); };
        for (int r = 0; r < kRows; ++r)
            for (int c = 0; c < m.cols; ++c)
                if (const int hp = m.bricks[static_cast<std::size_t>(r * m.cols + c)]; hp > 0) {
                    const Rgb col = kRowColour[static_cast<std::size_t>(r)];
                    rect(m.brick_x0() + c * (kBrickW + 1), kTop + r * (kBrickH + 1), kBrickW, kBrickH,
                         (r < 2 && hp < 2) ? scale(col, 0.5f) : col);
                }
        for (const auto& s : m.sparks) dot(s.x, s.y, scale(s.colour, s.life / 12.f));
        for (const auto& b : m.balls) {
            for (int i = 0; i < kTrail; ++i) {
                const auto [tx, ty] = b.trail[static_cast<std::size_t>((b.t - 1 - i + kTrail) % kTrail)];
                const float k = 1.f - static_cast<float>(i) / kTrail;
                if (tx > 0 || ty > 0) dot(tx, ty, Rgb{static_cast<std::uint8_t>(180 * k * k), static_cast<std::uint8_t>(180 * k * k), static_cast<std::uint8_t>(90 * k * k)});
            }
            dot(b.x, b.y, {255, 255, 200});
        }
        rect(static_cast<int>(m.paddle - m.paddle_w / 2.0f), static_cast<int>(m.paddle_top()), m.paddle_w, 2, {240, 240, 255});
        return img;
    }

    static Element status_bar(const Model& m) {
        auto dim = [](std::string s) { return text(std::move(s)) | fgc(Color::rgb(90, 90, 110)); };
        std::string hearts;
        for (int i = 0; i < m.lives; ++i) hearts += "♥ ";
        return h(text(" BREAKOUT ") | fgc(Color::rgb(255, 200, 60)) | Bold,
                 dim("│ score "), text(std::to_string(m.score)) | Bold,
                 dim(" │ "), text(hearts) | fgc(Color::rgb(255, 60, 80)),
                 dim("│ level " + std::to_string(m.level)),
                 spacer(),
                 dim(m.phase == Phase::Serving ? "space: launch  " : m.phase == Phase::Paused ? "PAUSED  " : ""),
                 dim("←→ move  q quit ")) | bgc(Color::rgb(20, 20, 30));
    }

    static Element banner(const Model& m) {
        const bool won = m.phase == Phase::Won;
        return center()(v(text(won ? "YOU WIN!" : "GAME OVER") | Bold
                            | fgc(won ? Color::rgb(100, 255, 120) : Color::rgb(255, 60, 60)),
                        text("score " + std::to_string(m.score)) | fgc(Color::rgb(160, 160, 180)),
                        text("space: play again   q: quit") | Dim)
                      | pad<1> | border_<Round> | bgc(Color::rgb(10, 10, 20)));
    }

    static Element view(const Model& m) {
        if (m.w == 0) return text("");
        Element game = pixels(board(m));
        if (m.phase == Phase::Over || m.phase == Phase::Won)
            game = zstack({std::move(game), banner(m)});
        return v(std::move(game), status_bar(m));
    }

    // ── subscribe ────────────────────────────────────────────────────────────

    static Sub subscribe(const Model&) {
        return Sub::batch(
            Sub::every(16ms, Tick{}),
            Sub::on(on_resize{}, [](const ResizeEvent& r) -> std::optional<Msg> {
                return Resize{r.width.value, r.height.value};
            }),
            keys<Sub>({
                {SpecialKey::Left, Move{-1}}, {'h', Move{-1}},
                {SpecialKey::Right, Move{+1}}, {'l', Move{+1}},
                {' ', Launch{}}, {'r', Restart{}},
                {'q', Quit{}}, {SpecialKey::Escape, Quit{}},
            }));
    }
    static bool subs_key(const Model&) { return true; }
};

static_assert(Program<Breakout>);

}  // namespace

int main() { return run<Breakout>({.title = "breakout"}); }
