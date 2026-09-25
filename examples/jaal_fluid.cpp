// examples/jaal_fluid.cpp — Stam's stable-fluids solver, as a jaal program.
//
//   Model      the fluid (density and velocity fields on a grid twice as
//              tall as the terminal, for half blocks), the pointer, the
//              palette, viscosity and the pause switch.
//   update()   Tick injects dye and momentum where the pointer is dragging
//              and steps the solver; mouse and keys change what's injected.
//   view()     density through the palette into an Image, plus a status bar.
//
// The Tick is subscribed ONLY while something is moving (dye still visible,
// or the button down): a still fluid costs no CPU at all, and subscribe()
// is where that is said, not an early return inside the step.
//
// Mouse: drag to push dye.  Keys: 1-5 palette  +/- viscosity  space pause
//                                 r reset  q quit

#include <maya/element/pixels.hpp>
#include <maya/jaal/host.hpp>
#include <maya/maya.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace maya;
using namespace maya::dsl;
using namespace std::chrono_literals;

namespace {

constexpr float kDt = 0.1f;
constexpr int   kIter = 10;
constexpr float kMaxDens = 5.f;
constexpr float kVisible = kMaxDens / 63;        // below this every palette is black

// ── palettes ─────────────────────────────────────────────────────────────────

struct Palette { const char* name; std::array<Rgb, 5> stops; };
constexpr std::array<Palette, 5> kPalettes = {{
    {"FIRE",    {{{0, 0, 0}, {140, 20, 0}, {220, 100, 0}, {255, 220, 50}, {255, 255, 255}}}},
    {"OCEAN",   {{{0, 0, 0}, {0, 20, 100}, {0, 80, 140}, {0, 200, 220}, {255, 255, 255}}}},
    {"NEON",    {{{0, 0, 0}, {80, 0, 120}, {180, 0, 180}, {255, 80, 200}, {255, 255, 255}}}},
    {"SMOKE",   {{{0, 0, 0}, {40, 40, 40}, {100, 100, 100}, {180, 180, 180}, {255, 255, 255}}}},
    {"RAINBOW", {{{0, 0, 0}, {255, 0, 80}, {0, 200, 100}, {80, 120, 255}, {255, 255, 255}}}},
}};

// 64 steps per palette: plenty for a smooth ramp, few enough colours that the
// renderer's style cache stays hot.
constexpr int kSteps = 64;
using Lut = std::array<std::array<Rgb, kSteps>, kPalettes.size()>;
const Lut kLut = [] {
    Lut l{};
    for (std::size_t p = 0; p < kPalettes.size(); ++p)
        for (int i = 0; i < kSteps; ++i) {
            const float t = static_cast<float>(i) / (kSteps - 1) * 4.f;
            const int seg = std::min(static_cast<int>(t), 3);
            const float f = t - static_cast<float>(seg);
            const Rgb a = kPalettes[p].stops[static_cast<std::size_t>(seg)], b = kPalettes[p].stops[static_cast<std::size_t>(seg + 1)];
            auto mix = [f](std::uint8_t x, std::uint8_t y) { return static_cast<std::uint8_t>(x + (y - x) * f); };
            l[p][static_cast<std::size_t>(i)] = {mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b)};
        }
    return l;
}();

// ── the solver (Jos Stam, "Real-Time Fluid Dynamics for Games") ─────────────

struct Fluid {
    int n = 0, m = 0;                                      // width, height
    std::vector<float> dens, dens0, vx, vy, vx0, vy0;

    void resize(int w, int h) {
        n = std::max(4, w); m = std::max(4, h);
        const auto sz = static_cast<std::size_t>(n * m);
        for (auto* f : {&dens, &dens0, &vx, &vy, &vx0, &vy0}) f->assign(sz, 0.f);
    }
    void clear() { resize(n, m); }

    [[nodiscard]] int ix(int x, int y) const { return y * n + x; }
    [[nodiscard]] bool interior(int x, int y) const { return x >= 1 && x < n - 1 && y >= 1 && y < m - 1; }
    [[nodiscard]] bool visible() const {
        return std::ranges::any_of(dens, [](float d) { return d >= kVisible; });
    }

