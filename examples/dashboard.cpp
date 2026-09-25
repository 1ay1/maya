// maya — NEXUS: Cyberpunk Mission Control Dashboard
//
// A flashy animated data visualization dashboard with:
//   - Dual waveform oscilloscope with dense fill, grid, and amplitude readout
//   - Spectrum analyzer with reflection, peak hold, frequency labels
//   - Rotating radar sweep with smooth braille trails, range rings, blips
//   - Live hex data waterfall with address column and byte-value heatmap
//   - System gauges with sparkline history and threshold markers
//   - Network throughput with braille area fill and dual RX/TX lines
//   - Animated spirograph with interpolated braille curves and color cycling
//   - Particle engine with gravity and trails
//
// Keys: q/Esc=quit  1-4=theme  space=pause  +/-=speed
//       p=particles  g=glitch  w=waveform mode  r=reset
//
//   Model      every signal the panels show (spectrum, radar blips, hex
//              stream, network history, gauges, spirograph trail,
//              particles), the theme, speed, pause, and the RNG.
//   update()   Tick advances every simulation; keys are messages.
//   view()     the panels drawn into a Glyphs grid through a small
//              cell-drawing Surface (braille plots, boxes, text) with
//              colours from the theme; a glitch is a pure function of the
//              frame number, so view() stays pure.

#include <maya/app.hpp>
#include <maya/element/pixels.hpp>
#include <maya/maya.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <array>
#include <chrono>
#include <cstring>
#include <optional>
#include <variant>
#include <vector>

using namespace maya;
using namespace std::chrono_literals;

namespace {

static constexpr float PI = 3.14159265f;
static constexpr float TAU = 6.28318530f;

// ── Themes ────────────────────────────────────────────────────────────────────

struct Palette {
    const char* name;
    uint8_t accent_r, accent_g, accent_b;
    uint8_t dim_r, dim_g, dim_b;
    uint8_t bg_r, bg_g, bg_b;
    uint8_t hot_r, hot_g, hot_b;
    uint8_t cold_r, cold_g, cold_b;
};

static constexpr Palette g_themes[] = {
    {"CYBER",    0,255,200,   0,80,60,    5,10,15,   255,0,100,   0,100,255},
    {"NEON",     255,0,255,   80,0,80,    10,5,15,   255,255,0,   0,200,255},
    {"EMBER",    255,120,0,   80,40,0,    15,8,5,    255,40,40,   255,200,60},
    {"ARCTIC",   100,200,255, 30,60,80,   5,10,18,   255,255,255, 0,150,200},
};

// ── Style IDs (interned in on_resize) ─────────────────────────────────────────

// A cell style: what a pre-interned style id used to name.
struct Sty { Rgb fg{200, 200, 200}; bool bold = false; };

// Every colour the panels use, derived from the theme (built per frame: a
// few hundred lerps, nothing next to the drawing).
struct Styles {
    Rgb bg;
    Sty DIM, VDIM, ACCENT, BRIGHT, HOT, COLD, BORDER, TITLE, LABEL, DATA, GRID, HEX_ADDR, NET_LINE, NET_LINE2;
    std::array<Sty, 8> BAR, WAVE_FILL, HEX_HEAT, BRAILLE, NET_FILL;
    std::array<Sty, 10> RADAR;
    std::array<Sty, 4> WAVE;
    std::array<Sty, 5> PARTICLE;
    std::array<Sty, 12> SPECTRUM;
    std::array<Sty, 6> SPEC_REFL;
};

// The drawing surface: a Glyphs grid with the Canvas-style calls the
// panels are written against.
struct Cell { char32_t character; };
struct Surface {
    Glyphs& g;
    Rgb bg;
    void set(int x, int y, char32_t ch, Sty s) {
        if (g.in(x, y)) g(x, y) = {ch, s.fg, bg, s.bold};
    }
    void write_text(int x, int y, const char* str, Sty s) {
        for (int i = 0; str[i]; ++i) set(x + i, y, static_cast<unsigned char>(str[i]), s);
    }
    [[nodiscard]] Cell get(int x, int y) const { return {g.in(x, y) ? g(x, y).ch : U' '}; }
};
using Canvas = Surface;   // the panels below were written against maya's Canvas

// ── Model ─────────────────────────────────────────────────────────────────────

struct Blip { float angle, dist, life, max_life; };
struct Particle { float x, y, vx, vy, life, max_life; uint8_t r, g, b; };
struct HexLine { std::vector<uint8_t> bytes; uint32_t addr; };
struct SpiroPoint { float x, y; int pattern; };

struct Model {
    int w = 0, h = 0;                  // terminal cells
    std::mt19937 rng{42};
    int theme = 0;
    float time = 0.f;
    float speed = 1.f;
    bool paused = false;
    int frame = 0;
    int wave_mode = 0;

    float spectrum[64] = {};
    float spectrum_peak[64] = {};
    float spectrum_vel[64] = {};

    float radar_angle = 0.f;
    std::vector<Blip> blips;
    float blip_timer = 0.f;

    std::vector<Particle> particles;
    float particle_timer = 0.f;        // until the next automatic burst

    std::vector<HexLine> hex_lines;
    float hex_timer = 0.f;
    uint32_t hex_addr = 0x0040'0000;

    float net_data[120] = {};
    float net_data2[120] = {};         // second channel (TX)
    int net_idx = 0;
    float net_timer = 0.f;

    float spiro_t = 0.f;
    std::vector<SpiroPoint> spiro_trail;

    float gauges[6] = {0.45f, 0.72f, 0.33f, 0.88f, 0.56f, 0.21f};
    float gauge_targets[6] = {0.65f, 0.80f, 0.40f, 0.75f, 0.60f, 0.30f};
    float gauge_history[6][60] = {};
    int gauge_hist_idx = 0;
    float gauge_hist_timer = 0.f;

    float glitch_timer = 0.f;
};

// ── Helpers ───────────────────────────────────────────────────────────────────

static uint8_t lerp8(uint8_t a, uint8_t b, float t) {
    t = std::clamp(t, 0.f, 1.f);
    return static_cast<uint8_t>(a + (b - a) * t);
}

static void draw_box(Canvas& c, int x, int y, int w, int h, Sty style,
                     const char* title = nullptr, Sty title_style = {}) {
    if (w < 2 || h < 2) return;
    c.set(x, y, U'╭', style);
    c.set(x + w - 1, y, U'╮', style);
    c.set(x, y + h - 1, U'╰', style);
    c.set(x + w - 1, y + h - 1, U'╯', style);
    for (int i = 1; i < w - 1; ++i) {
        c.set(x + i, y, U'─', style);
        c.set(x + i, y + h - 1, U'─', style);
    }
    for (int j = 1; j < h - 1; ++j) {
        c.set(x, y + j, U'│', style);
        c.set(x + w - 1, y + j, U'│', style);
    }
    if (title) {
        int len = static_cast<int>(std::strlen(title));
        int tx = x + 2;
        c.set(tx - 1, y, U'┤', style);
        c.write_text(tx, y, title, title_style);
        c.set(tx + len, y, U'├', style);
    }
}

// ── Braille helpers ───────────────────────────────────────────────────────────

static constexpr int braille_dot[8] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};

static char32_t braille_char(uint8_t dots) {
    return U'\u2800' + dots;
}

