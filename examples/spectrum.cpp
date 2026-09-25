// examples/spectrum.cpp — a simulated audio spectrum analyser.
//
// Five synthetic "tracks" (banks of amplitude-modulated oscillators) feed 64
// frequency bins, drawn four ways: bars with falling peaks, mirrored bars, a
// radial burst, and a scrolling waterfall. Beats (bass spikes over their
// running average) flash the background.
//
//   Model      the bins (target, smoothed, peaks), the waterfall history,
//              beat state, the mode, the track, the clock, the size, the RNG.
//   update()   Tick synthesises the next spectrum and eases the display
//              toward it; keys switch mode and track.
//   view()     the chosen mode drawn into an Image (half blocks: two pixel
//              rows per cell), and a status bar with a VU meter.
//
// Keys: 1-4 mode   space next track   q quit

#include <maya/host/run.hpp>
#include <maya/element/pixels.hpp>
#include <maya/maya.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <variant>
#include <vector>

using namespace maya;
using namespace maya::dsl;
using namespace std::chrono_literals;

namespace {

constexpr int   NUM_BARS      = 64;
constexpr int   NUM_WATERFALL = 128;    // rows of waterfall history
constexpr float PEAK_DECAY    = 0.012f;
constexpr float BAR_SMOOTH    = 0.25f;  // ease-out per frame
constexpr float PI            = 3.14159265358979f;
constexpr float TWO_PI        = 6.28318530717959f;
constexpr float kDt           = 1.f / 60.f;

constexpr std::array<const char*, 4> kModeNames  = {"BARS", "MIRROR", "CIRCULAR", "WATERFALL"};
constexpr std::array<const char*, 5> kTrackNames = {"EDM", "AMBIENT", "ROCK", "SYNTHWAVE", "GLITCH"};

// ── palettes ─────────────────────────────────────────────────────────────────

Rgb lerp_rgb(float s, Rgb a, Rgb b) {
    auto l = [s](std::uint8_t x, std::uint8_t y) { return static_cast<std::uint8_t>(x + (y - x) * std::clamp(s, 0.f, 1.f)); };
    return {l(a.r, b.r), l(a.g, b.g), l(a.b, b.b)};
}

// blue -> cyan -> green -> yellow -> red
Rgb gradient(float t) {
    t = std::clamp(t, 0.f, 1.f);
    if (t < 0.25f) return lerp_rgb(t / 0.25f, {0, 0, 200}, {0, 200, 255});
    if (t < 0.50f) return lerp_rgb((t - 0.25f) / 0.25f, {0, 200, 255}, {0, 255, 0});
    if (t < 0.75f) return lerp_rgb((t - 0.50f) / 0.25f, {0, 255, 0}, {255, 225, 0});
    return lerp_rgb((t - 0.75f) / 0.25f, {255, 225, 0}, {255, 0, 0});
}

// black -> blue -> purple -> red -> yellow -> white
Rgb waterfall_color(float t) {
    t = std::clamp(t, 0.f, 1.f);
    if (t < 0.2f) return lerp_rgb(t / 0.2f, {0, 0, 0}, {0, 0, 180});
    if (t < 0.4f) return lerp_rgb((t - 0.2f) / 0.2f, {0, 0, 180}, {140, 0, 255});
    if (t < 0.6f) return lerp_rgb((t - 0.4f) / 0.2f, {140, 0, 255}, {255, 40, 0});
    if (t < 0.8f) return lerp_rgb((t - 0.6f) / 0.2f, {255, 40, 0}, {255, 255, 0});
    return lerp_rgb((t - 0.8f) / 0.2f, {255, 255, 0}, {255, 255, 255});
}

// The colour ramps as 64-step tables: the view is lookups, and the renderer
// sees a few dozen colours, not thousands.
constexpr int kSteps = 64;
template <Rgb (*F)(float)>
const std::array<Rgb, kSteps>& lut() {
    static const auto t = [] {
        std::array<Rgb, kSteps> a{};
        for (int i = 0; i < kSteps; ++i) a[static_cast<std::size_t>(i)] = F(static_cast<float>(i) / (kSteps - 1));
        return a;
    }();
    return t;
}
Rgb grad(float t)  { return lut<gradient>()[static_cast<std::size_t>(std::clamp(static_cast<int>(t * (kSteps - 1)), 0, kSteps - 1))]; }
Rgb water(float t) { return lut<waterfall_color>()[static_cast<std::size_t>(std::clamp(static_cast<int>(t * (kSteps - 1)), 0, kSteps - 1))]; }

constexpr Rgb kBlack   = {10, 10, 15};
constexpr Rgb kBeatBg  = {25, 10, 20};

// ── tracks ───────────────────────────────────────────────────────────────────

struct TrackDef {
    struct Osc {
        float freq;      // bin position (0..64)
        float amp;
        float phase;
        float mod_freq;  // amplitude modulation Hz
        float mod_depth; // 0..1
    };
    std::vector<Osc> oscillators;
    float bass_freq;
    float bass_amp;
    float bass_mod;
};

const std::vector<TrackDef>& tracks() {
    static const std::vector<TrackDef> t = [] {
        std::vector<TrackDef> t;
        // Track 0: EDM-like with strong bass kick
        t.push_back({{
            {2.0f,  0.9f, 0.0f, 0.5f,  0.8f},   // deep bass pulse
            {4.0f,  0.7f, 0.3f, 1.0f,  0.5f},   // sub bass
            {8.0f,  0.5f, 1.0f, 2.0f,  0.6f},   // low mid
            {16.0f, 0.4f, 0.5f, 3.0f,  0.4f},   // mid
            {24.0f, 0.3f, 0.8f, 4.5f,  0.5f},   // upper mid
            {32.0f, 0.25f, 1.2f, 6.0f, 0.3f},   // presence
            {48.0f, 0.15f, 0.2f, 8.0f, 0.7f},   // high
        }, 2.0f, 0.9f, 0.5f});

        // Track 1: Ambient / pad
        t.push_back({{
            {1.5f,  0.4f, 0.0f, 0.1f, 0.3f},
            {3.0f,  0.5f, 0.7f, 0.15f, 0.4f},
            {6.0f,  0.6f, 1.4f, 0.2f, 0.5f},
            {12.0f, 0.7f, 0.3f, 0.25f, 0.3f},
            {20.0f, 0.5f, 2.0f, 0.3f, 0.4f},
            {30.0f, 0.3f, 1.1f, 0.4f, 0.5f},
            {45.0f, 0.2f, 0.5f, 0.5f, 0.6f},
        }, 1.5f, 0.4f, 0.1f});

        // Track 2: Rock / drums
        t.push_back({{
            {2.5f,  0.8f, 0.0f, 2.0f, 0.9f},
            {5.0f,  0.6f, 0.5f, 2.0f, 0.7f},
            {10.0f, 0.7f, 1.0f, 4.0f, 0.5f},
            {15.0f, 0.5f, 0.3f, 3.0f, 0.6f},
            {22.0f, 0.6f, 0.8f, 5.0f, 0.4f},
            {35.0f, 0.4f, 1.5f, 7.0f, 0.5f},
            {50.0f, 0.3f, 0.2f, 9.0f, 0.3f},
        }, 2.5f, 0.8f, 2.0f});

        // Track 3: Synthwave
        t.push_back({{
            {1.8f,  0.6f, 0.0f, 0.8f, 0.6f},
            {3.6f,  0.5f, 1.0f, 1.2f, 0.5f},
            {7.2f,  0.7f, 0.5f, 1.6f, 0.7f},
            {14.0f, 0.8f, 1.5f, 2.4f, 0.4f},
            {21.0f, 0.6f, 0.3f, 3.2f, 0.6f},
            {28.0f, 0.5f, 0.8f, 4.0f, 0.5f},
            {42.0f, 0.35f, 1.2f, 5.5f, 0.4f},
        }, 1.8f, 0.6f, 0.8f});

        // Track 4: Glitch / IDM
        t.push_back({{
            {3.0f,  0.7f, 0.0f, 3.0f,  0.9f},
            {7.0f,  0.5f, 0.4f, 5.0f,  0.8f},
            {11.0f, 0.6f, 0.9f, 7.0f,  0.7f},
            {17.0f, 0.5f, 1.3f, 11.0f, 0.6f},
            {23.0f, 0.4f, 0.2f, 13.0f, 0.8f},
            {37.0f, 0.3f, 0.7f, 17.0f, 0.5f},
            {53.0f, 0.2f, 1.1f, 19.0f, 0.7f},
        }, 3.0f, 0.7f, 3.0f});
        return t;
    }();
    return t;
}

// ── model ────────────────────────────────────────────────────────────────────

struct Model {
    int w = 0, h = 0;                              // drawing area, in pixels (h = 2 * rows)
    int mode = 0;                                  // 0 bars, 1 mirror, 2 circular, 3 waterfall
    int track = 0;
    float time = 0.f;
    std::array<float, NUM_BARS> spectrum{};        // this frame's target values [0..1]
    std::array<float, NUM_BARS> display{};         // eased toward spectrum
    std::array<float, NUM_BARS> peaks{};           // peak hold
    std::array<float, NUM_BARS> peak_vel{};
    std::vector<std::array<float, NUM_BARS>> waterfall;   // oldest first
    float bass_avg = 0.f;
    int beat_flash = 0;                            // frames of background flash left
    std::mt19937 rng{42};
};

void step(Model& m) {
    m.time += kDt;
    std::uniform_real_distribution<float> noise(-0.02f, 0.02f);
    const TrackDef& track = tracks()[static_cast<std::size_t>(m.track)];

    for (int i = 0; i < NUM_BARS; ++i) {
        const float freq_pos = static_cast<float>(i) / NUM_BARS;
        float val = 0.f;
        for (const auto& osc : track.oscillators) {
            const float osc_pos = osc.freq / 64.f;
            const float dist = std::abs(freq_pos - osc_pos);
            const float spread = 0.08f + osc_pos * 0.05f;              // wider at the top
            const float influence = std::exp(-dist * dist / (2.f * spread * spread));
            const float mod = 1.f - osc.mod_depth * (0.5f + 0.5f * std::sin(TWO_PI * osc.mod_freq * m.time + osc.phase));
            val += osc.amp * mod * influence;
        }
        val += 0.15f * std::sin(TWO_PI * (3.f + freq_pos * 20.f) * m.time * 0.1f) * (1.f - freq_pos);  // harmonics, low end
        val += noise(m.rng);
        m.spectrum[static_cast<std::size_t>(i)] = std::clamp(val, 0.f, 1.f);
    }

    // Beat: the bass eighth spiking over its running average.
    float bass = 0.f;
    for (int i = 0; i < NUM_BARS / 8; ++i) bass += m.spectrum[static_cast<std::size_t>(i)];
    bass /= NUM_BARS / 8;
    m.bass_avg = m.bass_avg * 0.95f + bass * 0.05f;
    if (bass > m.bass_avg * 1.4f && bass > 0.4f) m.beat_flash = 6;
    else if (m.beat_flash > 0) --m.beat_flash;

    for (std::size_t i = 0; i < NUM_BARS; ++i) {
        const float target = m.spectrum[i];
        m.display[i] += (target - m.display[i]) * (target > m.display[i] ? 0.4f : BAR_SMOOTH);  // fast rise, slow fall
        if (m.display[i] > m.peaks[i]) {
            m.peaks[i] = m.display[i];
            m.peak_vel[i] = 0.f;
        } else {
            m.peak_vel[i] += PEAK_DECAY * 0.5f;
            m.peaks[i] = std::max(0.f, m.peaks[i] - m.peak_vel[i]);
        }
    }

    m.waterfall.push_back(m.display);
    if (static_cast<int>(m.waterfall.size()) > NUM_WATERFALL) m.waterfall.erase(m.waterfall.begin());
}

// ── drawing ──────────────────────────────────────────────────────────────────

void fill_rect(Image& img, int x0, int y0, int x1, int y1, Rgb c) {
    x0 = std::max(x0, 0); y0 = std::max(y0, 0);
    x1 = std::min(x1, img.width()); y1 = std::min(y1, img.height());
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) img(x, y) = c;
}

