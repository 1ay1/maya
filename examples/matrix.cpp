// examples/matrix.cpp — the Matrix digital rain, as a jaal program.
//
// Two streams per column fall at their own speed; each draws a fading trail
// of half-width katakana and digits, the glyphs under a trail flicker, and a
// bright head leads it. Four colour modes, and a "WAKE UP NEO" message that
// fades in and out with glitched letters.
//
//   Model      the streams (position, speed, gap, trail, their characters),
//              the colour mode, pause, the message timer, frame, RNG.
//   update()   Tick moves every stream (and resets the ones that fell off);
//              keys pick a mode, pause, or trigger the message.
//   view()     a Glyphs grid (characters + colours) and a status bar. The
//              colours are computed here, from the mode: no style tables.
//
// Keys: 1-4 colour mode   m message   space pause   q quit

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

constexpr std::u32string_view kCharset =
    U"ｦｧｨｩｪｫｬｭｮｯｰｱｲｳｴｵｶｷｸｹｺｻｼｽｾｿﾀﾁﾂﾃﾄﾅﾆﾇﾈﾉﾊﾋﾌﾍﾎﾏﾐﾑﾒﾓﾔﾕﾖﾗﾘﾙﾚﾛﾜﾝ"
    U"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
constexpr int kTrail = 24, kMinSpeed = 1, kMaxSpeed = 4, kMinGap = 4, kMaxGap = 30;
constexpr int kMessageTicks = 90;
constexpr std::u32string_view kMessage = U"WAKE UP NEO";
constexpr std::array<float, 6> kHues = {120, 180, 270, 300, 90, 160};

enum class Mode { Classic, MultiColour, RedPill, Rainbow };
constexpr std::array<const char*, 4> kModeName = {"CLASSIC", "MULTI-COLOR", "RED PILL", "RAINBOW"};

Rgb hsv(float h, float s, float v) {
    const float c = v * s, x = c * (1 - std::fabs(std::fmod(h / 60.f, 2.f) - 1)), m = v - c;
    float r = 0, g = 0, b = 0;
    if (h < 60) { r = c; g = x; } else if (h < 120) { r = x; g = c; } else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; } else if (h < 300) { r = x; b = c; } else { r = c; b = x; }
    return {static_cast<std::uint8_t>((r + m) * 255), static_cast<std::uint8_t>((g + m) * 255),
            static_cast<std::uint8_t>((b + m) * 255)};
}

// ── model ────────────────────────────────────────────────────────────────────

struct Stream {
    float y = 0;                 // head row (fractional while it falls)
    int   speed = 1, gap = 0, trail = kTrail, hue = 0;
    std::vector<char32_t> chars; // one per row: the glyph this stream shows there
};

struct Model {
    int w = 0, h = 0;            // the rain area, in cells (the status bar is one more row)
    std::vector<Stream> streams; // 2 per column: stream i falls in column i % w
    Mode mode = Mode::Classic;
    bool paused = false;
    int  message = 0;            // ticks left of "WAKE UP NEO"
    int  frame = 0;
    std::mt19937 rng{std::random_device{}()};

    int roll(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); }
    char32_t glyph() { return kCharset[static_cast<std::size_t>(roll(0, static_cast<int>(kCharset.size()) - 1))]; }
};

// ── messages ─────────────────────────────────────────────────────────────────

struct Tick {};
struct Resize  { int cols, rows; };
struct SetMode { Mode m; };
struct Message {};
struct Pause   {};
struct Quit    {};
using Msg = std::variant<Tick, Resize, SetMode, Message, Pause, Quit>;

// ── rules ────────────────────────────────────────────────────────────────────

void reset(Model& m, Stream& s, bool first) {
    s.y = first ? -static_cast<float>(m.roll(0, m.h)) : -static_cast<float>(m.roll(2, 12));
    s.speed = m.roll(kMinSpeed, kMaxSpeed);
    s.gap = first ? 0 : m.roll(kMinGap, kMaxGap);
    s.trail = m.roll(kTrail / 2, kTrail);
    s.hue = m.roll(0, static_cast<int>(kHues.size()) - 1);
    s.chars.resize(static_cast<std::size_t>(m.h));
    for (auto& c : s.chars) c = m.glyph();
}

void step(Model& m, Stream& s) {
    if (s.gap > 0) { --s.gap; return; }
    s.y += static_cast<float>(s.speed);
    if (m.roll(0, 3) == 0) {                       // a glyph in the trail flickers
        const int row = static_cast<int>(s.y) - m.roll(1, s.trail);
        if (row >= 0 && row < m.h) s.chars[static_cast<std::size_t>(row)] = m.glyph();
    }
    if (static_cast<int>(s.y) - s.trail > m.h) reset(m, s, false);
}

// The trail's colour: `t` is brightness (1 at the head, fading to 0).
Rgb trail_colour(const Model& m, int col, int hue, float t, bool head) {
    const int rainbow = ((col * 64 / std::max(m.w, 1)) + m.frame / 3) % 64;
    switch (m.mode) {
        case Mode::Classic:
            return head ? Rgb{220, 255, 220}
                        : Rgb{static_cast<std::uint8_t>(t * t * 80), static_cast<std::uint8_t>(30 + t * 225), 0};
        case Mode::MultiColour:
            return head ? hsv(kHues[static_cast<std::size_t>(hue)], 0.2f, 1.f)
                        : hsv(kHues[static_cast<std::size_t>(hue)], 0.8f, 0.15f + t * 0.85f);
        case Mode::RedPill:
            return head ? Rgb{255, 200, 200}
                        : Rgb{static_cast<std::uint8_t>(30 + t * 225), static_cast<std::uint8_t>(t * t * 40), 0};
        case Mode::Rainbow:
            return head ? hsv(rainbow * 360.f / 64, 0.15f, 1.f) : hsv(rainbow * 360.f / 64, 0.85f, 0.12f + t * 0.88f);
    }
    return {};
}

