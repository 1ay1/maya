// maya -- Matrix digital rain
//
// Classic Matrix-style falling green characters (katakana, digits, latin)
// cascading down the screen with fading trails and character mutation.
//
// Keys: 1-4=color mode  m=message reveal  space=pause  q/Esc=quit

#include <maya/internal.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace maya;

// -- Character set -----------------------------------------------------------

static constexpr char32_t CHARSET[] = {
    // Half-width katakana U+FF66..U+FF9D
    0xFF66, 0xFF67, 0xFF68, 0xFF69, 0xFF6A, 0xFF6B, 0xFF6C, 0xFF6D,
    0xFF6E, 0xFF6F, 0xFF70, 0xFF71, 0xFF72, 0xFF73, 0xFF74, 0xFF75,
    0xFF76, 0xFF77, 0xFF78, 0xFF79, 0xFF7A, 0xFF7B, 0xFF7C, 0xFF7D,
    0xFF7E, 0xFF7F, 0xFF80, 0xFF81, 0xFF82, 0xFF83, 0xFF84, 0xFF85,
    0xFF86, 0xFF87, 0xFF88, 0xFF89, 0xFF8A, 0xFF8B, 0xFF8C, 0xFF8D,
    0xFF8E, 0xFF8F, 0xFF90, 0xFF91, 0xFF92, 0xFF93, 0xFF94, 0xFF95,
    0xFF96, 0xFF97, 0xFF98, 0xFF99, 0xFF9A, 0xFF9B, 0xFF9C, 0xFF9D,
    // Digits
    U'0', U'1', U'2', U'3', U'4', U'5', U'6', U'7', U'8', U'9',
    // Latin
    U'A', U'B', U'C', U'D', U'E', U'F', U'G', U'H', U'I', U'J',
    U'K', U'L', U'M', U'N', U'O', U'P', U'Q', U'R', U'S', U'T',
    U'U', U'V', U'W', U'X', U'Y', U'Z',
};
static constexpr int CHARSET_SIZE = sizeof(CHARSET) / sizeof(CHARSET[0]);

// -- Constants ---------------------------------------------------------------

static constexpr int BRIGHTNESS_LEVELS = 32;
static constexpr int TRAIL_LENGTH      = 24;
static constexpr int MIN_SPEED         = 1;
static constexpr int MAX_SPEED         = 4;
static constexpr int MIN_GAP           = 4;
static constexpr int MAX_GAP           = 30;

// -- Color modes -------------------------------------------------------------

enum class ColorMode { Classic, MultiColor, RedPill, Rainbow };

static const char* mode_name(ColorMode m) {
    switch (m) {
        case ColorMode::Classic:    return "CLASSIC";
        case ColorMode::MultiColor: return "MULTI-COLOR";
        case ColorMode::RedPill:    return "RED PILL";
        case ColorMode::Rainbow:    return "RAINBOW";
    }
    return "";
}

// -- HSV to RGB --------------------------------------------------------------

struct RGB { uint8_t r, g, b; };

static RGB hsv_to_rgb(float h, float s, float v) {
    float c = v * s;
    float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float r1, g1, b1;
    if (h < 60)       { r1 = c; g1 = x; b1 = 0; }
    else if (h < 120) { r1 = x; g1 = c; b1 = 0; }
    else if (h < 180) { r1 = 0; g1 = c; b1 = x; }
    else if (h < 240) { r1 = 0; g1 = x; b1 = c; }
    else if (h < 300) { r1 = x; g1 = 0; b1 = c; }
    else               { r1 = c; g1 = 0; b1 = x; }
    return {
        static_cast<uint8_t>((r1 + m) * 255),
        static_cast<uint8_t>((g1 + m) * 255),
        static_cast<uint8_t>((b1 + m) * 255),
    };
}

// -- Stream (one falling column) ---------------------------------------------

struct Stream {
    float    y_pos;
    int      speed;
    int      gap_remaining;
    int      trail_len;
    int      hue_offset;
    std::vector<char32_t> chars;
};

// -- Global state ------------------------------------------------------------

static std::mt19937 g_rng{std::random_device{}()};
static int g_w = 0, g_h = 0;
static std::vector<Stream> g_streams;
static ColorMode g_mode = ColorMode::Classic;
static bool g_paused = false;
static int g_frame = 0;

// Message reveal
static bool g_msg_active = false;
static int  g_msg_timer  = 0;
static constexpr int MSG_DURATION = 90;
static const char* MSG_TEXT = "WAKE UP NEO";