static void braille_plot(Canvas& c, int ox, int oy, int bw, int bh,
                         int px, int py, Sty style) {
    int bx = px / 2, by = py / 4;
    if (bx < 0 || bx >= bw || by < 0 || by >= bh) return;
    int dcol = px % 2, drow = py % 4;
    int dot_idx = (drow < 3) ? (drow + dcol * 3) : (6 + dcol);
    auto existing = c.get(ox + bx, oy + by);
    uint8_t dots = (existing.character >= U'\u2800' && existing.character <= U'\u28FF')
                 ? static_cast<uint8_t>(existing.character - U'\u2800') : 0;
    dots |= braille_dot[dot_idx];
    c.set(ox + bx, oy + by, braille_char(dots), style);
}

// Draw braille line between two points with interpolation
static void braille_line(Canvas& c, int ox, int oy, int bw, int bh,
                         int x0, int y0, int x1, int y1, Sty style) {
    int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
    int steps = std::max(dx, dy);
    if (steps == 0) { braille_plot(c, ox, oy, bw, bh, x0, y0, style); return; }
    for (int i = 0; i <= steps; ++i) {
        int px = x0 + (x1 - x0) * i / steps;
        int py = y0 + (y1 - y0) * i / steps;
        braille_plot(c, ox, oy, bw, bh, px, py, style);
    }
}

static const char32_t g_vblocks[] = {U' ', U'▁', U'▂', U'▃', U'▄', U'▅', U'▆', U'▇', U'█'};

// ── Tick ──────────────────────────────────────────────────────────────────────

static void tick(Model& m, float dt) {
    if (m.paused) return;
    float t = dt * m.speed;
    m.time += t;
    m.frame++;

    // Spectrum — 64 bands with musical envelope
    for (int i = 0; i < 64; ++i) {
        float freq = 1.2f + i * 0.35f;
        float target = 0.15f + 0.85f * (0.5f + 0.5f * std::sin(m.time * freq + i * 0.5f));
        float envelope = 1.f - static_cast<float>(i) / 80.f;
        envelope *= 0.6f + 0.4f * std::sin(m.time * 0.4f + i * 0.08f);
        target *= std::clamp(envelope, 0.f, 1.f);
        float beat = 0.5f + 0.5f * std::sin(m.time * 3.5f);
        target *= 0.7f + 0.3f * beat;
        target += 0.08f * std::sin(m.time * 11.f + i * 1.7f);
        target = std::clamp(target, 0.02f, 1.f);
        m.spectrum[i] += (target - m.spectrum[i]) * 10.f * t;
        if (m.spectrum[i] > m.spectrum_peak[i]) {
            m.spectrum_peak[i] = m.spectrum[i];
            m.spectrum_vel[i] = 0.f;
        } else {
            m.spectrum_vel[i] += 1.5f * t;
            m.spectrum_peak[i] -= m.spectrum_vel[i] * t;
            if (m.spectrum_peak[i] < m.spectrum[i]) m.spectrum_peak[i] = m.spectrum[i];
        }
    }

    // Radar
    m.radar_angle += 2.0f * t;
    if (m.radar_angle > TAU) m.radar_angle -= TAU;
    m.blip_timer += t;
    if (m.blip_timer > 0.6f) {
        m.blip_timer = 0.f;
        float a = std::uniform_real_distribution<float>(0.f, TAU)(m.rng);
        float d = std::uniform_real_distribution<float>(0.15f, 0.92f)(m.rng);
        float life = std::uniform_real_distribution<float>(3.f, 6.f)(m.rng);
        m.blips.push_back({a, d, life, life});
    }
    for (auto& b : m.blips) b.life -= t;
    std::erase_if(m.blips, [](const Blip& b) { return b.life <= 0.f; });

    // Particles
    for (auto& p : m.particles) {
        p.vy += 12.f * t;
        p.x += p.vx * t;
        p.y += p.vy * t;
        p.life -= t;
    }
    std::erase_if(m.particles, [](const Particle& p) { return p.life <= 0.f; });

    // Hex waterfall
    m.hex_timer += t;
    if (m.hex_timer > 0.05f) {
        m.hex_timer = 0.f;
        HexLine line;
        line.addr = m.hex_addr;
        m.hex_addr += 16;
        line.bytes.resize(16);
        for (auto& b : line.bytes)
            b = std::uniform_int_distribution<int>(0, 255)(m.rng);
        m.hex_lines.push_back(std::move(line));
        if (m.hex_lines.size() > 200) m.hex_lines.erase(m.hex_lines.begin());
    }

    // Network pulse — dual channels
    m.net_timer += t;
    if (m.net_timer > 0.08f) {
        m.net_timer = 0.f;
        int idx = m.net_idx % 120;
        float base = 0.35f + 0.25f * std::sin(m.time * 0.8f);
        float spike = 0.3f * std::pow(0.5f + 0.5f * std::sin(m.time * 4.f), 3.f);
        float noise = 0.08f * std::sin(m.time * 17.f) + 0.05f * std::sin(m.time * 31.f);
        m.net_data[idx] = std::clamp(base + spike + noise, 0.02f, 1.f);
        // TX channel: offset and lower
        float tx_base = 0.2f + 0.15f * std::sin(m.time * 1.1f + 1.f);
        float tx_spike = 0.2f * std::pow(0.5f + 0.5f * std::sin(m.time * 3.f + 2.f), 3.f);
        m.net_data2[idx] = std::clamp(tx_base + tx_spike + noise * 0.5f, 0.02f, 0.8f);
        m.net_idx++;
    }

    // Spirograph — dual patterns, more points for density
    m.spiro_t += 3.f * t;
    for (int sub = 0; sub < 3; ++sub) {
        float st = m.spiro_t + sub * 0.15f;
        {
            float R = 5.f, r = 3.f, d = 4.f;
            float sx = (R - r) * std::cos(st) + d * std::cos((R - r) / r * st);
            float sy = (R - r) * std::sin(st) - d * std::sin((R - r) / r * st);
            m.spiro_trail.push_back({sx, sy, 0});
        }
        {
            float R = 4.f, r = 1.5f, d = 3.5f;
            float sx = (R + r) * std::cos(st * 0.7f) - d * std::cos((R + r) / r * st * 0.7f);
            float sy = (R + r) * std::sin(st * 0.7f) - d * std::sin((R + r) / r * st * 0.7f);
            m.spiro_trail.push_back({sx, sy, 1});
        }
    }
    while (m.spiro_trail.size() > 3000)
        m.spiro_trail.erase(m.spiro_trail.begin(), m.spiro_trail.begin() + 6);

    // Gauges drift with history
    m.gauge_hist_timer += t;
    if (m.gauge_hist_timer > 0.2f) {
        m.gauge_hist_timer = 0.f;
        for (int i = 0; i < 6; ++i)
            m.gauge_history[i][m.gauge_hist_idx % 60] = m.gauges[i];
        m.gauge_hist_idx++;
    }
    for (int i = 0; i < 6; ++i) {
        m.gauges[i] += (m.gauge_targets[i] - m.gauges[i]) * 2.5f * t;
        if (std::abs(m.gauges[i] - m.gauge_targets[i]) < 0.02f)
            m.gauge_targets[i] = std::uniform_real_distribution<float>(0.1f, 0.95f)(m.rng);
    }

    if (m.glitch_timer > 0.f) m.glitch_timer -= t;
}

// ── Spawn particles ──────────────────────────────────────────────────────────