struct BarLayout { int count, width, draw; };
BarLayout bar_layout(int w) {
    const int count = std::min(NUM_BARS, w / 2);
    const int width = count > 0 ? w / count : 0;
    return {count, width, width - (width > 2 ? 1 : 0)};
}

void draw_bars(const Model& m, Image& img) {
    const auto [count, width, draw] = bar_layout(img.width());
    const int H = img.height();
    for (int i = 0; i < count; ++i) {
        const int x0 = i * width;
        const int bar_h = static_cast<int>(m.display[static_cast<std::size_t>(i)] * static_cast<float>(H));
        for (int j = 0; j < bar_h && j < H; ++j)
            fill_rect(img, x0, H - 1 - j, x0 + draw, H - j, grad(static_cast<float>(j) / static_cast<float>(H)));
        const int peak = static_cast<int>(m.peaks[static_cast<std::size_t>(i)] * static_cast<float>(H));
        if (peak > 0 && peak < H)
            fill_rect(img, x0, H - 1 - peak, x0 + draw, H - peak, grad(static_cast<float>(peak) / static_cast<float>(H)));
    }
}

void draw_mirror(const Model& m, Image& img) {
    const auto [count, width, draw] = bar_layout(img.width());
    const int mid = img.height() / 2;
    for (int i = 0; i < count; ++i) {
        const int x0 = i * width;
        const int half = static_cast<int>(m.display[static_cast<std::size_t>(i)] * static_cast<float>(mid));
        for (int j = 0; j < half && j < mid; ++j) {
            const Rgb c = grad(static_cast<float>(j) / static_cast<float>(mid));
            fill_rect(img, x0, mid - 1 - j, x0 + draw, mid - j, c);
            fill_rect(img, x0, mid + j, x0 + draw, mid + j + 1, c);
        }
        const int peak = static_cast<int>(m.peaks[static_cast<std::size_t>(i)] * static_cast<float>(mid));
        if (peak > 0 && peak < mid) {
            const Rgb c = grad(static_cast<float>(peak) / static_cast<float>(mid));
            fill_rect(img, x0, mid - 1 - peak, x0 + draw, mid - peak, c);
            fill_rect(img, x0, mid + peak, x0 + draw, mid + peak + 1, c);
        }
    }
}