// Styles
static uint16_t g_green_styles[BRIGHTNESS_LEVELS];
static uint16_t g_head_style;
static uint16_t g_bar_bg, g_bar_dim, g_bar_accent;
static uint16_t g_black_style;
static uint16_t g_msg_style;

static constexpr int NUM_HUES = 6;
static constexpr float HUES[NUM_HUES] = {120.0f, 180.0f, 270.0f, 300.0f, 90.0f, 160.0f};
static uint16_t g_hue_styles[NUM_HUES][BRIGHTNESS_LEVELS];
static uint16_t g_hue_head_styles[NUM_HUES];

static uint16_t g_red_styles[BRIGHTNESS_LEVELS];
static uint16_t g_red_head;

static constexpr int RAINBOW_HUES = 64;
static uint16_t g_rainbow_styles[RAINBOW_HUES][BRIGHTNESS_LEVELS];
static uint16_t g_rainbow_head[RAINBOW_HUES];

// -- Random helpers ----------------------------------------------------------

static char32_t rand_char() {
    return CHARSET[std::uniform_int_distribution<int>(0, CHARSET_SIZE - 1)(g_rng)];
}

static int rand_range(int lo, int hi) {
    return std::uniform_int_distribution<int>(lo, hi)(g_rng);
}

// -- Stream management -------------------------------------------------------

static void init_stream(Stream& s, int col_height) {
    s.y_pos = static_cast<float>(-rand_range(0, col_height));
    s.speed = rand_range(MIN_SPEED, MAX_SPEED);
    s.gap_remaining = 0;
    s.trail_len = rand_range(TRAIL_LENGTH / 2, TRAIL_LENGTH);
    s.hue_offset = rand_range(0, NUM_HUES - 1);
    s.chars.resize(col_height);
    for (auto& c : s.chars) c = rand_char();
}

static void reset_stream(Stream& s, int col_height) {
    s.y_pos = static_cast<float>(-rand_range(2, 12));
    s.speed = rand_range(MIN_SPEED, MAX_SPEED);
    s.gap_remaining = rand_range(MIN_GAP, MAX_GAP);
    s.trail_len = rand_range(TRAIL_LENGTH / 2, TRAIL_LENGTH);
    s.hue_offset = rand_range(0, NUM_HUES - 1);
    for (auto& c : s.chars) c = rand_char();
}

// -- Resize ------------------------------------------------------------------