static void spawn_burst(Model& m, float cx, float cy, int count) {
    auto& th = g_themes[m.theme];
    for (int i = 0; i < count; ++i) {
        float angle = std::uniform_real_distribution<float>(0.f, TAU)(m.rng);
        float speed = std::uniform_real_distribution<float>(5.f, 25.f)(m.rng);
        float life = std::uniform_real_distribution<float>(1.f, 3.f)(m.rng);
        float blend = std::uniform_real_distribution<float>(0.f, 1.f)(m.rng);
        uint8_t r = lerp8(th.accent_r, th.hot_r, blend);
        uint8_t g = lerp8(th.accent_g, th.hot_g, blend);
        uint8_t b = lerp8(th.accent_b, th.hot_b, blend);
        m.particles.push_back({cx, cy, std::cos(angle) * speed, std::sin(angle) * speed * 0.5f,
                               life, life, r, g, b});
    }
}

// ── Waveform sampling ─────────────────────────────────────────────────────────

static float wave_sample(const Model& m, float t) {
    switch (m.wave_mode) {
        case 0: return 0.8f * std::sin(t * 4.f + m.time * 3.f)
                            * std::cos(t * 1.5f + m.time * 0.7f);
        case 1: return 0.7f * std::sin(t * 3.f + m.time * 2.f)
                     + 0.3f * std::sin(t * 7.f + m.time * 5.f);
        case 2: return 0.6f * std::sin(t * 5.f + m.time * 4.f)
                     + 0.3f * std::sin(t * 13.f + m.time * 7.f)
                     + 0.1f * std::sin(t * 29.f + m.time * 11.f);
        case 3: {
            float phase = std::fmod(t * 2.f + m.time * 1.5f, TAU);
            return std::exp(-phase * 2.f) * std::sin(phase * 8.f) * 0.9f;
        }
        default: return 0.f;
    }
}

// ── Drawing: Waveform Oscilloscope ────────────────────────────────────────────

static void draw_waveform(const Model& m, const Styles& S, Canvas& c, int x, int y, int w, int h) {
    draw_box(c, x, y, w, h, S.BORDER, " WAVEFORM ", S.TITLE);

    int iw = w - 2, ih = h - 2;
    if (iw < 8 || ih < 4) return;
    int ox = x + 1, oy = y + 1;

    int bw = iw, bh = ih;
    int pw = bw * 2, ph = bh * 4;
    int center_py = ph / 2;

    // Grid lines (drawn as dim characters, will be overwritten by braille data)
    for (int gy = 0; gy < ih; ++gy) {
        bool is_center = (gy == ih / 2);
        bool is_quarter = (gy == ih / 4 || gy == ih * 3 / 4);
        if (is_center) {
            for (int gx = 0; gx < iw; ++gx)
                c.set(ox + gx, oy + gy, U'─', S.GRID);
        } else if (is_quarter) {
            for (int gx = 0; gx < iw; gx += 2)
                c.set(ox + gx, oy + gy, U'·', S.GRID);
        }
    }
    for (int gx = 0; gx < iw; gx += std::max(1, iw / 8)) {
        for (int gy = 0; gy < ih; gy += 2)
            c.set(ox + gx, oy + gy, U'·', S.GRID);
    }

    // Channel 1: main wave with dense area fill
    std::vector<int> ch1_py(pw);
    for (int px = 0; px < pw; ++px) {
        float t1 = static_cast<float>(px) / pw * TAU;
        float v1 = wave_sample(m, t1);
        ch1_py[px] = std::clamp(static_cast<int>((0.5f - v1 * 0.45f) * ph), 0, ph - 1);
    }

    // Area fill — every braille dot between wave and center
    for (int px = 0; px < pw; ++px) {
        int py1 = ch1_py[px];
        int top = std::min(py1, center_py);
        int bot = std::max(py1, center_py);
        for (int fill_py = top; fill_py <= bot; ++fill_py) {
            float dist = static_cast<float>(std::abs(fill_py - py1)) / std::max(1, bot - top);
            int fi = std::clamp(static_cast<int>(dist * 7.f), 0, 7);
            braille_plot(c, ox, oy, bw, bh, px, fill_py, S.WAVE_FILL[fi]);
        }
    }

    // Main line (thick: 3 braille rows)
    for (int px = 0; px < pw; ++px) {
        int py1 = ch1_py[px];
        braille_plot(c, ox, oy, bw, bh, px, py1, S.WAVE[0]);
        if (py1 > 0) braille_plot(c, ox, oy, bw, bh, px, py1 - 1, S.WAVE[0]);
        if (py1 < ph - 1) braille_plot(c, ox, oy, bw, bh, px, py1 + 1, S.WAVE[0]);
    }

    // Channel 2: secondary wave with its own thin line
    for (int px = 0; px < pw; ++px) {
        float t2 = static_cast<float>(px) / pw * TAU + 1.5f;
        float v2 = wave_sample(m, t2) * 0.6f;
        int py2 = std::clamp(static_cast<int>((0.5f - v2 * 0.45f) * ph), 0, ph - 1);
        braille_plot(c, ox, oy, bw, bh, px, py2, S.WAVE[2]);
        if (py2 > 0) braille_plot(c, ox, oy, bw, bh, px, py2 - 1, S.WAVE[2]);
    }

    // Y-axis labels
    c.write_text(ox, oy, "+1", S.DIM);
    c.write_text(ox, oy + ih / 2, " 0", S.DIM);
    c.write_text(ox, oy + ih - 1, "-1", S.DIM);

    // Mode + readout in title bar
    const char* modes[] = {"SINE", "LISSAJOUS", "NOISE", "HEARTBEAT"};
    float amp = std::abs(wave_sample(m, m.time * 0.5f));
    char amp_buf[32];
    std::snprintf(amp_buf, sizeof(amp_buf), " %s A:%.2f ", modes[m.wave_mode], amp);
    int label_x = x + w - static_cast<int>(std::strlen(amp_buf)) - 2;
    c.write_text(std::max(ox, label_x), y, amp_buf, S.ACCENT);
}

// ── Drawing: Spectrum Analyzer ────────────────────────────────────────────────

static void draw_spectrum(const Model& m, const Styles& S, Canvas& c, int x, int y, int w, int h) {
    draw_box(c, x, y, w, h, S.BORDER, " SPECTRUM ", S.TITLE);

    int iw = w - 2, ih = h - 2;
    if (iw < 4 || ih < 3) return;
    int ox = x + 1, oy = y + 1;

    // Reflection zone at bottom
    int refl_h = std::max(1, ih / 5);
    int main_h = ih - refl_h;
    if (main_h < 2) { main_h = ih; refl_h = 0; }

    // Scale bars to fill width completely
    int num_bars = std::min(64, iw);

    for (int i = 0; i < num_bars; ++i) {
        float val = m.spectrum[i * 64 / num_bars];
        int bar_h = std::max(1, static_cast<int>(val * main_h));

        int bx_start = ox + (i * iw) / num_bars;
        int bx_end = ox + ((i + 1) * iw) / num_bars;
        int draw_w = bx_end - bx_start;
        // Gap of 1 only if bars are wide enough
        if (draw_w > 2) draw_w--;

        // Main bars — full block gradient
        for (int j = 0; j < main_h; ++j) {
            int cy = oy + main_h - 1 - j;
            if (j < bar_h) {
                float grad = static_cast<float>(j) / main_h;
                int si = std::clamp(static_cast<int>(grad * 11.f), 0, 11);
                // Top row of bar gets a bright cap
                char32_t ch = (j == bar_h - 1) ? U'▀' : U'█';
                Sty sty = (j == bar_h - 1) ? S.SPECTRUM[std::min(si + 2, 11)] : S.SPECTRUM[si];
                for (int k = 0; k < draw_w; ++k) {
                    int cx = bx_start + k;
                    if (cx < ox + iw) c.set(cx, cy, ch, sty);
                }
            }
        }

        // Reflection (inverted, dimmer, uses ▄ blocks)
        for (int j = 0; j < refl_h; ++j) {
            int cy = oy + main_h + j;
            int mirror_j = bar_h - 1 - j * 2;  // fade faster
            if (mirror_j < 0) break;
            float grad = static_cast<float>(mirror_j) / main_h;
            int si = std::clamp(static_cast<int>(grad * 5.f), 0, 5);
            char32_t ch = (j == 0) ? U'▄' : U'░';
            for (int k = 0; k < draw_w; ++k) {
                int cx = bx_start + k;
                if (cx < ox + iw) c.set(cx, cy, ch, S.SPEC_REFL[si]);
            }
        }

        // Peak hold marker
        int peak_idx = i * 64 / num_bars;
        int peak_row = static_cast<int>((1.f - m.spectrum_peak[peak_idx]) * main_h);
        if (peak_row >= 0 && peak_row < main_h) {
            for (int k = 0; k < draw_w; ++k) {
                int cx = bx_start + k;
                if (cx < ox + iw)
                    c.set(cx, oy + peak_row, U'━', S.HOT);
            }
        }
    }

    // Frequency labels on bottom border
    const char* freq_labels[] = {"63", "250", "1K", "4K", "8K", "16K"};
    for (int i = 0; i < 6; ++i) {
        int lx = ox + (i * iw) / 6;
        if (lx + 3 < ox + iw)
            c.write_text(lx, y + h - 1, freq_labels[i], S.DIM);
    }
}

