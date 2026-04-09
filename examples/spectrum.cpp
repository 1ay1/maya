// maya -- Audio Spectrum Analyzer
//
// Simulated real-time spectrum analyzer with four visualization modes.
// Audio data is synthesized from layered sine waves that evolve over time,
// creating convincing music-like patterns with beat detection.
//
// Keys: 1-4=mode  space=change track  q/Esc=quit

#include <maya/internal.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace maya;

// -- Constants ---------------------------------------------------------------

static constexpr int NUM_BARS      = 64;
static constexpr int NUM_WATERFALL = 128; // rows of waterfall history
static constexpr float PEAK_DECAY  = 0.012f;
static constexpr float BAR_SMOOTH  = 0.25f;  // interpolation factor per frame
static constexpr float PI          = 3.14159265358979f;
static constexpr float TWO_PI      = 6.28318530717959f;

// -- State -------------------------------------------------------------------

static std::mt19937 g_rng{42};
static int g_mode = 0;          // 0=bars, 1=mirror, 2=circular, 3=waterfall
static int g_track = 0;
static int g_num_tracks = 5;

// Spectrum data
static float g_spectrum[NUM_BARS];        // current target values [0..1]
static float g_display[NUM_BARS];         // smoothed display values
static float g_peaks[NUM_BARS];           // peak hold positions
static float g_peak_vel[NUM_BARS];        // peak fall velocity

// Waterfall history
static std::vector<std::vector<float>> g_waterfall;

// Beat detection
static float g_bass_avg    = 0.0f;
static float g_bass_energy = 0.0f;
static bool  g_beat        = false;
static int   g_beat_flash  = 0;

// Time
static float g_time = 0.0f;
using Clock = std::chrono::steady_clock;
static auto g_last = Clock::now();

// -- Style cache -------------------------------------------------------------

// We pre-intern styles for the gradient at various amplitudes
static constexpr int GRAD_STEPS = 64;
static uint16_t S_GRAD[GRAD_STEPS];          // fg=gradient color, bg=black
static uint16_t S_GRAD_BG[GRAD_STEPS];       // bg=gradient color (for filled bars)
static uint16_t S_PEAK[GRAD_STEPS];          // peak dot style
static uint16_t S_WATERFALL[GRAD_STEPS];     // bg=intensity color for waterfall

// Half-block styles for circular mode: need fg/bg combos -- use dynamic
// For circular mode we need arbitrary fg+bg combos; pre-intern a set
static uint16_t S_CIRC[GRAD_STEPS * GRAD_STEPS]; // [fg_idx * GRAD_STEPS + bg_idx] -- too many
// Instead, for circular we just use a smaller palette
static constexpr int CIRC_STEPS = 16;
static uint16_t S_CIRC_FB[CIRC_STEPS][CIRC_STEPS]; // fg x bg

static uint16_t S_BAR_BG;
static uint16_t S_BAR_DIM;
static uint16_t S_BAR_ACC;
static uint16_t S_BAR_BEAT;
static uint16_t S_BLACK;
static uint16_t S_BEAT_BG;      // flash background on beat

// -- Gradient color -----------------------------------------------------------

static Color gradient_color(float t) {
    // blue -> cyan -> green -> yellow -> red
    t = std::clamp(t, 0.0f, 1.0f);
    if (t < 0.25f) {
        float s = t / 0.25f;
        return Color::rgb(0,
                          static_cast<uint8_t>(s * 200),
                          static_cast<uint8_t>(200 + s * 55));
    }
    if (t < 0.5f) {
        float s = (t - 0.25f) / 0.25f;
        return Color::rgb(0,
                          static_cast<uint8_t>(200 + s * 55),
                          static_cast<uint8_t>(255 - s * 255));
    }
    if (t < 0.75f) {
        float s = (t - 0.5f) / 0.25f;
        return Color::rgb(static_cast<uint8_t>(s * 255),
                          static_cast<uint8_t>(255 - s * 30),
                          0);
    }
    float s = (t - 0.75f) / 0.25f;
    return Color::rgb(255,
                      static_cast<uint8_t>(225 - s * 225),
                      0);
}

