// examples/jaal_mandelbrot.cpp — an auto-zooming Mandelbrot set, as a jaal program.
//
//   Model      the view (centre, zoom), the palette, whether auto-zoom is on,
//              and the field size in pixels.
//   update()   Tick pans toward a seahorse-valley target and zooms in,
//              restarting once double precision runs out; keys pan, zoom,
//              switch palette, toggle auto, reset.
//   view()     every pixel is a pure function of the model, so the picture
//              is computed with Image::fill_rows (rows in parallel across
//              cores) and shown with pixels(); plus a status bar.
//
// Keys: 1-6 palette   +/- zoom   arrows pan   space auto   r reset   q quit

#include <maya/element/pixels.hpp>
#include <maya/jaal/host.hpp>
#include <maya/maya.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <variant>

using namespace maya;
using namespace maya::dsl;
using namespace std::chrono_literals;

namespace {

constexpr int    kMaxIter = 512;
constexpr float  kTau = 6.28318530f;
constexpr double kTargetX = -0.7463, kTargetY = 0.1102;
constexpr double kZoomSpeed = 0.015, kPanSpeed = 0.02;   // per Tick
constexpr std::array<const char*, 6> kPaletteName = {"ultra", "fire", "ocean", "neon", "gray", "rainbow"};

// ── colour ───────────────────────────────────────────────────────────────────

struct C3 { float r, g, b; };

C3 hsv(float h, float s, float v) {
    h = std::fmod(h, 1.f); if (h < 0) h += 1.f;
    const float c = v * s, x = c * (1.f - std::fabs(std::fmod(h * 6.f, 2.f) - 1.f)), m = v - c;
    C3 k = h < 1.f / 6 ? C3{c, x, 0} : h < 2.f / 6 ? C3{x, c, 0} : h < 3.f / 6 ? C3{0, c, x}
         : h < 4.f / 6 ? C3{0, x, c} : h < 5.f / 6 ? C3{x, 0, c} : C3{c, 0, x};
    return {k.r + m, k.g + m, k.b + m};
}

C3 cosine(float t, C3 a, C3 b, C3 c, C3 d) {
    auto ch = [t](float a, float b, float c, float d) { return std::clamp(a + b * std::cos(kTau * (c * t + d)), 0.f, 1.f); };
    return {ch(a.r, b.r, c.r, d.r), ch(a.g, b.g, c.g, d.g), ch(a.b, b.b, c.b, d.b)};
}

C3 palette(int which, float t) {
    switch (which) {
        case 1:                                                  // fire
            if (t < 0.33f) { const float s = t / 0.33f; return {s, s * 0.15f, 0}; }
            if (t < 0.66f) { const float s = (t - 0.33f) / 0.33f; return {1, 0.15f + s * 0.65f, 0}; }
            { const float s = (t - 0.66f) / 0.34f; return {1, 0.8f + s * 0.2f, s}; }
        case 2:                                                  // ocean
            if (t < 0.5f) { const float s = t / 0.5f; return {0, s * 0.4f, 0.15f + s * 0.55f}; }
            { const float s = (t - 0.5f) / 0.5f; return {s * 0.7f, 0.4f + s * 0.6f, 0.7f + s * 0.3f}; }
        case 3: return cosine(t, {.5f, .5f, .5f}, {.5f, .5f, .5f}, {1, 1, .5f}, {.8f, .2f, .5f});   // neon
        case 4: {                                                // gray with coloured bands
            const float band = std::sin(t * 40.f);
            if (band > 0.85f)  return {0.2f + t * 0.6f, 0.1f + t * 0.3f, 0.05f};
            if (band < -0.85f) return {0.05f, 0.1f + t * 0.3f, 0.2f + t * 0.6f};
            return {t, t, t};
        }
        case 5: return hsv(t, 0.85f, 0.9f);                      // rainbow
        default: return cosine(t, {.5f, .5f, .5f}, {.5f, .5f, .5f}, {1, 1, 1}, {0, .1f, .2f});    // ultra
    }
}

// Smooth escape count, or -1 inside the set (cardioid and bulb skipped outright).
float escape(double cr, double ci, int max_iter) {
    const double q = (cr - 0.25) * (cr - 0.25) + ci * ci;
    if (q * (q + (cr - 0.25)) <= 0.25 * ci * ci) return -1.f;
    if ((cr + 1.0) * (cr + 1.0) + ci * ci <= 0.0625) return -1.f;
    double zr = 0, zi = 0, zr2 = 0, zi2 = 0;
    int i = 0;
    for (; i < max_iter && zr2 + zi2 <= 256.0; ++i) {
        zi = 2.0 * zr * zi + ci; zr = zr2 - zi2 + cr;
        zr2 = zr * zr; zi2 = zi * zi;
    }
    if (i >= max_iter) return -1.f;
    return static_cast<float>(i) + 1.f - std::log2(std::log2(static_cast<float>(std::sqrt(zr2 + zi2))));
}

// ── model ────────────────────────────────────────────────────────────────────

struct Model {
    int w = 0, h = 0;                          // the field, in pixels
    double cx = -0.5, cy = 0.0, zoom = 1.0;    // zoom = half-height in the complex plane
    int palette = 0;
    bool autozoom = true;