// ── Drawing: Radar ────────────────────────────────────────────────────────────

static void draw_radar(const Model& m, const Styles& S, Canvas& c, int x, int y, int w, int h) {
    draw_box(c, x, y, w, h, S.BORDER, " RADAR ", S.TITLE);

    int iw = w - 2, ih = h - 2;
    if (iw < 8 || ih < 6) return;
    int ox = x + 1, oy = y + 1;

    // Braille coordinate space
    int bw = iw, bh = ih;
    int pw = bw * 2, ph = bh * 4;
    int cpx = pw / 2, cpy = ph / 2;

    // Max radius in braille pixels — use full panel width and height
    float rx = static_cast<float>(pw) / 2.f - 2.f;
    float ry = static_cast<float>(ph) / 2.f - 2.f;

    // Concentric range rings (3 rings, using braille)
    for (int ring = 1; ring <= 3; ++ring) {
        float frac = static_cast<float>(ring) / 3.f;
        int steps = static_cast<int>(std::max(rx, ry) * frac * 4.f);
        for (int s = 0; s < steps; ++s) {
            float a = TAU * s / steps;
            int ppx = cpx + static_cast<int>(std::cos(a) * rx * frac);
            int ppy = cpy + static_cast<int>(std::sin(a) * ry * frac);
            braille_plot(c, ox, oy, bw, bh, ppx, ppy, S.DIM);
        }
    }

    // Cross hairs (braille)
    for (int i = 0; i < pw; ++i) braille_plot(c, ox, oy, bw, bh, i, cpy, S.GRID);
    for (int i = 0; i < ph; ++i) braille_plot(c, ox, oy, bw, bh, cpx, i, S.GRID);

    // Diagonal cross hairs
    for (int i = 0; i < static_cast<int>(std::min(rx, ry)); ++i) {
        braille_plot(c, ox, oy, bw, bh, cpx + i, cpy + static_cast<int>(i * ry / rx), S.GRID);
        braille_plot(c, ox, oy, bw, bh, cpx - i, cpy + static_cast<int>(i * ry / rx), S.GRID);
        braille_plot(c, ox, oy, bw, bh, cpx + i, cpy - static_cast<int>(i * ry / rx), S.GRID);
        braille_plot(c, ox, oy, bw, bh, cpx - i, cpy - static_cast<int>(i * ry / rx), S.GRID);
    }

    // Sweep: filled wedge using braille (10-step trail, wide arc)
    for (int trail = 0; trail < 10; ++trail) {
        float a_center = m.radar_angle - trail * 0.06f;
        int si = std::min(trail, 9);
        // Each trail step is a thin line from center to edge
        int line_steps = static_cast<int>(std::max(rx, ry));
        for (int d = 2; d < line_steps; ++d) {
            float frac = static_cast<float>(d) / line_steps;
            int ppx = cpx + static_cast<int>(std::cos(a_center) * rx * frac);
            int ppy = cpy + static_cast<int>(std::sin(a_center) * ry * frac);
            braille_plot(c, ox, oy, bw, bh, ppx, ppy, S.RADAR[si]);
        }
    }

    // Blips
    for (auto& b : m.blips) {
        float intensity = std::clamp(b.life / b.max_life, 0.f, 1.f);
        int bpx = cpx + static_cast<int>(std::cos(b.angle) * b.dist * rx);
        int bpy = cpy + static_cast<int>(std::sin(b.angle) * b.dist * ry);
        // Draw as a small braille cluster for visibility
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                Sty sty = (dx == 0 && dy == 0) ? S.HOT :
                               (intensity > 0.5f ? S.ACCENT : S.DIM);
                braille_plot(c, ox, oy, bw, bh, bpx + dx, bpy + dy, sty);
            }
        }
    }

    // Cardinal labels
    int cx_r = ox + iw / 2, cy_r = oy + ih / 2;
    c.write_text(cx_r, oy, "N", S.ACCENT);
    c.write_text(cx_r, oy + ih - 1, "S", S.ACCENT);
    c.write_text(ox, cy_r, "W", S.ACCENT);
    c.write_text(ox + iw - 1, cy_r, "E", S.ACCENT);

    // Center reticle
    c.set(cx_r, cy_r, U'+', S.BRIGHT);

    // Blip count in title
    char count_buf[16];
    std::snprintf(count_buf, sizeof(count_buf), " %zuT ", m.blips.size());
    c.write_text(x + w - static_cast<int>(std::strlen(count_buf)) - 2, y, count_buf, S.DATA);
}

// ── Drawing: Hex Waterfall ────────────────────────────────────────────────────