static void rebuild(StylePool& pool, int w, int h) {
    g_w = w;
    g_h = h;

    g_black_style = pool.intern(Style{}.with_fg(Color::rgb(0, 0, 0)).with_bg(Color::rgb(0, 0, 0)));

    // Classic green palette
    for (int i = 0; i < BRIGHTNESS_LEVELS; ++i) {
        float t = static_cast<float>(i) / (BRIGHTNESS_LEVELS - 1);
        uint8_t g = static_cast<uint8_t>(30 + t * 225);
        uint8_t r = static_cast<uint8_t>(t * t * 80);
        g_green_styles[i] = pool.intern(
            Style{}.with_fg(Color::rgb(r, g, 0)).with_bg(Color::rgb(0, 0, 0)));
    }
    g_head_style = pool.intern(
        Style{}.with_fg(Color::rgb(220, 255, 220)).with_bg(Color::rgb(0, 0, 0)).with_bold());

    // Multi-color palettes
    for (int h_idx = 0; h_idx < NUM_HUES; ++h_idx) {
        for (int i = 0; i < BRIGHTNESS_LEVELS; ++i) {
            float t = static_cast<float>(i) / (BRIGHTNESS_LEVELS - 1);
            auto c = hsv_to_rgb(HUES[h_idx], 0.8f, 0.15f + t * 0.85f);
            g_hue_styles[h_idx][i] = pool.intern(
                Style{}.with_fg(Color::rgb(c.r, c.g, c.b)).with_bg(Color::rgb(0, 0, 0)));
        }
        auto hc = hsv_to_rgb(HUES[h_idx], 0.2f, 1.0f);
        g_hue_head_styles[h_idx] = pool.intern(
            Style{}.with_fg(Color::rgb(hc.r, hc.g, hc.b)).with_bg(Color::rgb(0, 0, 0)).with_bold());
    }

    // Red pill palette
    for (int i = 0; i < BRIGHTNESS_LEVELS; ++i) {
        float t = static_cast<float>(i) / (BRIGHTNESS_LEVELS - 1);
        uint8_t r = static_cast<uint8_t>(30 + t * 225);
        uint8_t g = static_cast<uint8_t>(t * t * 40);
        g_red_styles[i] = pool.intern(
            Style{}.with_fg(Color::rgb(r, g, 0)).with_bg(Color::rgb(0, 0, 0)));
    }
    g_red_head = pool.intern(
        Style{}.with_fg(Color::rgb(255, 200, 200)).with_bg(Color::rgb(0, 0, 0)).with_bold());

    // Rainbow palette
    for (int hi = 0; hi < RAINBOW_HUES; ++hi) {
        float hue = (360.0f * hi) / RAINBOW_HUES;
        for (int i = 0; i < BRIGHTNESS_LEVELS; ++i) {
            float t = static_cast<float>(i) / (BRIGHTNESS_LEVELS - 1);
            auto c = hsv_to_rgb(hue, 0.85f, 0.12f + t * 0.88f);
            g_rainbow_styles[hi][i] = pool.intern(
                Style{}.with_fg(Color::rgb(c.r, c.g, c.b)).with_bg(Color::rgb(0, 0, 0)));
        }
        auto hc = hsv_to_rgb(hue, 0.15f, 1.0f);
        g_rainbow_head[hi] = pool.intern(
            Style{}.with_fg(Color::rgb(hc.r, hc.g, hc.b)).with_bg(Color::rgb(0, 0, 0)).with_bold());
    }

    // Message style
    g_msg_style = pool.intern(
        Style{}.with_fg(Color::rgb(255, 255, 255)).with_bg(Color::rgb(0, 0, 0)).with_bold());

    // Status bar
    g_bar_bg     = pool.intern(Style{}.with_bg(Color::rgb(10, 10, 10)).with_fg(Color::rgb(100, 100, 100)));
    g_bar_dim    = pool.intern(Style{}.with_bg(Color::rgb(10, 10, 10)).with_fg(Color::rgb(60, 60, 60)));
    g_bar_accent = pool.intern(Style{}.with_bg(Color::rgb(10, 10, 10)).with_fg(Color::rgb(0, 200, 0)).with_bold());

    // Two streams per column for dense rain
    int num_streams = w * 2;
    g_streams.resize(num_streams);
    int rain_h = h - 1;
    for (int i = 0; i < num_streams; ++i) {
        init_stream(g_streams[i], std::max(rain_h, 1));
        if (i >= w)
            g_streams[i].y_pos -= static_cast<float>(rand_range(0, std::max(rain_h, 1)));
    }
}

// -- Events ------------------------------------------------------------------

static bool handle(const Event& ev) {
    if (key(ev, 'q') || key(ev, SpecialKey::Escape)) return false;

    on(ev, ' ', [] { g_paused = !g_paused; });
    on(ev, 'm', [] { g_msg_active = true; g_msg_timer = MSG_DURATION; });
    on(ev, '1', [] { g_mode = ColorMode::Classic; });
    on(ev, '2', [] { g_mode = ColorMode::MultiColor; });
    on(ev, '3', [] { g_mode = ColorMode::RedPill; });
    on(ev, '4', [] { g_mode = ColorMode::Rainbow; });

    return true;
}

// -- Style lookup by mode ----------------------------------------------------

static uint16_t trail_style(int col, int brightness, int hue_offset) {
    int b = std::clamp(brightness, 0, BRIGHTNESS_LEVELS - 1);
    switch (g_mode) {
        case ColorMode::Classic:
            return g_green_styles[b];
        case ColorMode::MultiColor:
            return g_hue_styles[hue_offset % NUM_HUES][b];
        case ColorMode::RedPill:
            return g_red_styles[b];
        case ColorMode::Rainbow: {
            int hi = ((col * RAINBOW_HUES / std::max(g_w, 1)) + g_frame / 3) % RAINBOW_HUES;
            return g_rainbow_styles[hi][b];
        }
    }
    return g_green_styles[b];
}

static uint16_t get_head_style(int col, int hue_offset) {
    switch (g_mode) {
        case ColorMode::Classic:    return g_head_style;
        case ColorMode::MultiColor: return g_hue_head_styles[hue_offset % NUM_HUES];
        case ColorMode::RedPill:    return g_red_head;
        case ColorMode::Rainbow: {
            int hi = ((col * RAINBOW_HUES / std::max(g_w, 1)) + g_frame / 3) % RAINBOW_HUES;
            return g_rainbow_head[hi];
        }
    }
    return g_head_style;
}