// Waterfall uses a different palette: black -> blue -> purple -> red -> yellow -> white
static Color waterfall_color(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    if (t < 0.2f) {
        float s = t / 0.2f;
        return Color::rgb(0, 0, static_cast<uint8_t>(s * 180));
    }
    if (t < 0.4f) {
        float s = (t - 0.2f) / 0.2f;
        return Color::rgb(static_cast<uint8_t>(s * 140),
                          0,
                          static_cast<uint8_t>(180 + s * 75));
    }
    if (t < 0.6f) {
        float s = (t - 0.4f) / 0.2f;
        return Color::rgb(static_cast<uint8_t>(140 + s * 115),
                          static_cast<uint8_t>(s * 40),
                          static_cast<uint8_t>(255 - s * 255));
    }
    if (t < 0.8f) {
        float s = (t - 0.6f) / 0.2f;
        return Color::rgb(255,
                          static_cast<uint8_t>(40 + s * 215),
                          0);
    }
    float s = (t - 0.8f) / 0.2f;
    return Color::rgb(255, 255, static_cast<uint8_t>(s * 255));
}

// -- Audio simulation --------------------------------------------------------

struct TrackDef {
    struct Osc {
        float freq;      // Hz
        float amp;       // amplitude
        float phase;     // phase offset
        float mod_freq;  // amplitude modulation Hz
        float mod_depth; // 0..1
    };
    std::vector<Osc> oscillators;
    float bass_freq;
    float bass_amp;
    float bass_mod;
};

static std::vector<TrackDef> g_tracks;

static void init_tracks() {
    g_tracks.clear();

    // Track 0: EDM-like with strong bass kick
    g_tracks.push_back({{
        {2.0f,  0.9f, 0.0f, 0.5f,  0.8f},   // deep bass pulse
        {4.0f,  0.7f, 0.3f, 1.0f,  0.5f},   // sub bass
        {8.0f,  0.5f, 1.0f, 2.0f,  0.6f},   // low mid
        {16.0f, 0.4f, 0.5f, 3.0f,  0.4f},   // mid
        {24.0f, 0.3f, 0.8f, 4.5f,  0.5f},   // upper mid
        {32.0f, 0.25f, 1.2f, 6.0f, 0.3f},   // presence
        {48.0f, 0.15f, 0.2f, 8.0f, 0.7f},   // high
    }, 2.0f, 0.9f, 0.5f});

    // Track 1: Ambient / pad
    g_tracks.push_back({{
        {1.5f,  0.4f, 0.0f, 0.1f, 0.3f},
        {3.0f,  0.5f, 0.7f, 0.15f, 0.4f},
        {6.0f,  0.6f, 1.4f, 0.2f, 0.5f},
        {12.0f, 0.7f, 0.3f, 0.25f, 0.3f},
        {20.0f, 0.5f, 2.0f, 0.3f, 0.4f},
        {30.0f, 0.3f, 1.1f, 0.4f, 0.5f},
        {45.0f, 0.2f, 0.5f, 0.5f, 0.6f},
    }, 1.5f, 0.4f, 0.1f});

    // Track 2: Rock / drums
    g_tracks.push_back({{
        {2.5f,  0.8f, 0.0f, 2.0f, 0.9f},
        {5.0f,  0.6f, 0.5f, 2.0f, 0.7f},
        {10.0f, 0.7f, 1.0f, 4.0f, 0.5f},
        {15.0f, 0.5f, 0.3f, 3.0f, 0.6f},
        {22.0f, 0.6f, 0.8f, 5.0f, 0.4f},
        {35.0f, 0.4f, 1.5f, 7.0f, 0.5f},
        {50.0f, 0.3f, 0.2f, 9.0f, 0.3f},
    }, 2.5f, 0.8f, 2.0f});

    // Track 3: Synthwave
    g_tracks.push_back({{
        {1.8f,  0.6f, 0.0f, 0.8f, 0.6f},
        {3.6f,  0.5f, 1.0f, 1.2f, 0.5f},
        {7.2f,  0.7f, 0.5f, 1.6f, 0.7f},
        {14.0f, 0.8f, 1.5f, 2.4f, 0.4f},
        {21.0f, 0.6f, 0.3f, 3.2f, 0.6f},
        {28.0f, 0.5f, 0.8f, 4.0f, 0.5f},
        {42.0f, 0.35f, 1.2f, 5.5f, 0.4f},
    }, 1.8f, 0.6f, 0.8f});

    // Track 4: Glitch / IDM
    g_tracks.push_back({{
        {3.0f,  0.7f, 0.0f, 3.0f,  0.9f},
        {7.0f,  0.5f, 0.4f, 5.0f,  0.8f},
        {11.0f, 0.6f, 0.9f, 7.0f,  0.7f},
        {17.0f, 0.5f, 1.3f, 11.0f, 0.6f},
        {23.0f, 0.4f, 0.2f, 13.0f, 0.8f},
        {37.0f, 0.3f, 0.7f, 17.0f, 0.5f},
        {53.0f, 0.2f, 1.1f, 19.0f, 0.7f},
    }, 3.0f, 0.7f, 3.0f});

    g_num_tracks = static_cast<int>(g_tracks.size());
}