static void draw_hex(const Model& m, const Styles& S, Canvas& c, int x, int y, int w, int h) {
    draw_box(c, x, y, w, h, S.BORDER, " DATA STREAM ", S.TITLE);

    int iw = w - 2, ih = h - 2;
    if (iw < 20 || ih < 2) return;

    int bytes_per_line = 16;
    bool show_ascii = (iw >= 70);

    // Column header
    int header_y = y + 1;
    c.write_text(x + 1, header_y, "ADDRESS ", S.DIM);
    int hx = x + 10;
    for (int i = 0; i < bytes_per_line; ++i) {
        if (hx + 2 >= x + w - 1) break;
        char hdr[4];
        std::snprintf(hdr, sizeof(hdr), "%02X", i);
        c.write_text(hx, header_y, hdr, S.VDIM);
        hx += 3;
        if (i == 7) hx++;
    }
    if (show_ascii) {
        int sep_x = hx + 1;
        if (sep_x < x + w - 2) c.set(sep_x, header_y, U'│', S.GRID);
        c.write_text(sep_x + 2, header_y, "ASCII", S.VDIM);
    }

    // Data rows
    int data_h = ih - 1;  // minus header
    int start = std::max(0, static_cast<int>(m.hex_lines.size()) - data_h);
    for (int row = 0; row < data_h && start + row < static_cast<int>(m.hex_lines.size()); ++row) {
        auto& line = m.hex_lines[start + row];
        int cy = y + 2 + row;
        float age = static_cast<float>(m.hex_lines.size() - start - row - 1) / std::max(1, data_h);
        float freshness = 1.f - age;

        // Address column
        char addr[12];
        std::snprintf(addr, sizeof(addr), "%08X", line.addr);
        c.write_text(x + 1, cy, addr, freshness > 0.9f ? S.ACCENT : S.HEX_ADDR);
        c.set(x + 9, cy, U':', S.VDIM);

        // Hex bytes with heatmap coloring
        int hex_x = x + 10;
        for (int i = 0; i < bytes_per_line && i < static_cast<int>(line.bytes.size()); ++i) {
            if (hex_x + 2 >= x + w - 1) break;
            uint8_t val = line.bytes[i];
            char hex[4];
            std::snprintf(hex, sizeof(hex), "%02X", val);

            int heat = val / 32;  // 0-7
            Sty style;
            if (freshness > 0.85f)
                style = S.HEX_HEAT[std::clamp(heat, 0, 7)];
            else if (freshness > 0.4f)
                style = S.DIM;
            else
                style = S.VDIM;

            c.write_text(hex_x, cy, hex, style);
            hex_x += 3;
            if (i == 7) hex_x++;
        }

        // ASCII column
        if (show_ascii) {
            int ascii_x = hex_x + 1;
            if (ascii_x < x + w - 2) c.set(ascii_x, cy, U'│', S.GRID);
            ascii_x += 1;
            for (int i = 0; i < bytes_per_line && i < static_cast<int>(line.bytes.size()); ++i) {
                if (ascii_x >= x + w - 1) break;
                uint8_t val = line.bytes[i];
                char ch = (val >= 0x20 && val < 0x7f) ? static_cast<char>(val) : '.';
                char buf[2] = {ch, 0};
                Sty style = (ch != '.') ? (freshness > 0.8f ? S.ACCENT : S.DIM) : S.VDIM;
                c.write_text(ascii_x, cy, buf, style);
                ascii_x++;
            }
        }
    }
}

// ── Drawing: System Gauges ────────────────────────────────────────────────────

static void draw_gauges(const Model& m, const Styles& S, Canvas& c, int x, int y, int w, int h) {
    draw_box(c, x, y, w, h, S.BORDER, " SYSTEMS ", S.TITLE);

    const char* labels[] = {"CPU", "GPU", "MEM", "NET", "DSK", "PWR"};
    int iw = w - 2, ih = h - 2;
    if (iw < 12 || ih < 1) return;

    // Always show sparklines: 2 rows per gauge
    int row_h = 2;
    int rows = std::min(6, ih / row_h);
    if (rows < 1) { rows = std::min(6, ih); row_h = 1; }
    int bar_w = iw - 9;  // label(4) + space + percentage(5)
    if (bar_w < 4) return;

    for (int i = 0; i < rows; ++i) {
        int cy = y + 1 + i * row_h;
        float val = m.gauges[i];

        // Label
        c.write_text(x + 1, cy, labels[i], val > 0.8f ? S.HOT : S.LABEL);

        // Bar with gradient fill and sub-character edge
        int filled_full = static_cast<int>(val * bar_w);
        float frac = val * bar_w - filled_full;

        for (int j = 0; j < bar_w; ++j) {
            int cx = x + 5 + j;
            if (j < filled_full) {
                // Gradient along the bar
                float grad = static_cast<float>(j) / bar_w;
                int si = std::clamp(static_cast<int>(grad * 7.f), 0, 7);
                c.set(cx, cy, U'█', S.BAR[si]);
            } else if (j == filled_full) {
                int bi = std::clamp(static_cast<int>(frac * 8.f), 0, 8);
                static const char32_t partials[] = {U' ', U'▏', U'▎', U'▍', U'▌', U'▋', U'▊', U'▉', U'█'};
                Sty style = val > 0.85f ? S.HOT : val > 0.6f ? S.ACCENT : S.COLD;
                c.set(cx, cy, partials[bi], style);
            } else {
                c.set(cx, cy, U'░', S.VDIM);
            }
        }

        // 80% threshold marker
        int warn_x = x + 5 + static_cast<int>(0.8f * bar_w);
        if (warn_x < x + w - 5)
            c.set(warn_x, cy, val > 0.8f ? U'!' : U'┊', val > 0.8f ? S.HOT : S.DIM);

        // Percentage
        char pct[6];
        std::snprintf(pct, sizeof(pct), "%3d%%", static_cast<int>(val * 100));
        Sty pct_style = val > 0.8f ? S.HOT : val > 0.5f ? S.ACCENT : S.COLD;
        c.write_text(x + w - 5, cy, pct, pct_style);

        // Sparkline history row
        if (row_h >= 2 && cy + 1 < y + h - 1) {
            int spark_w = std::min(bar_w, 60);
            for (int j = 0; j < spark_w; ++j) {
                int hi = (m.gauge_hist_idx - spark_w + j + 60) % 60;
                if (hi < 0) hi += 60;
                float hv = m.gauge_history[i][hi];
                int bi = std::clamp(static_cast<int>(hv * 7.f), 0, 7);
                c.set(x + 5 + j, cy + 1, g_vblocks[bi + 1], S.DIM);
            }
        }
    }
}

// ── Drawing: Network Pulse ────────────────────────────────────────────────────

static void draw_network(const Model& m, const Styles& S, Canvas& c, int x, int y, int w, int h) {
    draw_box(c, x, y, w, h, S.BORDER, " NETWORK ", S.TITLE);

    int iw = w - 2, ih = h - 2;
    if (iw < 8 || ih < 4) return;
    int ox = x + 1, oy = y + 1;

    // Faint grid
    for (int gy = 0; gy < ih; gy += std::max(1, ih / 4)) {
        for (int gx = 0; gx < iw; ++gx)
            c.set(ox + gx, oy + gy, U'·', S.GRID);
    }

    int bw = iw, bh = ih;
    int pw = bw * 2, ph = bh * 4;
    int samples = std::min(pw, 120);

    // Compute line y-values for RX and TX
    auto get_py = [&](const float* data, int i) -> int {
        int idx = (m.net_idx - samples + i + 120) % 120;
        if (idx < 0) idx += 120;
        return std::clamp(static_cast<int>((1.f - data[idx]) * (ph - 1)), 0, ph - 1);
    };

    // RX area fill
    for (int i = 0; i < samples; ++i) {
        int top = get_py(m.net_data, i);
        int bpx = (i * pw) / samples;
        for (int py = top; py < ph; ++py) {
            float depth = static_cast<float>(py - top) / std::max(1, ph - top);
            int fi = std::clamp(static_cast<int>(depth * 7.f), 0, 7);
            braille_plot(c, ox, oy, bw, bh, bpx, py, S.NET_FILL[fi]);
        }
    }

    // RX line (thick)
    for (int i = 0; i < samples; ++i) {
        int bpx = (i * pw) / samples;
        int py = get_py(m.net_data, i);
        braille_plot(c, ox, oy, bw, bh, bpx, py, S.NET_LINE);
        if (py > 0) braille_plot(c, ox, oy, bw, bh, bpx, py - 1, S.NET_LINE);
        if (py < ph - 1) braille_plot(c, ox, oy, bw, bh, bpx, py + 1, S.NET_LINE);
    }

    // TX line (thinner, different color)
    for (int i = 0; i < samples; ++i) {
        int bpx = (i * pw) / samples;
        int py = get_py(m.net_data2, i);
        braille_plot(c, ox, oy, bw, bh, bpx, py, S.NET_LINE2);
        if (py > 0) braille_plot(c, ox, oy, bw, bh, bpx, py - 1, S.NET_LINE2);
    }

    // Y-axis
    c.write_text(ox, oy, "10G", S.DIM);
    c.write_text(ox, oy + ih / 2, " 5G", S.DIM);
    c.write_text(ox, oy + ih - 1, "  0", S.DIM);

    // Legend
    c.write_text(ox + 5, oy, "RX", S.NET_LINE);
    c.write_text(ox + 8, oy, "TX", S.NET_LINE2);

    // Throughput readout
    float rx_tp = m.net_data[(m.net_idx - 1 + 120) % 120];
    float tx_tp = m.net_data2[(m.net_idx - 1 + 120) % 120];
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f/%.1fG", rx_tp * 10.f, tx_tp * 10.f);
    Sty tp_style = rx_tp > 0.7f ? S.HOT : rx_tp > 0.4f ? S.ACCENT : S.COLD;
    c.write_text(x + w - static_cast<int>(std::strlen(buf)) - 2, y, buf, tp_style);

    // Stats on bottom border
    float mn = 1.f, mx = 0.f, sum = 0.f;
    int cnt = std::min(m.net_idx, 120);
    for (int i = 0; i < cnt; ++i) {
        float v = m.net_data[i % 120];
        mn = std::min(mn, v); mx = std::max(mx, v); sum += v;
    }
    if (cnt > 0) {
        char stats[48];
        std::snprintf(stats, sizeof(stats), "min:%.1f avg:%.1f max:%.1f",
                      mn * 10.f, (sum / cnt) * 10.f, mx * 10.f);
        c.write_text(ox, y + h - 1, stats, S.DIM);
    }
}