// ── program ──────────────────────────────────────────────────────────────────

struct Matrix {
    using Model = ::Model;
    using Msg   = ::Msg;
    using Cmd   = jaal::Cmd<Msg>;
    using Sub   = jaal::Sub<Msg, on_key, on_resize>;

    static Cmd update(Model& m, Resize r) {
        m.w = std::max(1, r.cols);
        m.h = std::max(1, r.rows - 1);
        m.streams.assign(static_cast<std::size_t>(m.w * 2), Stream{});
        for (std::size_t i = 0; i < m.streams.size(); ++i) {
            reset(m, m.streams[i], true);
            if (static_cast<int>(i) >= m.w) m.streams[i].y -= static_cast<float>(m.roll(0, m.h));
        }
        return {};
    }
    static Cmd update(Model& m, Tick) {
        if (m.paused || m.w == 0) return {};
        ++m.frame;
        if (m.message > 0) --m.message;
        for (auto& s : m.streams) step(m, s);
        return {};
    }
    static Cmd update(Model& m, SetMode s) { m.mode = s.m; return {}; }
    static Cmd update(Model& m, Message)   { m.message = kMessageTicks; return {}; }
    static Cmd update(Model& m, Pause)     { m.paused = !m.paused; return {}; }
    static Cmd update(Model&, Quit)        { return Cmd::quit(0); }

    // ── view ─────────────────────────────────────────────────────────────────

    static Glyphs rain(const Model& m) {
        Glyphs g(m.w, m.h, Glyph{U' ', {0, 0, 0}, {0, 0, 0}});
        for (std::size_t i = 0; i < m.streams.size(); ++i) {
            const Stream& s = m.streams[i];
            if (s.gap > 0) continue;
            const int col = static_cast<int>(i) % m.w, head = static_cast<int>(s.y);
            for (int k = 0; k <= s.trail; ++k) {
                const int y = head - k;
                if (y < 0 || y >= m.h) continue;
                const float t = 1.f - static_cast<float>(k) / static_cast<float>(s.trail);
                g(col, y) = {s.chars[static_cast<std::size_t>(y)], trail_colour(m, col, s.hue, t, k == 0),
                             {0, 0, 0}, k == 0};
            }
        }
        // WAKE UP NEO: fades in over the first half, out over the second;
        // while faint, some letters glitch to random glyphs.
        if (m.message > 0) {
            const float p = static_cast<float>(m.message) / kMessageTicks;
            const float alpha = std::clamp(p > 0.5f ? (1 - p) * 2 : p * 2, 0.f, 1.f);
            const int x0 = (m.w - static_cast<int>(kMessage.size())) / 2, y = m.h / 2;
            if (alpha > 0.15f)
                for (std::size_t i = 0; i < kMessage.size(); ++i) {
                    if (kMessage[i] == U' ' || !g.in(x0 + static_cast<int>(i), y)) continue;
                    const bool glitch = alpha <= 0.4f && (m.frame + static_cast<int>(i) * 7) % 4 != 0;
                    const char32_t ch = glitch ? kCharset[static_cast<std::size_t>((m.frame * 31 + static_cast<int>(i) * 17) % static_cast<int>(kCharset.size()))]
                                               : kMessage[i];
                    g(x0 + static_cast<int>(i), y) = {ch, {255, 255, 255}, {0, 0, 0}, true};
                }
        }
        return g;
    }

    static Element status_bar(const Model& m) {
        auto dim = [](std::string s) { return text(std::move(s)) | fgc(Color::rgb(60, 60, 60)); };
        auto accent = [](std::string s) { return text(std::move(s)) | fgc(Color::rgb(0, 200, 0)) | Bold; };
        return h(text(" "), accent("MATRIX"),
                 dim(" │ [1-4] mode │ [m] message │ [space] pause │ [q] quit"),
                 spacer(),
                 accent(m.paused ? "PAUSED" : ""),
                 spacer(),
                 accent(kModeName[static_cast<std::size_t>(m.mode)]), text(" ")) | bgc(Color::rgb(10, 10, 10));
    }

    static Element view(const Model& m) {
        if (m.w == 0) return text("");
        return v(glyphs(rain(m)), status_bar(m));
    }

    // ── subscribe ────────────────────────────────────────────────────────────

    static Sub subscribe(const Model&) {
        return Sub::batch(
            Sub::every(33ms, Tick{}),          // 30 fps, like the original
            Sub::on(on_resize{}, [](const ResizeEvent& r) -> std::optional<Msg> {
                return Resize{r.width.value, r.height.value};
            }),
            keys<Sub>({
                {'1', SetMode{Mode::Classic}}, {'2', SetMode{Mode::MultiColour}},
                {'3', SetMode{Mode::RedPill}}, {'4', SetMode{Mode::Rainbow}},
                {'m', Message{}}, {' ', Pause{}},
                {'q', Quit{}}, {SpecialKey::Escape, Quit{}},
            }));
    }
    static bool subs_key(const Model&) { return true; }
};

static_assert(Program<Matrix>);

}  // namespace

int main() { return run<Matrix>({.title = "matrix"}); }