    void set_bnd(int b, std::vector<float>& x) const {
        for (int i = 1; i < n - 1; ++i) {
            x[ix(i, 0)]     = b == 2 ? -x[ix(i, 1)]     : x[ix(i, 1)];
            x[ix(i, m - 1)] = b == 2 ? -x[ix(i, m - 2)] : x[ix(i, m - 2)];
        }
        for (int j = 1; j < m - 1; ++j) {
            x[ix(0, j)]     = b == 1 ? -x[ix(1, j)]     : x[ix(1, j)];
            x[ix(n - 1, j)] = b == 1 ? -x[ix(n - 2, j)] : x[ix(n - 2, j)];
        }
        x[ix(0, 0)]         = 0.5f * (x[ix(1, 0)] + x[ix(0, 1)]);
        x[ix(0, m - 1)]     = 0.5f * (x[ix(1, m - 1)] + x[ix(0, m - 2)]);
        x[ix(n - 1, 0)]     = 0.5f * (x[ix(n - 2, 0)] + x[ix(n - 1, 1)]);
        x[ix(n - 1, m - 1)] = 0.5f * (x[ix(n - 2, m - 1)] + x[ix(n - 1, m - 2)]);
    }

    // Gauss-Seidel relaxation of (x - a*laplacian(x)) = x0.
    void relax(int b, std::vector<float>& x, const std::vector<float>& x0, float a, float inv_c) const {
        for (int k = 0; k < kIter; ++k) {
            for (int j = 1; j < m - 1; ++j) {
                float* __restrict row = x.data() + j * n;
                const float* __restrict up = row - n;
                const float* __restrict dn = row + n;
                const float* __restrict src = x0.data() + j * n;
                for (int i = 1; i < n - 1; ++i)
                    row[i] = (src[i] + a * (row[i - 1] + row[i + 1] + up[i] + dn[i])) * inv_c;
            }
            set_bnd(b, x);
        }
    }

    void diffuse(int b, std::vector<float>& x, const std::vector<float>& x0, float diff) const {
        const float a = kDt * diff * static_cast<float>((n - 2) * (m - 2));
        relax(b, x, x0, a, 1.f / (1.f + 4.f * a));
    }

    void advect(int b, std::vector<float>& d, const std::vector<float>& d0,
                const std::vector<float>& u, const std::vector<float>& v) const {
        const float dtx = kDt * static_cast<float>(n - 2), dty = kDt * static_cast<float>(m - 2);
        for (int j = 1; j < m - 1; ++j)
            for (int i = 1; i < n - 1; ++i) {
                const float x = std::clamp(static_cast<float>(i) - dtx * u[ix(i, j)], 0.5f, static_cast<float>(n) - 1.5f);
                const float y = std::clamp(static_cast<float>(j) - dty * v[ix(i, j)], 0.5f, static_cast<float>(m) - 1.5f);
                const int i0 = static_cast<int>(x), j0 = static_cast<int>(y);
                const float s1 = x - static_cast<float>(i0), s0 = 1.f - s1;
                const float t1 = y - static_cast<float>(j0), t0 = 1.f - t1;
                d[ix(i, j)] = s0 * (t0 * d0[ix(i0, j0)] + t1 * d0[ix(i0, j0 + 1)])
                            + s1 * (t0 * d0[ix(i0 + 1, j0)] + t1 * d0[ix(i0 + 1, j0 + 1)]);
            }
        set_bnd(b, d);
    }

