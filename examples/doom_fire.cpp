// maya -- Doom PSX fire effect
//
// The classic fire propagation algorithm: a 2D heat grid where each pixel
// copies from the pixel below minus random decay, producing convincing
// flames. Color palette goes black -> red -> orange -> yellow -> white.
//
// Keys: q/Esc=quit  space=toggle fire  left/right=wind

#include <maya/maya.hpp>

#include <cstdint>
#include <random>
#include <vector>

using namespace maya;

// -- Constants ---------------------------------------------------------------

static constexpr int MAX_HEAT = 36;

// -- State -------------------------------------------------------------------

static std::mt19937 g_rng{42};
static std::vector<uint8_t> g_fire;
static int g_w = 0, g_h = 0;
static bool g_source = true;
static int g_wind = 0;

// Style IDs: one per heat level, plus status bar styles
static uint16_t g_heat_style[MAX_HEAT + 1];
static uint16_t g_bar_bg;
static uint16_t g_bar_dim;
static uint16_t g_bar_accent;

// -- Palette -----------------------------------------------------------------

static Color heat_color(int h) {
    h = std::clamp(h, 0, MAX_HEAT);
    // 0-6:   black to dark red
    if (h <= 6) {
        uint8_t r = static_cast<uint8_t>(h * 36);  // 0 -> 216
        return Color::rgb(r, 0, 0);
    }
    // 7-15:  dark red to red
    if (h <= 15) {
        float t = float(h - 7) / 8.0f;
        uint8_t r = static_cast<uint8_t>(216 + t * 39);  // 216 -> 255
        return Color::rgb(r, 0, 0);
    }
    // 16-23: red to orange (green rises)
    if (h <= 23) {
        float t = float(h - 16) / 7.0f;
        uint8_t g = static_cast<uint8_t>(t * 165);  // 0 -> 165
        return Color::rgb(255, g, 0);
    }
    // 24-30: orange to yellow (green catches up)
    if (h <= 30) {
        float t = float(h - 24) / 6.0f;
        uint8_t g = static_cast<uint8_t>(165 + t * 90);  // 165 -> 255
        return Color::rgb(255, g, 0);
    }
    // 31-36: yellow to white (blue rises)
    float t = float(h - 31) / 5.0f;
    uint8_t b = static_cast<uint8_t>(t * 255);
    return Color::rgb(255, 255, b);
}

// -- Resize ------------------------------------------------------------------

static void rebuild(StylePool& pool, int w, int h) {
    g_w = w;
    g_h = h;
    g_fire.assign(w * h, 0);

    // Ignite bottom row
    if (g_source) {
        for (int x = 0; x < w; ++x)
            g_fire[(h - 1) * w + x] = MAX_HEAT;
    }

    // Intern heat palette
    for (int i = 0; i <= MAX_HEAT; ++i)
        g_heat_style[i] = pool.intern(Style{}.with_bg(heat_color(i)));

    // Status bar styles
    g_bar_bg     = pool.intern(Style{}.with_bg(Color::rgb(20, 20, 20)).with_fg(Color::rgb(120, 120, 120)));
    g_bar_dim    = pool.intern(Style{}.with_bg(Color::rgb(20, 20, 20)).with_fg(Color::rgb(80, 80, 80)));
    g_bar_accent = pool.intern(Style{}.with_bg(Color::rgb(20, 20, 20)).with_fg(Color::rgb(255, 100, 30)).with_bold());
}

// -- Event -------------------------------------------------------------------

static bool handle(const Event& ev) {
    if (key(ev, 'q') || key(ev, SpecialKey::Escape)) return false;

    on(ev, ' ', [] {
        g_source = !g_source;
        if (!g_source) {
            for (int x = 0; x < g_w; ++x)
                g_fire[(g_h - 1) * g_w + x] = 0;
        } else {
            for (int x = 0; x < g_w; ++x)
                g_fire[(g_h - 1) * g_w + x] = MAX_HEAT;
        }
    });

    on(ev, SpecialKey::Right, [] { g_wind = std::min(g_wind + 1, 3); });
    on(ev, SpecialKey::Left,  [] { g_wind = std::max(g_wind - 1, -3); });

    return true;
}

// -- Paint -------------------------------------------------------------------

static void paint(Canvas& canvas, int w, int h) {
    if (w != g_w || h != g_h) return;

    int fire_h = h - 1;  // reserve bottom row for status bar

    // Propagate fire upward
    std::uniform_int_distribution<int> decay_dist(0, 3);
    std::uniform_int_distribution<int> wind_dist(0, std::abs(g_wind));

    for (int y = 0; y < fire_h - 1; ++y) {
        for (int x = 0; x < w; ++x) {
            int src_y = y + 1;
            int wind_offset = (g_wind == 0) ? 0
                : (g_wind > 0 ? wind_dist(g_rng) : -wind_dist(g_rng));
            int src_x = std::clamp(x + wind_offset, 0, w - 1);

            int decay = decay_dist(g_rng);
            int heat = g_fire[src_y * w + src_x] - decay;
            if (heat < 0) heat = 0;

            g_fire[y * w + x] = static_cast<uint8_t>(heat);
        }
    }

    // Maintain source row
    if (g_source) {
        for (int x = 0; x < w; ++x)
            g_fire[(fire_h - 1) * w + x] = MAX_HEAT;
    }

    // Render fire pixels
    for (int y = 0; y < fire_h; ++y) {
        for (int x = 0; x < w; ++x) {
            int heat = g_fire[y * w + x];
            canvas.set(x, y, U' ', g_heat_style[heat]);
        }
    }

    // Status bar
    int bar_y = h - 1;
    for (int x = 0; x < w; ++x)
        canvas.set(x, bar_y, U' ', g_bar_bg);

    const char* bar = "DOOM FIRE \xe2\x94\x82 [space] toggle \xe2\x94\x82 [\xe2\x86\x90/\xe2\x86\x92] wind \xe2\x94\x82 [q] quit";
    canvas.write_text(1, bar_y, bar, g_bar_dim);
    canvas.write_text(1, bar_y, "DOOM FIRE", g_bar_accent);

    // Wind indicator on the right side
    if (g_wind != 0) {
        char wind_buf[16];
        std::snprintf(wind_buf, sizeof(wind_buf), "wind:%+d", g_wind);
        int len = static_cast<int>(std::strlen(wind_buf));
        if (w > len + 2)
            canvas.write_text(w - len - 1, bar_y, wind_buf, g_bar_accent);
    }
}

// -- Main --------------------------------------------------------------------

int main() {
    (void)canvas_run(
        CanvasConfig{.fps = 60, .title = "doom fire"},
        rebuild,
        handle,
        paint
    );
}