void draw_circular(const Model& m, Image& img) {
    const int W = img.width(), H = img.height();
    const float cx = static_cast<float>(W) / 2.f, cy = static_cast<float>(H) / 2.f;
    const float max_r = std::min(cx, cy) * 0.85f, inner_r = max_r * 0.3f;
    std::vector<float> glow(static_cast<std::size_t>(W * H), 0.f);
    auto plot = [&](int x, int y, float t) {
        if (x < 0 || x >= W || y < 0 || y >= H) return;
        float& g = glow[static_cast<std::size_t>(y * W + x)];
        g = std::max(g, t);
    };
    for (int i = 0; i < NUM_BARS; ++i) {
        const float a0 = TWO_PI * static_cast<float>(i) / NUM_BARS - PI / 2.f;
        const float a1 = TWO_PI * static_cast<float>(i + 1) / NUM_BARS - PI / 2.f;
        const float len = m.display[static_cast<std::size_t>(i)] * (max_r - inner_r);
        for (int s = 0; static_cast<float>(s) <= len; ++s) {
            const float r = inner_r + static_cast<float>(s);
            const int arc = std::max(2, static_cast<int>((a1 - a0) * r));
            for (int a = 0; a < arc; ++a) {
                const float ang = a0 + (a1 - a0) * static_cast<float>(a) / static_cast<float>(arc);
                plot(static_cast<int>(cx + r * std::cos(ang)), static_cast<int>(cy + r * std::sin(ang)),
                     static_cast<float>(s) / (max_r - inner_r));
            }
        }
        const float pr = inner_r + m.peaks[static_cast<std::size_t>(i)] * (max_r - inner_r);
        const float mid_a = (a0 + a1) * 0.5f;
        const int px = static_cast<int>(cx + pr * std::cos(mid_a)), py = static_cast<int>(cy + pr * std::sin(mid_a));
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) plot(px + dx, py + dy, 0.95f);
    }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (const float g = glow[static_cast<std::size_t>(y * W + x)]; g > 0.01f) img(x, y) = grad(g);
}