static void generate_spectrum(float dt) {
    g_time += dt;
    std::uniform_real_distribution<float> noise(-0.02f, 0.02f);

    const auto& track = g_tracks[g_track];

    for (int i = 0; i < NUM_BARS; ++i) {
        float freq_pos = static_cast<float>(i) / NUM_BARS; // 0..1
        float val = 0.0f;

        for (const auto& osc : track.oscillators) {
            // Each oscillator contributes to nearby frequency bins
            float osc_pos = osc.freq / 64.0f; // normalize to 0..1 range
            float dist = std::abs(freq_pos - osc_pos);
            float spread = 0.08f + osc_pos * 0.05f; // wider spread for higher freqs
            float influence = std::exp(-dist * dist / (2.0f * spread * spread));

            // Amplitude with modulation
            float mod = 1.0f - osc.mod_depth * (0.5f + 0.5f * std::sin(TWO_PI * osc.mod_freq * g_time + osc.phase));
            val += osc.amp * mod * influence;
        }

        // Add some harmonics and noise for realism
        float harmonic = 0.15f * std::sin(TWO_PI * (3.0f + freq_pos * 20.0f) * g_time * 0.1f);
        val += harmonic * (1.0f - freq_pos); // harmonics stronger at low end

        val += noise(g_rng);
        val = std::clamp(val, 0.0f, 1.0f);
        g_spectrum[i] = val;
    }

    // Beat detection: check bass energy
    float bass = 0.0f;
    for (int i = 0; i < NUM_BARS / 8; ++i)
        bass += g_spectrum[i];
    bass /= (NUM_BARS / 8);

    g_bass_avg = g_bass_avg * 0.95f + bass * 0.05f;
    g_bass_energy = bass;
    g_beat = (bass > g_bass_avg * 1.4f && bass > 0.4f);

    if (g_beat) g_beat_flash = 6;
    if (g_beat_flash > 0) --g_beat_flash;

    // Smooth display values
    for (int i = 0; i < NUM_BARS; ++i) {
        float target = g_spectrum[i];
        // Faster rise, slower fall
        float speed = (target > g_display[i]) ? 0.4f : BAR_SMOOTH;
        g_display[i] += (target - g_display[i]) * speed;

        // Peak hold
        if (g_display[i] > g_peaks[i]) {
            g_peaks[i] = g_display[i];
            g_peak_vel[i] = 0.0f;
        } else {
            g_peak_vel[i] += PEAK_DECAY * 0.5f;
            g_peaks[i] -= g_peak_vel[i];
            if (g_peaks[i] < 0.0f) g_peaks[i] = 0.0f;
        }
    }

    // Push into waterfall
    std::vector<float> row(NUM_BARS);
    for (int i = 0; i < NUM_BARS; ++i) row[i] = g_display[i];
    g_waterfall.push_back(std::move(row));
    if (static_cast<int>(g_waterfall.size()) > NUM_WATERFALL)
        g_waterfall.erase(g_waterfall.begin());
}