    [[nodiscard]] int iterations() const {
        return std::clamp(static_cast<int>(256 + 40 * std::log2(1.0 / zoom)), 256, kMaxIter);
    }
};

// ── messages ─────────────────────────────────────────────────────────────────

struct Tick {};
struct Resize     { int cols, rows; };
struct SetPalette { int p; };
struct Zoom       { double by; };
struct Pan        { double dx, dy; };
struct ToggleAuto {};
struct Reset      {};
struct Quit       {};
using Msg = std::variant<Tick, Resize, SetPalette, Zoom, Pan, ToggleAuto, Reset, Quit>;

// ── program ──────────────────────────────────────────────────────────────────

struct Mandelbrot {
    using Model = ::Model;
    using Msg   = ::Msg;
    using Cmd   = jaal::Cmd<Msg>;
    using Sub   = jaal::Sub<Msg, on_key, on_resize>;

    static Cmd update(Model& m, Resize r) { m.w = std::max(1, r.cols); m.h = std::max(2, (r.rows - 1) * 2); return {}; }
    static Cmd update(Model& m, SetPalette s) { m.palette = s.p; return {}; }
    static Cmd update(Model& m, Zoom z)       { m.zoom *= z.by; return {}; }
    static Cmd update(Model& m, Pan p)        { m.cx += p.dx * m.zoom; m.cy += p.dy * m.zoom; return {}; }
    static Cmd update(Model& m, ToggleAuto)   { m.autozoom = !m.autozoom; return {}; }
    static Cmd update(Model& m, Reset)        { m.cx = -0.5; m.cy = 0; m.zoom = 1; m.autozoom = true; return {}; }
    static Cmd update(Model&, Quit)           { return Cmd::quit(0); }

    static Cmd update(Model& m, Tick) {
        if (!m.autozoom) return {};
        m.cx += (kTargetX - m.cx) * kPanSpeed;
        m.cy += (kTargetY - m.cy) * kPanSpeed;
        m.zoom *= 1.0 - kZoomSpeed;
        if (m.zoom < 1e-13) { m.cx = -0.5; m.cy = 0; m.zoom = 1; }   // out of double precision: start over
        return {};
    }

    // ── view ────────────────────────────────────────────────────────────────

    static Image render(const Model& m) {
        Image img(m.w, m.h);
        const double aspect = static_cast<double>(m.w) / m.h;
        const int iters = m.iterations();
        img.fill_rows([&](int x, int y) -> Rgb {
            const double u = (2.0 * (x + 0.5) / m.w - 1.0) * aspect, v = 2.0 * (y + 0.5) / m.h - 1.0;
            const float s = escape(m.cx + u * m.zoom, m.cy + v * m.zoom, iters);
            if (s < 0) return {0, 0, 0};
            const C3 c = palette(m.palette, std::fmod(s / 64.f, 1.f));
            // 5 bits a channel: invisible in a smooth gradient, but it keeps the
            // distinct (top, bottom) colour pairs few enough that the renderer's
            // style cache stays hot instead of interning a new style per cell.
            auto b = [](float f) { return static_cast<std::uint8_t>(static_cast<int>(std::clamp(f, 0.f, 1.f) * 255.f) & 0xF8); };
            return {b(c.r), b(c.g), b(c.b)};
        });
        return img;
    }

    static Element status_bar(const Model& m) {
        auto dim    = [](std::string s) { return text(std::move(s)) | fgc(Color::rgb(90, 90, 90)); };
        auto accent = [](std::string s) { return text(std::move(s)) | fgc(Color::rgb(80, 180, 255)) | Bold; };
        char where[128];
        std::snprintf(where, sizeof where, " │ %.4e %+.4ei │ zoom %.2e │ iter %d │ ",
                      m.cx, m.cy, 1.0 / m.zoom, m.iterations());
        return h(text(" "), accent("MANDELBROT"), text(where) | fgc(Color::rgb(200, 200, 200)),
                 accent(kPaletteName[static_cast<std::size_t>(m.palette)]),
                 text(m.autozoom ? " [auto]" : "") | fgc(Color::rgb(200, 200, 200)), spacer(),
                 dim("[1-6] pal [+-] zoom [arrows] pan [spc] auto [r] reset [q] quit ")) | bgc(Color::rgb(15, 15, 15));
    }

    static Element view(const Model& m) {
        if (m.w == 0) return text("");
        return v(pixels(render(m)), status_bar(m));
    }

    static Sub subscribe(const Model&) {
        return Sub::batch(
            Sub::every(33ms, Tick{}),
            Sub::on(on_resize{}, [](const ResizeEvent& r) -> std::optional<Msg> {
                return Resize{r.width.value, r.height.value};
            }),
            jaal_key_map<Sub>({
                {'1', SetPalette{0}}, {'2', SetPalette{1}}, {'3', SetPalette{2}},
                {'4', SetPalette{3}}, {'5', SetPalette{4}}, {'6', SetPalette{5}},
                {'+', Zoom{0.8}}, {'=', Zoom{0.8}}, {'-', Zoom{1.25}},
                {SpecialKey::Left, Pan{-0.1, 0}}, {SpecialKey::Right, Pan{0.1, 0}},
                {SpecialKey::Up, Pan{0, -0.1}},   {SpecialKey::Down, Pan{0, 0.1}},
                {' ', ToggleAuto{}}, {'r', Reset{}}, {'q', Quit{}}, {SpecialKey::Escape, Quit{}},
            }));
    }
    static bool subs_key(const Model&) { return true; }
};

static_assert(JaalView<Mandelbrot>);

}  // namespace

int main() { return run_jaal<Mandelbrot>({.title = "mandelbrot"}); }