void draw_waterfall(const Model& m, Image& img) {
    const int W = img.width(), H = img.height();
    const int bars_h = H / 4, wf_h = H - bars_h;
    const int cols = std::min(NUM_BARS, W);
    const float col_w = static_cast<float>(W) / static_cast<float>(cols);
    auto col_x = [col_w](int i) { return static_cast<int>(static_cast<float>(i) * col_w); };

    for (int i = 0; i < cols; ++i) {                                   // mini bars on top
        const int bar_h = static_cast<int>(m.display[static_cast<std::size_t>(i)] * static_cast<float>(bars_h));
        for (int j = 0; j < bar_h && j < bars_h; ++j)
            fill_rect(img, col_x(i), bars_h - 1 - j, col_x(i + 1), bars_h - j, grad(static_cast<float>(j) / static_cast<float>(bars_h)));
    }
    const int rows = static_cast<int>(m.waterfall.size());           // history below, newest at the bottom
    const int shown = std::min(wf_h, rows);
    for (int r = 0; r < shown; ++r) {
        const auto& data = m.waterfall[static_cast<std::size_t>(rows - shown + r)];
        const int y = bars_h + (wf_h - shown) + r;
        for (int i = 0; i < cols; ++i)
            fill_rect(img, col_x(i), y, col_x(i + 1), y + 1, water(data[static_cast<std::size_t>(i)]));
    }
}