// -- Rebuild styles ----------------------------------------------------------

static void rebuild(StylePool& pool, int /*w*/, int /*h*/) {
    for (int i = 0; i < GRAD_STEPS; ++i) {
        float t = static_cast<float>(i) / (GRAD_STEPS - 1);
        Color c = gradient_color(t);
        S_GRAD[i]    = pool.intern(Style{}.with_fg(c));
        S_GRAD_BG[i] = pool.intern(Style{}.with_fg(c).with_bg(Color::rgb(10, 10, 15)));
        S_PEAK[i]    = pool.intern(Style{}.with_fg(c).with_bold());

        Color wc = waterfall_color(t);
        S_WATERFALL[i] = pool.intern(Style{}.with_bg(wc));
    }

    // Circular mode palette (smaller)
    for (int f = 0; f < CIRC_STEPS; ++f) {
        for (int b = 0; b < CIRC_STEPS; ++b) {
            float tf = static_cast<float>(f) / (CIRC_STEPS - 1);
            float tb = static_cast<float>(b) / (CIRC_STEPS - 1);
            S_CIRC_FB[f][b] = pool.intern(
                Style{}.with_fg(gradient_color(tf)).with_bg(gradient_color(tb)));
        }
    }

    S_BLACK   = pool.intern(Style{}.with_bg(Color::rgb(10, 10, 15)));
    S_BAR_BG  = pool.intern(Style{}.with_bg(Color::rgb(15, 15, 25)).with_fg(Color::rgb(100, 100, 120)));
    S_BAR_DIM = pool.intern(Style{}.with_bg(Color::rgb(15, 15, 25)).with_fg(Color::rgb(60, 60, 80)));
    S_BAR_ACC = pool.intern(Style{}.with_bg(Color::rgb(15, 15, 25)).with_fg(Color::rgb(80, 200, 255)).with_bold());
    S_BAR_BEAT= pool.intern(Style{}.with_bg(Color::rgb(60, 15, 25)).with_fg(Color::rgb(255, 100, 100)).with_bold());
    S_BEAT_BG = pool.intern(Style{}.with_bg(Color::rgb(25, 10, 20)));
}

// -- Event handling ----------------------------------------------------------

static bool handle(const Event& ev) {
    if (key(ev, 'q') || key(ev, SpecialKey::Escape)) return false;

    on(ev, '1', [] { g_mode = 0; });
    on(ev, '2', [] { g_mode = 1; });
    on(ev, '3', [] { g_mode = 2; });
    on(ev, '4', [] { g_mode = 3; });
    on(ev, ' ', [] {
        g_track = (g_track + 1) % g_num_tracks;
        // Reset peaks on track change
        for (int i = 0; i < NUM_BARS; ++i) {
            g_peaks[i] = 0.0f;
            g_peak_vel[i] = 0.0f;
        }
    });

    return true;
}

// -- Paint modes -------------------------------------------------------------

static int grad_idx(float t) {
    return std::clamp(static_cast<int>(t * (GRAD_STEPS - 1)), 0, GRAD_STEPS - 1);
}

static int circ_idx(float t) {
    return std::clamp(static_cast<int>(t * (CIRC_STEPS - 1)), 0, CIRC_STEPS - 1);
}