    // Make the velocity field divergence-free (mass-conserving).
    void project(std::vector<float>& u, std::vector<float>& v, std::vector<float>& p, std::vector<float>& div) const {
        const float hx = 1.f / static_cast<float>(n - 2), hy = 1.f / static_cast<float>(m - 2);
        for (int j = 1; j < m - 1; ++j)
            for (int i = 1; i < n - 1; ++i) {
                div[ix(i, j)] = -0.5f * (hx * (u[ix(i + 1, j)] - u[ix(i - 1, j)]) + hy * (v[ix(i, j + 1)] - v[ix(i, j - 1)]));
                p[ix(i, j)] = 0.f;
            }
        set_bnd(0, div);
        set_bnd(0, p);
        relax(0, p, div, 1.f, 0.25f);
        for (int j = 1; j < m - 1; ++j)
            for (int i = 1; i < n - 1; ++i) {
                u[ix(i, j)] -= 0.5f * (p[ix(i + 1, j)] - p[ix(i - 1, j)]) * static_cast<float>(n - 2);
                v[ix(i, j)] -= 0.5f * (p[ix(i, j + 1)] - p[ix(i, j - 1)]) * static_cast<float>(m - 2);
            }
        set_bnd(1, u);
        set_bnd(2, v);
    }

    void step(float visc, float diff) {
        std::swap(vx, vx0); diffuse(1, vx, vx0, visc);
        std::swap(vy, vy0); diffuse(2, vy, vy0, visc);
        project(vx, vy, vx0, vy0);
        std::swap(vx, vx0); std::swap(vy, vy0);
        advect(1, vx, vx0, vx0, vy0);
        advect(2, vy, vy0, vx0, vy0);
        project(vx, vy, vx0, vy0);
        std::swap(dens, dens0); diffuse(0, dens, dens0, diff);
        std::swap(dens, dens0); advect(0, dens, dens0, vx, vy);
        for (auto& d : dens) d = std::clamp(d, 0.f, kMaxDens);
    }

    // A disc of dye at (cx, cy), and the drag (dx, dy) as momentum.
    void inject(int cx, int cy, float dx, float dy) {
        constexpr int r = 3;
        for (int oy = -r; oy <= r; ++oy)
            for (int ox = -r; ox <= r; ++ox) {
                const int x = cx + ox, y = cy + oy;
                if (!interior(x, y)) continue;
                const float dist = std::sqrt(static_cast<float>(ox * ox + oy * oy));
                if (dist <= r) dens[ix(x, y)] += (1.f - dist / r) * 2.f;
                vx[ix(x, y)] += dx * 5.f;
                vy[ix(x, y)] += dy * 5.f;
            }
    }
};

// ── model ────────────────────────────────────────────────────────────────────

struct Model {
    Fluid fluid;
    bool down = false;              // the button is held
    int px = -1, py = -1;           // the pointer, in grid pixels
    int lx = -1, ly = -1;           // where it was at the last Tick (for the drag)
    bool moving = false;            // dye still visible: keep ticking
    bool paused = false;
    int palette = 0;
    float visc = 0.0001f;
};

// ── messages ─────────────────────────────────────────────────────────────────

struct Tick {};
struct Resize     { int cols, rows; };
struct Press      { int x, y; };
struct Drag       { int x, y; };
struct Release    {};
struct SetPalette { int p; };
struct Viscosity  { float by; };
struct Pause      {};
struct Reset      {};
struct Quit       {};
using Msg = std::variant<Tick, Resize, Press, Drag, Release, SetPalette, Viscosity, Pause, Reset, Quit>;

// ── program ──────────────────────────────────────────────────────────────────

struct FluidSim {
    using Model = ::Model;
    using Msg   = ::Msg;
    using Cmd   = jaal::Cmd<Msg>;
    using Sub   = jaal::Sub<Msg, on_key, on_mouse, on_resize>;

    static Cmd update(Model& m, Resize r)     { m.fluid.resize(r.cols, (r.rows - 1) * 2); m.moving = false; return {}; }
    static Cmd update(Model& m, Press p)      { m.down = true; m.px = m.lx = p.x; m.py = m.ly = p.y; return {}; }
    static Cmd update(Model& m, Drag d)       { if (m.down) { m.px = d.x; m.py = d.y; } return {}; }
    static Cmd update(Model& m, Release)      { m.down = false; return {}; }
    static Cmd update(Model& m, SetPalette s) { m.palette = s.p; return {}; }
    static Cmd update(Model& m, Viscosity v)  { m.visc = std::clamp(m.visc * v.by, 0.00001f, 0.01f); return {}; }
    static Cmd update(Model& m, Pause)        { m.paused = !m.paused; return {}; }
    static Cmd update(Model& m, Reset)        { m.fluid.clear(); m.moving = false; return {}; }
    static Cmd update(Model&, Quit)           { return Cmd::quit(0); }