// ── Drawing: Spirograph ───────────────────────────────────────────────────────

static void draw_spirograph(const Model& m, const Styles& S, Canvas& c, int x, int y, int w, int h) {
    draw_box(c, x, y, w, h, S.BORDER, " SIGNAL ", S.TITLE);

    int iw = w - 2, ih = h - 2;
    if (iw < 6 || ih < 4) return;
    int ox = x + 1, oy = y + 1;

    int bw = iw, bh = ih;
    int pw = bw * 2, ph = bh * 4;
    int cpx = pw / 2, cpy = ph / 2;
    float scale = std::min(pw / 20.f, ph / 20.f);

    int trail_len = static_cast<int>(m.spiro_trail.size());

    // Draw interpolated lines between consecutive same-pattern points
    int prev_px[2] = {-999, -999}, prev_py[2] = {-999, -999};
    for (int i = 0; i < trail_len; ++i) {
        auto& pt = m.spiro_trail[i];
        int ppx = cpx + static_cast<int>(pt.x * scale);
        int ppy = cpy + static_cast<int>(pt.y * scale);
        float age = static_cast<float>(i) / trail_len;
        int si = std::clamp(static_cast<int>(age * 7.f), 0, 7);
        if (pt.pattern == 1) si = std::clamp(7 - si, 0, 7);

        int p = pt.pattern;
        if (prev_px[p] != -999) {
            // Only interpolate if points are close enough
            int dx = std::abs(ppx - prev_px[p]);
            int dy = std::abs(ppy - prev_py[p]);
            if (dx < pw / 3 && dy < ph / 3) {
                braille_line(c, ox, oy, bw, bh, prev_px[p], prev_py[p], ppx, ppy, S.BRAILLE[si]);
            } else {
                braille_plot(c, ox, oy, bw, bh, ppx, ppy, S.BRAILLE[si]);
            }
        } else {
            braille_plot(c, ox, oy, bw, bh, ppx, ppy, S.BRAILLE[si]);
        }
        prev_px[p] = ppx;
        prev_py[p] = ppy;
    }

    // Phase indicator
    float phase = std::fmod(m.spiro_t, TAU);
    char phase_buf[16];
    std::snprintf(phase_buf, sizeof(phase_buf), " %.0f%c ", phase * 180.f / PI, 0x7f > 0x60 ? '.' : ' ');
    c.write_text(x + w - 7, y, phase_buf, S.DATA);
}

// ── Drawing: Particles ────────────────────────────────────────────────────────

static void draw_particles(const Model& m, const Styles& S, Canvas& c, int w, int h) {
    for (auto& p : m.particles) {
        int px = static_cast<int>(p.x);
        int py = static_cast<int>(p.y);
        if (px >= 0 && px < w && py >= 0 && py < h) {
            float age = 1.f - p.life / p.max_life;
            int si = std::clamp(static_cast<int>(age * 4.f), 0, 4);
            char32_t ch = age < 0.2f ? U'*' : age < 0.5f ? U'o' : U'.';
            c.set(px, py, ch, S.PARTICLE[si]);
        }
    }
}

// ── Drawing: Status Bar ───────────────────────────────────────────────────────

static void draw_status(const Model& m, const Styles& S, Canvas& c, int y, int w) {
    for (int x = 0; x < w; ++x) c.set(x, y, U' ', S.DIM);

    // Left: brand
    c.write_text(1, y, "NEXUS", S.BRIGHT);
    c.set(7, y, U'│', S.DIM);

    // Theme chips
    int chip_x = 9;
    for (int i = 0; i < 4; ++i) {
        bool active = (i == m.theme);
        char num = '1' + i;
        char buf[2] = {num, 0};
        c.write_text(chip_x, y, buf, active ? S.BRIGHT : S.VDIM);
        c.write_text(chip_x + 1, y, ":", S.VDIM);
        c.write_text(chip_x + 2, y, g_themes[i].name, active ? S.ACCENT : S.VDIM);
        chip_x += 2 + static_cast<int>(std::strlen(g_themes[i].name)) + 1;
    }
    c.set(chip_x, y, U'│', S.DIM);

    // Wave mode
    const char* modes[] = {"SIN", "LIS", "NOI", "HRT"};
    c.write_text(chip_x + 2, y, modes[m.wave_mode], S.ACCENT);

    // Live indicator
    static const char* spinners[] = {"|","/","-","\\"};
    int spin_idx = (m.frame / 4) % 4;
    int live_x = chip_x + 6;
    c.set(live_x, y, U'│', S.DIM);
    live_x += 2;
    c.write_text(live_x, y, m.paused ? "||" : spinners[spin_idx], m.paused ? S.HOT : S.ACCENT);
    c.write_text(live_x + 3, y, m.paused ? "PAUSED" : "LIVE", m.paused ? S.HOT : S.DATA);

    // Center: stats
    int fps_est = m.time > 1.f ? static_cast<int>(m.frame / m.time) : 0;
    char stats[64];
    std::snprintf(stats, sizeof(stats), "%dfps %.0fs %zup %zuT",
                  fps_est, m.time, m.particles.size(), m.blips.size());
    int stats_x = w / 2;
    c.set(stats_x - 1, y, U'│', S.DIM);
    c.write_text(stats_x + 1, y, stats, S.DATA);

    // Right: keybinds
    int rx = w - 48;
    if (rx > stats_x + 20) {
        c.set(rx - 1, y, U'│', S.DIM);
        const char* keys[] = {"q","w","p","g","+/-","spc","1-4"};
        const char* desc[] = {"uit ","av ","art ","ltch ","spd ","pse ","thm"};
        int kx = rx;
        for (int i = 0; i < 7 && kx < w - 4; ++i) {
            c.write_text(kx, y, keys[i], S.ACCENT);
            kx += static_cast<int>(std::strlen(keys[i]));
            c.write_text(kx, y, desc[i], S.DIM);
            kx += static_cast<int>(std::strlen(desc[i]));
        }
    }
}

