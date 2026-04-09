// maya -- Mandelbrot fractal explorer
//
// Real-time Mandelbrot renderer with smooth coloring, auto-zoom toward
// Seahorse Valley, 6 switchable palettes, half-block pixels for 2x
// vertical resolution.
//
// Keys: arrows=pan  +/-=zoom  space=toggle auto-zoom  1-6=palette
//       r=reset  q/Esc=quit

#include <maya/maya.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <cstring>

using namespace maya;

// ── Constants ────────────────────────────────────────────────────────────────

static constexpr int MAX_ITER = 512;
static constexpr float PI  = 3.14159265f;
static constexpr float TAU = 6.28318530f;

// ── State ────────────────────────────────────────────────────────────────────

static double g_cx     = -0.5;      // center x
static double g_cy     =  0.0;      // center y
static double g_zoom   =  1.0;      // units = half-width in complex plane
static int    g_palette = 0;        // 0-5
static bool   g_auto   = true;      // auto-zoom enabled

// Auto-zoom targets: Seahorse Valley spirals
static constexpr double TARGET_X = -0.7463;
static constexpr double TARGET_Y =  0.1102;
static constexpr double ZOOM_SPEED = 0.015;   // per frame, multiplied
static constexpr double PAN_SPEED  = 0.02;    // lerp factor per frame

static int    g_frame   = 0;
static float  g_elapsed = 0.f;
static int    g_iter_count = MAX_ITER;

// Style interning: quantized color cube
static constexpr int Q = 6;   // 6³×6³ = 46656 < 65535 (uint16_t style pool limit)
static uint16_t g_styles[Q*Q*Q][Q*Q*Q]; // [fg_idx][bg_idx]
static uint16_t g_bar_bg, g_bar_text, g_bar_accent, g_bar_dim;

// ── Color math ───────────────────────────────────────────────────────────────

struct Color3 { float r, g, b; };

static inline Color3 cmix(Color3 a, Color3 b, float t) {
    return {a.r*(1-t)+b.r*t, a.g*(1-t)+b.g*t, a.b*(1-t)+b.b*t};
}

static inline float clampf(float x, float lo, float hi) {
    return std::fmin(std::fmax(x, lo), hi);
}

// ── HSV to RGB ───────────────────────────────────────────────────────────────

static Color3 hsv(float h, float s, float v) {
    h = std::fmod(h, 1.0f);
    if (h < 0.f) h += 1.f;
    float c = v * s;
    float x = c * (1.f - std::fabs(std::fmod(h * 6.f, 2.f) - 1.f));
    float m = v - c;
    Color3 rgb;
    if      (h < 1.f/6.f) rgb = {c, x, 0};
    else if (h < 2.f/6.f) rgb = {x, c, 0};
    else if (h < 3.f/6.f) rgb = {0, c, x};
    else if (h < 4.f/6.f) rgb = {0, x, c};
    else if (h < 5.f/6.f) rgb = {x, 0, c};
    else                   rgb = {c, 0, x};
    return {rgb.r + m, rgb.g + m, rgb.b + m};
}

// ── Palettes ─────────────────────────────────────────────────────────────────

// Helper: cosine palette a + b * cos(2pi * (c*t + d))
static Color3 cosine_palette(float t, Color3 a, Color3 b, Color3 c, Color3 d) {
    return {
        clampf(a.r + b.r * std::cos(TAU * (c.r * t + d.r)), 0.f, 1.f),
        clampf(a.g + b.g * std::cos(TAU * (c.g * t + d.g)), 0.f, 1.f),
        clampf(a.b + b.b * std::cos(TAU * (c.b * t + d.b)), 0.f, 1.f)
    };
}

static Color3 palette_ultra(float t) {
    // Classic Ultra Fractal blue/gold/brown
    return cosine_palette(t,
        {0.5f, 0.5f, 0.5f},
        {0.5f, 0.5f, 0.5f},
        {1.0f, 1.0f, 1.0f},
        {0.00f, 0.10f, 0.20f});
}