    static Cmd update(Model& m, Tick) {
        if (m.down) {
            m.fluid.inject(m.px, m.py, static_cast<float>(m.px - m.lx), static_cast<float>(m.py - m.ly));
            m.lx = m.px; m.ly = m.py;
        }
        m.fluid.step(m.visc, 0.0001f);
        m.moving = m.fluid.visible();
        if (!m.moving) m.fluid.clear();      // settle to exact zero: no invisible drift
        return {};
    }

    // ── view ────────────────────────────────────────────────────────────────

    static Image render(const Model& m) {
        const Fluid& f = m.fluid;
        const auto& lut = kLut[static_cast<std::size_t>(m.palette)];
        Image img(f.n, f.m);
        for (int y = 0; y < f.m; ++y)
            for (int x = 0; x < f.n; ++x) {
                const float t = std::clamp(f.dens[static_cast<std::size_t>(f.ix(x, y))] / kMaxDens, 0.f, 1.f);
                img(x, y) = lut[static_cast<std::size_t>(t * (kSteps - 1))];
            }
        return img;
    }

    static Element status_bar(const Model& m) {
        const auto fg = Color::rgb(180, 180, 180);
        return h(text(" FLUID │ drag=add │ [1-5] palette │ [r] reset │ [+/-] visc │ [spc] pause │ [q] quit") | fgc(fg),
                 text(m.paused ? "  [paused]" : "") | fgc(Color::rgb(255, 200, 80)), spacer(),
                 text(std::string(kPalettes[static_cast<std::size_t>(m.palette)].name) + " ") | fgc(fg) | Bold)
               | bgc(Color::rgb(30, 30, 30));
    }

    static Element view(const Model& m) {
        if (m.fluid.dens.empty()) return text("");
        return v(pixels(render(m)), status_bar(m));
    }

    static Sub subscribe(const Model& m) {
        auto mouse = Sub::on(on_mouse{}, [](const MouseEvent& e) -> std::optional<Msg> {
            if (e.button != MouseButton::Left && e.kind != MouseEventKind::Move) return std::nullopt;
            const int x = e.x.value - 1, y = (e.y.value - 1) * 2;     // 1-based cells -> grid pixels
            switch (e.kind) {
                case MouseEventKind::Press:   return Press{x, y};
                case MouseEventKind::Move:    return Drag{x, y};
                case MouseEventKind::Release: return Release{};
            }
            return std::nullopt;
        });
        auto rest = Sub::batch(
            std::move(mouse),
            Sub::on(on_resize{}, [](const ResizeEvent& r) -> std::optional<Msg> {
                return Resize{r.width.value, r.height.value};
            }),
            jaal_key_map<Sub>({
                {'1', SetPalette{0}}, {'2', SetPalette{1}}, {'3', SetPalette{2}},
                {'4', SetPalette{3}}, {'5', SetPalette{4}},
                {'+', Viscosity{2.f}}, {'=', Viscosity{2.f}}, {'-', Viscosity{0.5f}},
                {' ', Pause{}}, {'r', Reset{}}, {'q', Quit{}}, {SpecialKey::Escape, Quit{}},
            }));
        if (m.paused || !(m.down || m.moving)) return rest;      // nothing moving: no clock
        return Sub::batch(Sub::every(33ms, Tick{}), std::move(rest));
    }
    static bool subs_key(const Model& m) { return !m.paused && (m.down || m.moving); }
};

static_assert(JaalView<FluidSim>);

}  // namespace

int main() { return run_jaal<FluidSim>({.title = "fluid", .mouse = true}); }