// ── Drawing: Glitch Effect ────────────────────────────────────────────────────

// A glitch: bars of cells copied sideways and recoloured, plus a scanline.
// Placement comes from a hash of the frame, not the RNG, so the view stays
// a pure function of the model.
static std::uint32_t mix(std::uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
static void apply_glitch(const Model& m, const Styles& S, Canvas& c, int w, int h) {
    if (m.glitch_timer <= 0.f) return;
    const float intensity = std::clamp(m.glitch_timer, 0.f, 1.f);
    const int num_bars = static_cast<int>(intensity * 20.f);
    std::uint32_t seed = mix(static_cast<std::uint32_t>(m.frame) * 2654435761U);
    auto roll = [&seed](int lo, int hi) { seed = mix(seed + 0x9e3779b9U); return lo + static_cast<int>(seed % static_cast<std::uint32_t>(hi - lo + 1)); };
    for (int i = 0; i < num_bars; ++i) {
        const int gy = roll(0, h - 1), gx = roll(0, w - 1), gw = roll(3, 25), shift = roll(-8, 8);
        for (int j = 0; j < gw && gx + j < w; ++j) {
            const int sx = gx + j + shift;
            if (sx >= 0 && sx < w) c.set(gx + j, gy, c.get(sx, gy).character, (j % 3 == 0) ? S.HOT : S.BRIGHT);
        }
    }
    if (intensity > 0.3f) {
        const int scanline = roll(0, h - 1);
        for (int gx = 0; gx < w; ++gx) c.set(gx, scanline, U'-', S.HOT);
    }
}

static Styles make_styles(int theme) {
    Styles S{};
    auto mk = [](int r, int g, int b, bool bold = false) {
        return Sty{{static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b)}, bold};
    };

        const auto& th = g_themes[theme];
        S.bg = {th.bg_r, th.bg_g, th.bg_b};
        S.DIM    = mk(th.dim_r, th.dim_g, th.dim_b);
        S.VDIM   = mk(th.dim_r / 2, th.dim_g / 2, th.dim_b / 2);
        S.ACCENT = mk(th.accent_r, th.accent_g, th.accent_b);
        S.BRIGHT = mk(th.accent_r, th.accent_g, th.accent_b, true);
        S.HOT    = mk(th.hot_r, th.hot_g, th.hot_b, true);
        S.COLD   = mk(th.cold_r, th.cold_g, th.cold_b);

        S.BORDER = mk(th.dim_r, th.dim_g, th.dim_b);
        S.TITLE  = mk(th.accent_r, th.accent_g, th.accent_b, true);
        S.LABEL  = mk(160, 160, 170);
        S.DATA   = mk(190, 190, 200);
        S.GRID   = mk(
            std::max(5, th.dim_r / 3), std::max(5, th.dim_g / 3), std::max(5, th.dim_b / 3));

        // Bar gradient: cold -> accent -> hot
        for (int i = 0; i < 8; ++i) {
            float t = static_cast<float>(i) / 7.f;
            uint8_t r, g, b;
            if (t < 0.5f) {
                float u = t * 2.f;
                r = lerp8(th.cold_r, th.accent_r, u);
                g = lerp8(th.cold_g, th.accent_g, u);
                b = lerp8(th.cold_b, th.accent_b, u);
            } else {
                float u = (t - 0.5f) * 2.f;
                r = lerp8(th.accent_r, th.hot_r, u);
                g = lerp8(th.accent_g, th.hot_g, u);
                b = lerp8(th.accent_b, th.hot_b, u);
            }
            S.BAR[i] = mk(r, g, b);
        }

        // Radar sweep gradient (10 steps, smooth fade to bg)
        for (int i = 0; i < 10; ++i) {
            float t = static_cast<float>(i) / 9.f;
            uint8_t r = lerp8(th.accent_r, th.bg_r, t);
            uint8_t g = lerp8(th.accent_g, th.bg_g, t);
            uint8_t b = lerp8(th.accent_b, th.bg_b, t);
            S.RADAR[i] = mk(
                std::max<uint8_t>(r, 5), std::max<uint8_t>(g, 5), std::max<uint8_t>(b, 5));
        }

        // Waveform
        S.WAVE[0] = mk(th.accent_r, th.accent_g, th.accent_b, true);
        S.WAVE[1] = mk(th.accent_r, th.accent_g, th.accent_b);
        S.WAVE[2] = mk(th.hot_r, th.hot_g, th.hot_b);
        S.WAVE[3] = mk(th.cold_r, th.cold_g, th.cold_b);

        // Waveform area fill gradient (8 steps: bright near line -> dim away)
        for (int i = 0; i < 8; ++i) {
            float t = static_cast<float>(i) / 7.f;
            uint8_t r = lerp8(th.accent_r, th.bg_r, t);
            uint8_t g = lerp8(th.accent_g, th.bg_g, t);
            uint8_t b = lerp8(th.accent_b, th.bg_b, t);
            S.WAVE_FILL[i] = mk(
                std::max<uint8_t>(r, 3), std::max<uint8_t>(g, 3), std::max<uint8_t>(b, 3));
        }

        // Hex byte heatmap
        for (int i = 0; i < 8; ++i) {
            float t = static_cast<float>(i) / 7.f;
            uint8_t r, g, b;
            if (t < 0.5f) {
                float u = t * 2.f;
                r = lerp8(th.cold_r, th.accent_r, u);
                g = lerp8(th.cold_g, th.accent_g, u);
                b = lerp8(th.cold_b, th.accent_b, u);
            } else {
                float u = (t - 0.5f) * 2.f;
                r = lerp8(th.accent_r, th.hot_r, u);
                g = lerp8(th.accent_g, th.hot_g, u);
                b = lerp8(th.accent_b, th.hot_b, u);
            }
            S.HEX_HEAT[i] = mk(r, g, b);
        }
        S.HEX_ADDR = mk(th.accent_r / 2, th.accent_g / 2, th.accent_b / 2);

        // Particles
        for (int i = 0; i < 5; ++i) {
            float t = static_cast<float>(i) / 4.f;
            uint8_t r = lerp8(th.hot_r, th.dim_r, t);
            uint8_t g = lerp8(th.hot_g, th.dim_g, t);
            uint8_t b = lerp8(th.hot_b, th.dim_b, t);
            S.PARTICLE[i] = mk(r, g, b, i == 0);
        }

        // Spectrum gradient: 12 steps cold -> hot -> white
        for (int i = 0; i < 12; ++i) {
            float t = static_cast<float>(i) / 11.f;
            uint8_t r, g, b;
            if (t < 0.3f) {
                float u = t / 0.3f;
                r = lerp8(th.cold_r, th.accent_r, u);
                g = lerp8(th.cold_g, th.accent_g, u);
                b = lerp8(th.cold_b, th.accent_b, u);
            } else if (t < 0.65f) {
                float u = (t - 0.3f) / 0.35f;
                r = lerp8(th.accent_r, th.hot_r, u);
                g = lerp8(th.accent_g, th.hot_g, u);
                b = lerp8(th.accent_b, th.hot_b, u);
            } else {
                float u = (t - 0.65f) / 0.35f;
                r = lerp8(th.hot_r, 255, u);
                g = lerp8(th.hot_g, 240, u);
                b = lerp8(th.hot_b, 255, u);
            }
            S.SPECTRUM[i] = mk(r, g, b);
        }

        // Spectrum reflection
        for (int i = 0; i < 6; ++i) {
            float t = static_cast<float>(i) / 5.f;
            uint8_t r = lerp8(th.dim_r, th.bg_r, t);
            uint8_t g = lerp8(th.dim_g, th.bg_g, t);
            uint8_t b = lerp8(th.dim_b, th.bg_b, t);
            S.SPEC_REFL[i] = mk(
                std::max<uint8_t>(r, 3), std::max<uint8_t>(g, 3), std::max<uint8_t>(b, 3));
        }

        // Spirograph colors (8 steps)
        for (int i = 0; i < 8; ++i) {
            float t = static_cast<float>(i) / 7.f;
            uint8_t r, g, b;
            if (t < 0.5f) {
                float u = t * 2.f;
                r = lerp8(th.bg_r, th.accent_r, u);
                g = lerp8(th.bg_g, th.accent_g, u);
                b = lerp8(th.bg_b, th.accent_b, u);
            } else {
                float u = (t - 0.5f) * 2.f;
                r = lerp8(th.accent_r, th.hot_r, u);
                g = lerp8(th.accent_g, th.hot_g, u);
                b = lerp8(th.accent_b, th.hot_b, u);
            }
            S.BRAILLE[i] = mk(std::max<uint8_t>(r, 3), std::max<uint8_t>(g, 3), std::max<uint8_t>(b, 3), i >= 6);
        }

        // Network
        S.NET_LINE = mk(th.accent_r, th.accent_g, th.accent_b, true);
        S.NET_LINE2 = mk(th.hot_r, th.hot_g, th.hot_b);
        for (int i = 0; i < 8; ++i) {
            float t = static_cast<float>(i) / 7.f;
            uint8_t r = lerp8(th.accent_r, th.bg_r, t);
            uint8_t g = lerp8(th.accent_g, th.bg_g, t);
            uint8_t b = lerp8(th.accent_b, th.bg_b, t);
            S.NET_FILL[i] = mk(
                std::max<uint8_t>(r, 3), std::max<uint8_t>(g, 3), std::max<uint8_t>(b, 3));
        }
    return S;
}