static Color3 palette_fire(float t) {
    // black -> red -> orange -> yellow -> white
    if (t < 0.33f) {
        float s = t / 0.33f;
        return {s, s * 0.15f, 0.f};
    } else if (t < 0.66f) {
        float s = (t - 0.33f) / 0.33f;
        return {1.f, 0.15f + s * 0.65f, 0.f};
    } else {
        float s = (t - 0.66f) / 0.34f;
        return {1.f, 0.8f + s * 0.2f, s};
    }
}

static Color3 palette_ocean(float t) {
    // deep blue -> cyan -> white
    if (t < 0.5f) {
        float s = t / 0.5f;
        return {0.f, s * 0.4f, 0.15f + s * 0.55f};
    } else {
        float s = (t - 0.5f) / 0.5f;
        return {s * 0.7f, 0.4f + s * 0.6f, 0.7f + s * 0.3f};
    }
}

static Color3 palette_neon(float t) {
    // purple -> magenta -> pink -> cyan
    return cosine_palette(t,
        {0.5f, 0.5f, 0.5f},
        {0.5f, 0.5f, 0.5f},
        {1.0f, 1.0f, 0.5f},
        {0.80f, 0.20f, 0.50f});
}

static Color3 palette_gray_bands(float t) {
    // Mostly grayscale with color bands at certain iteration counts
    float gray = t;
    float band = std::sin(t * 40.f);
    if (band > 0.85f) {
        return {0.2f + gray * 0.6f, 0.1f + gray * 0.3f, 0.05f};
    } else if (band < -0.85f) {
        return {0.05f, 0.1f + gray * 0.3f, 0.2f + gray * 0.6f};
    }
    return {gray, gray, gray};
}

static Color3 palette_rainbow(float t) {
    return hsv(t, 0.85f, 0.9f);
}

static Color3 get_color(float smooth_iter) {
    if (smooth_iter < 0.f) return {0.f, 0.f, 0.f}; // inside set = black

    float t = std::fmod(smooth_iter / 64.f, 1.0f);

    switch (g_palette) {
        case 0: return palette_ultra(t);
        case 1: return palette_fire(t);
        case 2: return palette_ocean(t);
        case 3: return palette_neon(t);
        case 4: return palette_gray_bands(t);
        case 5: return palette_rainbow(t);
        default: return palette_ultra(t);
    }
}

// ── Mandelbrot computation ───────────────────────────────────────────────────

static float mandelbrot(double cr, double ci) {
    double zr = 0.0, zi = 0.0;
    double zr2 = 0.0, zi2 = 0.0;
    int iter = 0;

    // Main cardioid / period-2 bulb check for early bail
    double q = (cr - 0.25) * (cr - 0.25) + ci * ci;
    if (q * (q + (cr - 0.25)) <= 0.25 * ci * ci) return -1.f;
    if ((cr + 1.0) * (cr + 1.0) + ci * ci <= 0.0625) return -1.f;

    while (iter < g_iter_count && zr2 + zi2 <= 256.0) {
        zi = 2.0 * zr * zi + ci;
        zr = zr2 - zi2 + cr;
        zr2 = zr * zr;
        zi2 = zi * zi;
        ++iter;
    }

    if (iter >= g_iter_count) return -1.f;

    // Smooth iteration count
    double abs_z = std::sqrt(zr2 + zi2);
    float smooth = static_cast<float>(iter) + 1.0f
                   - std::log2(std::log2(static_cast<float>(abs_z)));
    return smooth;
}

// ── Style interning ──────────────────────────────────────────────────────────

static uint8_t to8(int qi) {
    return static_cast<uint8_t>(qi * 255 / (Q - 1));
}

static int quantize(float v) {
    return std::clamp(static_cast<int>(v * (Q - 0.01f)), 0, Q - 1);
}