static void paint_bars(Canvas& canvas, int w, int h, int bar_area_h) {
    int num_bars = std::min(NUM_BARS, w / 2);
    if (num_bars <= 0) return;
    int bar_width = w / num_bars;
    int gap = (bar_width > 2) ? 1 : 0;
    int draw_w = bar_width - gap;

    // Background
    uint16_t bg = (g_beat_flash > 0) ? S_BEAT_BG : S_BLACK;
    for (int y = 0; y < bar_area_h; ++y)
        for (int x = 0; x < w; ++x)
            canvas.set(x, y, U' ', bg);

    for (int i = 0; i < num_bars; ++i) {
        float val = g_display[i];
        int bar_h = static_cast<int>(val * bar_area_h);
        int peak_y = static_cast<int>(g_peaks[i] * bar_area_h);
        int x0 = i * bar_width;

        // Draw filled bar from bottom up
        for (int j = 0; j < bar_h && j < bar_area_h; ++j) {
            int y = bar_area_h - 1 - j;
            float t = static_cast<float>(j) / bar_area_h;
            int gi = grad_idx(t);
            for (int dx = 0; dx < draw_w; ++dx) {
                if (x0 + dx < w)
                    canvas.set(x0 + dx, y, U'\u2588', S_GRAD[gi]); // full block
            }
        }

        // Peak indicator
        if (peak_y > 0 && peak_y < bar_area_h) {
            int py = bar_area_h - 1 - peak_y;
            float t = static_cast<float>(peak_y) / bar_area_h;
            int gi = grad_idx(t);
            for (int dx = 0; dx < draw_w; ++dx) {
                if (x0 + dx < w)
                    canvas.set(x0 + dx, py, U'\u2594', S_PEAK[gi]); // upper 1/8 block
            }
        }
    }
}

static void paint_mirror(Canvas& canvas, int w, int h, int bar_area_h) {
    int num_bars = std::min(NUM_BARS, w / 2);
    if (num_bars <= 0) return;
    int bar_width = w / num_bars;
    int gap = (bar_width > 2) ? 1 : 0;
    int draw_w = bar_width - gap;
    int mid = bar_area_h / 2;

    uint16_t bg = (g_beat_flash > 0) ? S_BEAT_BG : S_BLACK;
    for (int y = 0; y < bar_area_h; ++y)
        for (int x = 0; x < w; ++x)
            canvas.set(x, y, U' ', bg);

    for (int i = 0; i < num_bars; ++i) {
        float val = g_display[i];
        int half_h = static_cast<int>(val * mid);
        int x0 = i * bar_width;

        for (int j = 0; j < half_h && j < mid; ++j) {
            float t = static_cast<float>(j) / mid;
            int gi = grad_idx(t);
            // Upper half (going up from mid)
            int yu = mid - 1 - j;
            // Lower half (going down from mid)
            int yl = mid + j;
            for (int dx = 0; dx < draw_w; ++dx) {
                if (x0 + dx < w) {
                    canvas.set(x0 + dx, yu, U'\u2588', S_GRAD[gi]);
                    canvas.set(x0 + dx, yl, U'\u2588', S_GRAD[gi]);
                }
            }
        }

        // Peak indicators
        int peak_h = static_cast<int>(g_peaks[i] * mid);
        if (peak_h > 0 && peak_h < mid) {
            float t = static_cast<float>(peak_h) / mid;
            int gi = grad_idx(t);
            int pyu = mid - 1 - peak_h;
            int pyl = mid + peak_h;
            for (int dx = 0; dx < draw_w; ++dx) {
                if (x0 + dx < w) {
                    canvas.set(x0 + dx, pyu, U'\u2594', S_PEAK[gi]);
                    if (pyl < bar_area_h)
                        canvas.set(x0 + dx, pyl, U'\u2581', S_PEAK[gi]);
                }
            }
        }
    }
}