// ── Program ───────────────────────────────────────────────────────────────────

struct Tick {};
struct Resize   { int cols, rows; };
struct SetTheme { int theme; };
struct Pause {};
struct Speed    { float by; };
struct Particles {};
struct Glitch {};
struct WaveMode {};
struct Reset {};
struct Quit {};
using Msg = std::variant<Tick, Resize, SetTheme, Pause, Speed, Particles, Glitch, WaveMode, Reset, Quit>;

struct Dashboard {
    using Model = ::Model;
    using Msg   = ::Msg;
    using Cmd   = jaal::Cmd<Msg>;
    using Sub   = jaal::Sub<Msg, on_key, on_resize>;

    static constexpr float kDt = 1.f / 60.f;

    static Cmd update(Model& m, Resize r)   { m.w = r.cols; m.h = r.rows; return {}; }
    static Cmd update(Model& m, SetTheme t) { m.theme = t.theme; return {}; }
    static Cmd update(Model& m, Pause)      { m.paused = !m.paused; return {}; }
    static Cmd update(Model& m, Speed s)    { m.speed = std::clamp(m.speed + s.by, 0.25f, 4.f); return {}; }
    static Cmd update(Model& m, Glitch)     { m.glitch_timer = 1.5f; return {}; }
    static Cmd update(Model& m, WaveMode)   { m.wave_mode = (m.wave_mode + 1) % 4; return {}; }
    static Cmd update(Model& m, Reset)      { m = Model{.w = m.w, .h = m.h}; return {}; }
    static Cmd update(Model&, Quit)         { return Cmd::quit(0); }
    static Cmd update(Model& m, Particles) {
        std::uniform_real_distribution<float> fx(10.f, std::max(11.f, static_cast<float>(m.w - 10)));
        std::uniform_real_distribution<float> fy(3.f, std::max(4.f, m.h * 0.6f));
        spawn_burst(m, fx(m.rng), fy(m.rng), 40);
        return {};
    }
    static Cmd update(Model& m, Tick) {
        tick(m, kDt);
        if (m.paused) return {};
        m.particle_timer -= kDt;                       // an automatic burst every few seconds
        if (m.particle_timer <= 0.f && m.w > 10) {
            spawn_burst(m, std::uniform_real_distribution<float>(5.f, static_cast<float>(m.w - 5))(m.rng),
                        std::uniform_real_distribution<float>(2.f, m.h * 0.7f)(m.rng), 20);
            m.particle_timer = std::uniform_real_distribution<float>(2.f, 4.f)(m.rng);
        }
        return {};
    }

    static Element view(const Model& m) {
        if (m.w < 40 || m.h < 12) return dsl::text("");
        const Styles S = make_styles(m.theme);
        Glyphs g(m.w, m.h);
        for (int y = 0; y < m.h; ++y)
            for (int x = 0; x < m.w; ++x) g(x, y) = {U' ', S.DIM.fg, S.bg, false};
        Surface c{g, S.bg};
        const int w = m.w;
        const int avail_h = m.h - 1;                                   // the last row is the status bar
        const int top_h = std::max(6, avail_h * 35 / 100);
        const int mid_h = std::max(6, avail_h * 35 / 100);
        const int bot_h = std::max(4, avail_h - top_h - mid_h);

        // Top: waveform (60%) | radar (40%)
        int wave_w = w * 60 / 100;
        draw_waveform(m, S, c, 0, 0, wave_w, top_h);
        draw_radar(m, S, c, wave_w, 0, w - wave_w, top_h);

        // Mid: spectrum (40%) | gauges (30%) | spirograph (30%)
        int spec_w = w * 40 / 100;
        int gauge_w = w * 30 / 100;
        int spiro_w = w - spec_w - gauge_w;
        draw_spectrum(m, S, c, 0, top_h, spec_w, mid_h);
        draw_gauges(m, S, c, spec_w, top_h, gauge_w, mid_h);
        draw_spirograph(m, S, c, spec_w + gauge_w, top_h, spiro_w, mid_h);

        // Bot: hex (50%) | network (50%)
        int hex_w = w * 50 / 100;
        draw_hex(m, S, c, 0, top_h + mid_h, hex_w, bot_h);
        draw_network(m, S, c, hex_w, top_h + mid_h, w - hex_w, bot_h);

        draw_particles(m, S, c, m.w, m.h);
        apply_glitch(m, S, c, m.w, m.h);
        draw_status(m, S, c, m.h - 1, m.w);
        return glyphs(std::move(g));
    }

    static Sub subscribe(const Model&) {
        return Sub::batch(
            Sub::every(16ms, Tick{}),
            Sub::on(on_resize{}, [](const ResizeEvent& r) -> std::optional<Msg> {
                return Resize{r.width.value, r.height.value};
            }),
            keys<Sub>({
                {'1', SetTheme{0}}, {'2', SetTheme{1}}, {'3', SetTheme{2}}, {'4', SetTheme{3}},
                {' ', Pause{}}, {'+', Speed{0.25f}}, {'=', Speed{0.25f}}, {'-', Speed{-0.25f}},
                {'p', Particles{}}, {'g', Glitch{}}, {'w', WaveMode{}}, {'r', Reset{}},
                {'q', Quit{}}, {SpecialKey::Escape, Quit{}},
            }));
    }
    static bool subs_key(const Model&) { return true; }
};

static_assert(Program<Dashboard>);

}  // namespace

int main() { return run<Dashboard>({.title = "NEXUS"}); }