// -- Paint -------------------------------------------------------------------

static void paint(Canvas& canvas, int w, int h) {
    if (w != g_w || h != g_h) return;

    int rain_h = h - 1;
    if (rain_h <= 0) return;

    // Fill with black
    for (int y = 0; y < rain_h; ++y)
        for (int x = 0; x < w; ++x)
            canvas.set(x, y, U' ', g_black_style);

    if (!g_paused) {
        ++g_frame;
        if (g_msg_active) {
            --g_msg_timer;
            if (g_msg_timer <= 0) g_msg_active = false;
        }
    }

    int num_streams = static_cast<int>(g_streams.size());

    for (int si = 0; si < num_streams; ++si) {
        auto& s = g_streams[si];
        int col = si % w;

        if (!g_paused) {
            if (s.gap_remaining > 0) {
                --s.gap_remaining;
                continue;
            }
            s.y_pos += static_cast<float>(s.speed);

            // Mutate random trail characters
            if (rand_range(0, 3) == 0) {
                int mutate_row = static_cast<int>(s.y_pos) - rand_range(1, s.trail_len);
                if (mutate_row >= 0 && mutate_row < rain_h)
                    s.chars[mutate_row] = rand_char();
            }

            // Reset if fully off screen
            if (static_cast<int>(s.y_pos) - s.trail_len > rain_h) {
                reset_stream(s, rain_h);
                continue;
            }
        } else if (s.gap_remaining > 0) {
            continue;
        }

        int head_y = static_cast<int>(s.y_pos);

        for (int i = 0; i <= s.trail_len; ++i) {
            int y = head_y - i;
            if (y < 0 || y >= rain_h) continue;

            if (i == 0) {
                canvas.set(col, y, s.chars[y], get_head_style(col, s.hue_offset));
            } else {
                float fade = 1.0f - static_cast<float>(i) / static_cast<float>(s.trail_len);
                int brightness = static_cast<int>(fade * (BRIGHTNESS_LEVELS - 1));
                canvas.set(col, y, s.chars[y], trail_style(col, brightness, s.hue_offset));
            }
        }
    }

    // Message reveal effect
    if (g_msg_active && g_msg_timer > 0) {
        int msg_len = static_cast<int>(std::strlen(MSG_TEXT));
        int mx = (w - msg_len) / 2;
        int my = h / 2;

        float progress = static_cast<float>(g_msg_timer) / MSG_DURATION;
        float alpha = (progress > 0.5f) ? (1.0f - progress) * 2.0f : progress * 2.0f;
        alpha = std::clamp(alpha, 0.0f, 1.0f);

        if (alpha > 0.15f) {
            for (int i = 0; i < msg_len; ++i) {
                int x = mx + i;
                if (x >= 0 && x < w && my >= 0 && my < rain_h) {
                    char32_t ch = static_cast<char32_t>(MSG_TEXT[i]);
                    if (ch == U' ') continue;
                    if (alpha > 0.4f)
                        canvas.set(x, my, ch, g_msg_style);
                    else
                        canvas.set(x, my, (rand_range(0, 3) == 0) ? ch : rand_char(), g_msg_style);
                }
            }
        }
    }

    // Status bar
    int bar_y = h - 1;
    for (int x = 0; x < w; ++x)
        canvas.set(x, bar_y, U' ', g_bar_bg);

    const char* bar = "MATRIX \xe2\x94\x82 [1-4] mode \xe2\x94\x82 [m] message \xe2\x94\x82 [space] pause \xe2\x94\x82 [q] quit";
    canvas.write_text(1, bar_y, bar, g_bar_dim);
    canvas.write_text(1, bar_y, "MATRIX", g_bar_accent);

    const char* mname = mode_name(g_mode);
    int mlen = static_cast<int>(std::strlen(mname));
    if (w > mlen + 2)
        canvas.write_text(w - mlen - 1, bar_y, mname, g_bar_accent);

    if (g_paused) {
        int px = (w - 6) / 2;
        if (px > 0)
            canvas.write_text(px, bar_y, "PAUSED", g_bar_accent);
    }
}

// -- Main --------------------------------------------------------------------

int main() {
    (void)canvas_run(
        CanvasConfig{.fps = 30, .mouse = false, .mode = Mode::Fullscreen, .title = "matrix"},
        rebuild,
        handle,
        paint
    );
}