// ── program ──────────────────────────────────────────────────────────────────

struct Tick {};
struct Resize    { int cols, rows; };
struct SetMode   { int mode; };
struct NextTrack {};
struct Quit {};
using Msg = std::variant<Tick, Resize, SetMode, NextTrack, Quit>;

struct Spectrum {
    using Model = ::Model;
    using Msg   = ::Msg;
    using Cmd   = jaal::Cmd<Msg>;
    using Sub   = jaal::Sub<Msg, on_key, on_resize>;

    static Cmd update(Model& m, Tick)      { step(m); return {}; }
    static Cmd update(Model& m, Resize r)  { m.w = std::max(1, r.cols); m.h = std::max(2, (r.rows - 1) * 2); return {}; }
    static Cmd update(Model& m, SetMode s) { m.mode = s.mode; return {}; }
    static Cmd update(Model&, Quit)        { return Cmd::quit(0); }
    static Cmd update(Model& m, NextTrack) {
        m.track = (m.track + 1) % static_cast<int>(tracks().size());
        m.peaks.fill(0.f);                     // a new track starts its peaks from the floor
        m.peak_vel.fill(0.f);
        return {};
    }

    static Image render(const Model& m) {
        Image img(m.w, m.h);
        img.fill(m.beat_flash > 0 && m.mode != 3 ? kBeatBg : kBlack);
        switch (m.mode) {
            case 0: draw_bars(m, img); break;
            case 1: draw_mirror(m, img); break;
            case 2: draw_circular(m, img); break;
            default: draw_waterfall(m, img); break;
        }
        return img;
    }

    static Element status_bar(const Model& m) {
        float vu = 0.f;
        for (float d : m.display) vu += d;
        vu /= NUM_BARS;
        const int vu_width = std::clamp(m.w / 5, 1, 20);
        const int fill = static_cast<int>(vu * static_cast<float>(vu_width));
        const bool beat = m.beat_flash > 0;
        const auto bg = beat ? Color::rgb(60, 15, 25) : Color::rgb(15, 15, 25);
        return h(text(std::string(" ") + kModeNames[static_cast<std::size_t>(m.mode)] + "  Track: " +
                      kTrackNames[static_cast<std::size_t>(m.track)] + "  VU [" +
                      std::string(static_cast<std::size_t>(fill), '|') +
                      std::string(static_cast<std::size_t>(vu_width - fill), ' ') + "]")
                     | fgc(beat ? Color::rgb(255, 100, 100) : Color::rgb(80, 200, 255)) | Bold,
                 spacer(),
                 text("[1-4] mode  [space] track  [q] quit ") | fgc(Color::rgb(60, 60, 80))) | bgc(bg);
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
            keys<Sub>({
                {'1', SetMode{0}}, {'2', SetMode{1}}, {'3', SetMode{2}}, {'4', SetMode{3}},
                {' ', NextTrack{}}, {'q', Quit{}}, {SpecialKey::Escape, Quit{}},
            }));
    }
    static bool subs_key(const Model&) { return true; }
};

static_assert(Program<Spectrum>);

}  // namespace

int main() { return run<Spectrum>({.title = "spectrum"}); }