static void paint_circular(Canvas& canvas, int w, int h, int bar_area_h) {
    // Clear with beat-aware background
    uint16_t bg = (g_beat_flash > 0) ? S_BEAT_BG : S_BLACK;
    for (int y = 0; y < bar_area_h; ++y)
        for (int x = 0; x < w; ++x)
            canvas.set(x, y, U' ', bg);

    // Pixel grid: w columns, bar_area_h*2 pixel rows (half-block rendering)
    int px_w = w;
    int px_h = bar_area_h * 2;
    float cx = static_cast<float>(px_w) / 2.0f;
    float cy = static_cast<float>(px_h) / 2.0f;
    float max_r = std::min(cx, cy) * 0.85f;
    float inner_r = max_r * 0.3f;

    // We'll build a pixel buffer
    // Using a flat vector for the pixel intensities
    std::vector<float> pixels(px_w * px_h, 0.0f);

    int num_bars = NUM_BARS;
    for (int i = 0; i < num_bars; ++i) {
        float angle = TWO_PI * static_cast<float>(i) / num_bars - PI / 2.0f;
        float next_angle = TWO_PI * static_cast<float>(i + 1) / num_bars - PI / 2.0f;
        float val = g_display[i];
        float bar_len = val * (max_r - inner_r);

        // Draw radial line segments
        int steps = static_cast<int>(bar_len) + 1;
        for (int s = 0; s <= steps; ++s) {
            float r = inner_r + static_cast<float>(s);
            if (r > inner_r + bar_len) break;
            // Sweep a small arc
            int arc_steps = std::max(2, static_cast<int>((next_angle - angle) * r));
            for (int a = 0; a < arc_steps; ++a) {
                float ang = angle + (next_angle - angle) * static_cast<float>(a) / arc_steps;
                int px = static_cast<int>(cx + r * std::cos(ang));
                int py = static_cast<int>(cy + r * std::sin(ang));
                if (px >= 0 && px < px_w && py >= 0 && py < px_h) {
                    float t = static_cast<float>(s) / (max_r - inner_r);
                    pixels[py * px_w + px] = std::max(pixels[py * px_w + px], t);
                }
            }
        }

        // Peak dot
        float peak_r = inner_r + g_peaks[i] * (max_r - inner_r);
        float mid_angle = (angle + next_angle) * 0.5f;
        int ppx = static_cast<int>(cx + peak_r * std::cos(mid_angle));
        int ppy = static_cast<int>(cy + peak_r * std::sin(mid_angle));
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                int fx = ppx + dx, fy = ppy + dy;
                if (fx >= 0 && fx < px_w && fy >= 0 && fy < px_h)
                    pixels[fy * px_w + fx] = std::max(pixels[fy * px_w + fx], 0.95f);
            }
        }
    }

    // Render with half-block characters
    for (int ty = 0; ty < bar_area_h; ++ty) {
        int py_top = ty * 2;
        int py_bot = ty * 2 + 1;
        for (int x = 0; x < w; ++x) {
            float top_val = (py_top < px_h) ? pixels[py_top * px_w + x] : 0.0f;
            float bot_val = (py_bot < px_h) ? pixels[py_bot * px_w + x] : 0.0f;

            if (top_val > 0.01f || bot_val > 0.01f) {
                int fi = circ_idx(top_val);
                int bi = circ_idx(bot_val);
                canvas.set(x, ty, U'\u2580', S_CIRC_FB[fi][bi]);
            }
        }
    }
}

static void paint_waterfall(Canvas& canvas, int w, int h, int bar_area_h) {
    // Top portion: small bars (1/4 height)
    int bars_h = bar_area_h / 4;
    int wf_h = bar_area_h - bars_h;

    // Mini bars at top
    {
        int num_bars = std::min(NUM_BARS, w);
        float col_w = static_cast<float>(w) / num_bars;

        for (int y = 0; y < bars_h; ++y)
            for (int x = 0; x < w; ++x)
                canvas.set(x, y, U' ', S_BLACK);

        for (int i = 0; i < num_bars; ++i) {
            float val = g_display[i];
            int bar_h = static_cast<int>(val * bars_h);
            int x0 = static_cast<int>(i * col_w);
            int x1 = static_cast<int>((i + 1) * col_w);
            for (int j = 0; j < bar_h && j < bars_h; ++j) {
                int y = bars_h - 1 - j;
                float t = static_cast<float>(j) / bars_h;
                int gi = grad_idx(t);
                for (int x = x0; x < x1 && x < w; ++x)
                    canvas.set(x, y, U'\u2588', S_GRAD[gi]);
            }
        }
    }

    // Waterfall below
    int wf_rows = static_cast<int>(g_waterfall.size());
    int display_rows = std::min(wf_h, wf_rows);

    for (int row = 0; row < wf_h; ++row) {
        int y = bars_h + row;
        int data_idx = wf_rows - display_rows + row;
        if (data_idx < 0 || data_idx >= wf_rows) {
            for (int x = 0; x < w; ++x)
                canvas.set(x, y, U' ', S_BLACK);
            continue;
        }

        const auto& data = g_waterfall[data_idx];
        int num_bars = std::min(NUM_BARS, static_cast<int>(data.size()));
        float col_w = static_cast<float>(w) / num_bars;

        for (int i = 0; i < num_bars; ++i) {
            float val = data[i];
            int gi = grad_idx(val);
            int x0 = static_cast<int>(i * col_w);
            int x1 = static_cast<int>((i + 1) * col_w);
            for (int x = x0; x < x1 && x < w; ++x)
                canvas.set(x, y, U' ', S_WATERFALL[gi]);
        }
    }
}