static int color_idx(float r, float g, float b) {
    return quantize(r) * Q * Q + quantize(g) * Q + quantize(b);
}

static void intern_styles(StylePool& pool, int /*w*/, int /*h*/) {
    for (int fi = 0; fi < Q*Q*Q; ++fi) {
        int fr = fi / (Q*Q), fg = (fi / Q) % Q, fb = fi % Q;
        for (int bi = 0; bi < Q*Q*Q; ++bi) {
            int br = bi / (Q*Q), bg = (bi / Q) % Q, bb = bi % Q;
            g_styles[fi][bi] = pool.intern(
                Style{}.with_fg(Color::rgb(to8(fr), to8(fg), to8(fb)))
                       .with_bg(Color::rgb(to8(br), to8(bg), to8(bb))));
        }
    }
    g_bar_bg     = pool.intern(Style{}.with_fg(Color::rgb(120,120,120)).with_bg(Color::rgb(15,15,15)));
    g_bar_text   = pool.intern(Style{}.with_fg(Color::rgb(200,200,200)).with_bg(Color::rgb(15,15,15)));
    g_bar_accent = pool.intern(Style{}.with_fg(Color::rgb(80,180,255)).with_bg(Color::rgb(15,15,15)).with_bold());
    g_bar_dim    = pool.intern(Style{}.with_fg(Color::rgb(70,70,70)).with_bg(Color::rgb(15,15,15)));
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main() {
    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();
    auto fps_last = Clock::now();
    int fps_frames = 0;
    int fps_display = 0;

    (void)canvas_run(
        CanvasConfig{.fps = 30, .mouse = false, .mode = Mode::Fullscreen, .auto_clear = false, .title = "MANDELBROT"},

        // on_resize
        intern_styles,

        // on_event
        [&](const Event& ev) -> bool {
            if (key(ev, 'q') || key(ev, SpecialKey::Escape)) return false;

            // Palette switching
            on(ev, '1', [] { g_palette = 0; });
            on(ev, '2', [] { g_palette = 1; });
            on(ev, '3', [] { g_palette = 2; });
            on(ev, '4', [] { g_palette = 3; });
            on(ev, '5', [] { g_palette = 4; });
            on(ev, '6', [] { g_palette = 5; });

            // Manual zoom
            on(ev, '+', [] { g_zoom *= 0.8; });
            on(ev, '=', [] { g_zoom *= 0.8; });  // unshifted + key
            on(ev, '-', [] { g_zoom *= 1.25; });

            // Pan: arrows move in complex plane relative to zoom
            on(ev, SpecialKey::Left,  [] { g_cx -= g_zoom * 0.1; });
            on(ev, SpecialKey::Right, [] { g_cx += g_zoom * 0.1; });
            on(ev, SpecialKey::Up,    [] { g_cy -= g_zoom * 0.1; });
            on(ev, SpecialKey::Down,  [] { g_cy += g_zoom * 0.1; });

            // Toggle auto-zoom
            on(ev, ' ', [] { g_auto = !g_auto; });

            // Reset
            on(ev, 'r', [] {
                g_cx = -0.5; g_cy = 0.0; g_zoom = 1.0;
                g_auto = true;
            });

            return true;
        },

        // on_paint
        [&](Canvas& canvas, int W, int H) {
            auto now = Clock::now();
            float dt = std::chrono::duration<float>(now - last).count();
            last = now;
            dt = std::fmin(dt, 0.1f);
            g_frame++;
            g_elapsed += dt;

            // FPS counter
            fps_frames++;
            float fps_dt = std::chrono::duration<float>(now - fps_last).count();
            if (fps_dt >= 0.5f) {
                fps_display = static_cast<int>(fps_frames / fps_dt);
                fps_frames = 0;
                fps_last = now;
            }

            if (W < 10 || H < 5) return;

            // Auto-zoom: drift toward target and zoom in
            if (g_auto) {
                g_cx += (TARGET_X - g_cx) * PAN_SPEED;
                g_cy += (TARGET_Y - g_cy) * PAN_SPEED;
                g_zoom *= (1.0 - ZOOM_SPEED);

                // Reset when zoomed too deep (floating point limits)
                if (g_zoom < 1e-13) {
                    g_cx = -0.5; g_cy = 0.0; g_zoom = 1.0;
                }
            }

            // Adaptive iteration count based on zoom depth
            g_iter_count = std::clamp(
                static_cast<int>(256 + 40 * std::log2(1.0 / g_zoom)),
                256, MAX_ITER);

            int bar_y = H - 1;
            int canvas_h = H - 1;
            int pixel_h = canvas_h * 2;
            int pixel_w = W;

            double aspect = static_cast<double>(pixel_w) / static_cast<double>(pixel_h);

            // Parallel Mandelbrot — each thread handles a horizontal strip.
            static const int n_threads = std::max(1, static_cast<int>(
                std::thread::hardware_concurrency()));

            auto compute_rows = [&](int y_begin, int y_end) {
                for (int cy = y_begin; cy < y_end; ++cy) {
                    for (int cx = 0; cx < pixel_w; ++cx) {
                        int py_top = cy * 2;
                        double u = (2.0 * (cx + 0.5) / pixel_w - 1.0) * aspect;
                        double v_top = (2.0 * (py_top + 0.5) / pixel_h - 1.0);
                        double cr = g_cx + u * g_zoom;
                        double ci_top = g_cy + v_top * g_zoom;
                        float s_top = mandelbrot(cr, ci_top);
                        Color3 c_top = get_color(s_top);

                        double v_bot = (2.0 * (py_top + 1.5) / pixel_h - 1.0);
                        double ci_bot = g_cy + v_bot * g_zoom;
                        float s_bot = mandelbrot(cr, ci_bot);
                        Color3 c_bot = get_color(s_bot);

                        int fi = color_idx(c_top.r, c_top.g, c_top.b);
                        int bi = color_idx(c_bot.r, c_bot.g, c_bot.b);
                        canvas.set(cx, cy, U'\u2580', g_styles[fi][bi]);
                    }
                }
            };

            if (n_threads <= 1 || canvas_h < 4) {
                compute_rows(0, canvas_h);
            } else {
                std::vector<std::jthread> threads;
                threads.reserve(static_cast<size_t>(n_threads));
                int chunk = (canvas_h + n_threads - 1) / n_threads;
                for (int t = 0; t < n_threads; ++t) {
                    int lo = t * chunk;
                    int hi = std::min(lo + chunk, canvas_h);
                    if (lo >= hi) break;
                    threads.emplace_back([=, &canvas] { compute_rows(lo, hi); });
                }
            }

            // Status bar
            for (int x = 0; x < W; ++x)
                canvas.set(x, bar_y, U' ', g_bar_bg);

            static const char* pal_names[] = {
                "ultra", "fire", "ocean", "neon", "gray", "rainbow"
            };

            char buf[256];
            std::snprintf(buf, sizeof(buf),
                " MANDELBROT \xe2\x94\x82 %.4e + %.4ei \xe2\x94\x82 zoom:%.2e \xe2\x94\x82 iter:%d \xe2\x94\x82 fps:%d \xe2\x94\x82 %s%s",
                g_cx, g_cy, 1.0/g_zoom, g_iter_count, fps_display,
                pal_names[g_palette],
                g_auto ? " [auto]" : "");
            canvas.write_text(0, bar_y, buf, g_bar_accent);

            // Help on right side
            const char* help = "[1-6] pal [+-] zoom [spc] auto [r] reset [q] quit";
            int help_len = static_cast<int>(std::strlen(help));
            if (W > help_len + 2)
                canvas.write_text(W - help_len - 1, bar_y, help, g_bar_dim);
        }
    );
}