// -- Main paint --------------------------------------------------------------

static void paint(Canvas& canvas, int w, int h) {
    // Timing
    auto now = Clock::now();
    float dt = std::chrono::duration<float>(now - g_last).count();
    g_last = now;
    dt = std::clamp(dt, 0.001f, 0.1f);

    generate_spectrum(dt);

    int bar_y = h - 1;
    int bar_area_h = h - 1;

    switch (g_mode) {
        case 0: paint_bars(canvas, w, h, bar_area_h); break;
        case 1: paint_mirror(canvas, w, h, bar_area_h); break;
        case 2: paint_circular(canvas, w, h, bar_area_h); break;
        case 3: paint_waterfall(canvas, w, h, bar_area_h); break;
    }

    // -- Status bar --
    for (int x = 0; x < w; ++x)
        canvas.set(x, bar_y, U' ', S_BAR_BG);

    static const char* mode_names[] = {"BARS", "MIRROR", "CIRCULAR", "WATERFALL"};
    static const char* track_names[] = {"EDM", "AMBIENT", "ROCK", "SYNTHWAVE", "GLITCH"};

    // VU meter
    float vu = 0.0f;
    for (int i = 0; i < NUM_BARS; ++i) vu += g_display[i];
    vu /= NUM_BARS;

    int vu_width = std::min(20, w / 5);
    int vu_fill = static_cast<int>(vu * vu_width);

    char status[256];
    std::snprintf(status, sizeof(status), " %s  Track: %s  VU [",
                  mode_names[g_mode], track_names[g_track]);

    // Build VU bar string
    char vu_str[64];
    int vi = 0;
    for (int i = 0; i < vu_width; ++i) {
        vu_str[vi++] = (i < vu_fill) ? '|' : ' ';
    }
    vu_str[vi] = '\0';

    char full_status[384];
    std::snprintf(full_status, sizeof(full_status), "%s%s]", status, vu_str);

    uint16_t status_style = (g_beat_flash > 0) ? S_BAR_BEAT : S_BAR_ACC;
    canvas.write_text(0, bar_y, full_status, status_style);

    // Right side: keybindings
    const char* keys = "[1-4] mode  [space] track  [q] quit ";
    int klen = static_cast<int>(std::strlen(keys));
    if (klen < w)
        canvas.write_text(w - klen, bar_y, keys, S_BAR_DIM);
}

// -- Main --------------------------------------------------------------------

int main() {
    init_tracks();

    // Initialize display arrays
    std::fill(std::begin(g_spectrum), std::end(g_spectrum), 0.0f);
    std::fill(std::begin(g_display),  std::end(g_display),  0.0f);
    std::fill(std::begin(g_peaks),    std::end(g_peaks),    0.0f);
    std::fill(std::begin(g_peak_vel), std::end(g_peak_vel), 0.0f);

    (void)canvas_run(
        CanvasConfig{.fps = 60, .mouse = false, .mode = Mode::Fullscreen, .title = "spectrum"},
        rebuild,
        handle,
        paint
    );
}
