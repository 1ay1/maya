// maya — Matrix digital rain
//
// Authentic Matrix falling-code effect with multiple visual modes,
// message injection ("wake up, Neo..."), glitch bursts, wave pulses,
// and a hacker-style status bar with live stats.
//
// Keys: q/Esc=quit  +/-=speed  space=pause  1-5=color mode
//       d=density  r=reset  m=inject message  g=glitch  w=wave

#include <maya/maya.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace maya;
using namespace maya::dsl;

// ── Glyph sets ─────────────────────────────────────────────────────────────

static const char* g_katakana[] = {
    "ア", "イ", "ウ", "エ", "オ", "カ", "キ", "ク", "ケ", "コ",
    "サ", "シ", "ス", "セ", "ソ", "タ", "チ", "ツ", "テ", "ト",
    "ナ", "ニ", "ヌ", "ネ", "ノ", "ハ", "ヒ", "フ", "ヘ", "ホ",
    "マ", "ミ", "ム", "メ", "モ", "ヤ", "ユ", "ヨ",
    "ラ", "リ", "ル", "レ", "ロ", "ワ", "ヲ", "ン",
};
static constexpr int NUM_KATA = sizeof(g_katakana) / sizeof(g_katakana[0]);

static const char* g_symbols[] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "A", "B", "C", "D", "E", "F",
    ":", ".", "=", "*", "+", "-", "<", ">", "~",
    "!", "@", "#", "$", "%", "^", "&",
    "{", "}", "[", "]", "(", ")",
    "/", "\\", "|", "?",
};
static constexpr int NUM_SYM = sizeof(g_symbols) / sizeof(g_symbols[0]);

static const char* glyph(int idx) {
    // Mix katakana and symbols for variety
    if (idx < NUM_KATA) return g_katakana[idx];
    return g_symbols[(idx - NUM_KATA) % NUM_SYM];
}
static constexpr int NUM_GLYPHS = NUM_KATA + NUM_SYM;

// ── Color modes ────────────────────────────────────────────────────────────

enum class ColorMode { Green, RedPill, BluePill, Gold, Phantom };

struct Palette {
    const char* name;
    const char* icon;
    uint8_t hr, hg, hb;      // head
    uint8_t br, bg_, bb;     // bright (just behind head)
    uint8_t tr, tg, tb;      // trail start
    uint8_t er, eg, eb;      // trail end (faded)
};

static constexpr Palette g_palettes[] = {
    {"CLASSIC",   ">>",  255,255,255,  180,255,180,  20,200,0,    2,30,0},
    {"RED PILL",  "@@",  255,220,220,  255,120,120,  200,30,30,   40,5,5},
    {"BLUE PILL", "~~",  220,220,255,  120,160,255,  30,80,220,   5,10,50},
    {"GOLDEN",    "$$",  255,255,200,  255,220,100,  200,160,20,  50,30,0},
    {"PHANTOM",   "..",  255,255,255,  200,200,220,  120,100,160, 20,15,30},
};

// ── Per-column rain stream ─────────────────────────────────────────────────

struct Stream {
    float   y       = 0.f;
    float   speed   = 1.f;
    int     length  = 10;
    std::vector<int> glyphs;
    bool    is_message = false;     // column carries an injected message
    int     msg_col    = -1;        // which char of the message to show at head
};

// ── Message injection ──────────────────────────────────────────────────────

static const char* g_messages[] = {
    "WAKE UP NEO...",
    "THE MATRIX HAS YOU",
    "FOLLOW THE WHITE RABBIT",
    "KNOCK KNOCK NEO",
    "THERE IS NO SPOON",
    "FREE YOUR MIND",
    "I KNOW KUNG FU",
    "WELCOME TO THE REAL WORLD",
    "THE ONE",
    "DEJA VU",
    "SYSTEM FAILURE",
    "TRACE PROGRAM RUNNING",
    "CALL TRANS OPT: RECEIVED",
};
static constexpr int NUM_MSGS = sizeof(g_messages) / sizeof(g_messages[0]);

struct MessageInject {
    const char* text = nullptr;
    int   row   = 0;
    int   col   = 0;        // starting column in the grid
    float timer = 0.f;      // chars revealed so far (fractional)
    float speed = 12.f;     // chars per second
    float hold  = 0.f;      // time held after fully revealed
    float fade  = 0.f;      // fade-out timer
};

// ── Glitch burst ───────────────────────────────────────────────────────────

struct GlitchBurst {
    float timer  = 0.f;
    float dur    = 0.3f;
    int   center_col = 0;
    int   center_row = 0;
    int   radius = 8;
};

// ── Wave pulse ─────────────────────────────────────────────────────────────

struct WavePulse {
    float radius = 0.f;
    float speed  = 40.f;
    int   cx, cy;
    float life = 0.f;
};

// ── Global state ───────────────────────────────────────────────────────────

static std::mt19937 g_rng{42};
static std::vector<Stream> g_streams;
static int g_cols = 0, g_rows = 0;
static float g_speed_mult = 1.0f;
static bool g_paused = false;
static ColorMode g_mode = ColorMode::Green;
static int g_density = 3;
static float g_elapsed = 0.f;
static int g_frame = 0;
static long long g_total_glyphs = 0;  // total glyphs rendered

static std::vector<MessageInject> g_messages_active;
static std::vector<GlitchBurst> g_glitches;
static std::vector<WavePulse> g_waves;

static int g_msg_index = 0;
static bool g_show_bar = true;

// ── Stream init ────────────────────────────────────────────────────────────

static void init_stream(Stream& s, int rows, bool from_top) {
    std::uniform_real_distribution<float> speed_dist(4.f, 18.f);
    std::uniform_int_distribution<int> len_dist(6, std::max(7, rows));
    std::uniform_real_distribution<float> delay_dist(0.f, 3.f);
    std::uniform_int_distribution<int> glyph_dist(0, NUM_GLYPHS - 1);

    s.speed  = speed_dist(g_rng);
    s.length = len_dist(g_rng);
    float delay = from_top ? delay_dist(g_rng) : 0.f;
    s.y = from_top ? -static_cast<float>(s.length) - delay * s.speed
                   : -static_cast<float>(s.length);
    s.is_message = false;
    s.msg_col = -1;

    s.glyphs.resize(rows + s.length + 20);
    for (auto& g : s.glyphs) g = glyph_dist(g_rng);
}

static int stream_count(int cols) {
    int base = cols / 2;
    switch (g_density) {
        case 1: return std::max(1, base / 2);
        case 3: return base;
        default: return std::max(1, base * 3 / 4);
    }
}

static void resize(int cols, int rows) {
    int want = stream_count(cols);
    if (cols == g_cols && rows == g_rows && static_cast<int>(g_streams.size()) == want) return;
    g_cols = cols;
    g_rows = rows;
    g_streams.resize(want);
    for (auto& s : g_streams) init_stream(s, rows, true);
}

static void inject_message() {
    if (g_rows < 3 || g_cols < 10) return;
    const char* msg = g_messages[g_msg_index % NUM_MSGS];
    g_msg_index++;

    int len = static_cast<int>(std::strlen(msg));
    int max_col = std::max(0, static_cast<int>(g_streams.size()) - len - 2);
    int col = std::uniform_int_distribution<int>(1, std::max(1, max_col))(g_rng);
    int row = std::uniform_int_distribution<int>(g_rows / 4, g_rows * 3 / 4)(g_rng);

    g_messages_active.push_back({
        .text  = msg,
        .row   = row,
        .col   = col,
        .timer = 0.f,
        .speed = 14.f,
        .hold  = 0.f,
        .fade  = 0.f,
    });
}

static void trigger_glitch() {
    int cx = std::uniform_int_distribution<int>(0, std::max(0, static_cast<int>(g_streams.size()) - 1))(g_rng);
    int cy = std::uniform_int_distribution<int>(0, std::max(0, g_rows - 1))(g_rng);
    g_glitches.push_back({.timer = 0.f, .dur = 0.4f, .center_col = cx, .center_row = cy, .radius = 12});
}

static void trigger_wave() {
    int cx = std::uniform_int_distribution<int>(0, std::max(0, static_cast<int>(g_streams.size()) - 1))(g_rng);
    int cy = std::uniform_int_distribution<int>(0, std::max(0, g_rows - 1))(g_rng);
    g_waves.push_back({.radius = 0.f, .speed = 50.f, .cx = cx, .cy = cy, .life = 0.f});
}

// ── Tick ───────────────────────────────────────────────────────────────────

static void tick(float dt) {
    if (g_paused) return;
    g_elapsed += dt;
    g_frame++;

    std::uniform_int_distribution<int> glyph_dist(0, NUM_GLYPHS - 1);

    for (auto& s : g_streams) {
        s.y += s.speed * g_speed_mult * dt;

        // Flicker
        if (!s.glyphs.empty()) {
            // Mutate 1-3 glyphs per frame for heavier flicker
            int mutations = std::uniform_int_distribution<int>(1, 3)(g_rng);
            for (int m = 0; m < mutations; ++m) {
                int idx = std::uniform_int_distribution<int>(0, static_cast<int>(s.glyphs.size()) - 1)(g_rng);
                s.glyphs[idx] = glyph_dist(g_rng);
            }
        }

        if (s.y - s.length > g_rows + 5) {
            init_stream(s, g_rows, false);
        }
    }

    // Update messages
    for (auto& m : g_messages_active) {
        int len = static_cast<int>(std::strlen(m.text));
        if (m.timer < len) {
            m.timer += m.speed * dt;
        } else if (m.hold < 2.0f) {
            m.hold += dt;
        } else {
            m.fade += dt * 2.f;
        }
    }
    std::erase_if(g_messages_active, [](const MessageInject& m) { return m.fade > 1.5f; });

    // Update glitches
    for (auto& g : g_glitches) g.timer += dt;
    std::erase_if(g_glitches, [](const GlitchBurst& g) { return g.timer > g.dur; });

    // Update waves
    for (auto& w : g_waves) { w.radius += w.speed * dt; w.life += dt; }
    std::erase_if(g_waves, [](const WavePulse& w) { return w.life > 3.f; });

    // Auto-inject a message every ~15 seconds
    static float auto_msg_timer = 8.f;
    auto_msg_timer -= dt;
    if (auto_msg_timer <= 0.f) {
        inject_message();
        auto_msg_timer = std::uniform_real_distribution<float>(12.f, 22.f)(g_rng);
    }

    // Occasional random glitch
    static float auto_glitch_timer = 5.f;
    auto_glitch_timer -= dt;
    if (auto_glitch_timer <= 0.f) {
        trigger_glitch();
        auto_glitch_timer = std::uniform_real_distribution<float>(4.f, 10.f)(g_rng);
    }
}

// ── Column rendering ───────────────────────────────────────────────────────

static Element build_column(int col_idx, const Stream& s, int rows) {
    std::vector<Element> cells;
    cells.reserve(rows);

    int head = static_cast<int>(s.y);
    auto& pal = g_palettes[static_cast<int>(g_mode)];

    auto lerp8 = [](uint8_t a, uint8_t b, float t) -> uint8_t {
        return static_cast<uint8_t>(a + (b - a) * std::clamp(t, 0.f, 1.f));
    };

    for (int row = 0; row < rows; ++row) {
        // Check if a message occupies this cell
        bool msg_cell = false;
        char msg_char = 0;
        float msg_brightness = 1.f;
        for (auto& m : g_messages_active) {
            int len = static_cast<int>(std::strlen(m.text));
            int ci = col_idx - m.col;
            if (m.row == row && ci >= 0 && ci < len && ci < static_cast<int>(m.timer)) {
                msg_cell = true;
                msg_char = m.text[ci];
                msg_brightness = m.fade > 0.f ? std::max(0.f, 1.f - m.fade) : 1.f;
                // Typewriter shimmer on newly revealed char
                if (ci == static_cast<int>(m.timer) - 1 && m.timer - static_cast<int>(m.timer) < 0.5f)
                    msg_brightness = 1.2f;
                break;
            }
        }

        if (msg_cell && msg_brightness > 0.01f) {
            char buf[4] = {msg_char, ' ', 0, 0};
            uint8_t b = static_cast<uint8_t>(255 * std::min(msg_brightness, 1.f));
            cells.push_back(
                text(std::string(buf),
                     Style{}.with_fg(Color::rgb(b, b, b)).with_bold()).build());
            g_total_glyphs++;
            continue;
        }

        // Check glitch effect
        bool in_glitch = false;
        for (auto& gb : g_glitches) {
            int dc = col_idx - gb.center_col;
            int dr = row - gb.center_row;
            if (dc * dc + dr * dr < gb.radius * gb.radius) {
                in_glitch = true;
                break;
            }
        }

        // Check wave pulse brightness boost
        float wave_boost = 0.f;
        for (auto& w : g_waves) {
            float dc = static_cast<float>(col_idx - w.cx);
            float dr = static_cast<float>(row - w.cy);
            float dist = std::sqrt(dc * dc + dr * dr);
            float ring_dist = std::abs(dist - w.radius);
            if (ring_dist < 3.f) {
                float intensity = (1.f - ring_dist / 3.f) * std::max(0.f, 1.f - w.life * 0.5f);
                wave_boost = std::max(wave_boost, intensity);
            }
        }

        int dist = head - row;

        if (in_glitch) {
            // Glitch: random bright glyph with inverted/random color
            int gi = std::uniform_int_distribution<int>(0, NUM_GLYPHS - 1)(g_rng);
            const char* g = glyph(gi);
            // Randomly pick between inverse, bright accent, or dim
            int style_pick = std::uniform_int_distribution<int>(0, 3)(g_rng);
            Style sty;
            if (style_pick == 0)
                sty = Style{}.with_fg(Color::rgb(pal.hr, pal.hg, pal.hb)).with_bold().with_inverse();
            else if (style_pick == 1)
                sty = Style{}.with_fg(Color::rgb(pal.br, pal.bg_, pal.bb)).with_bold();
            else if (style_pick == 2)
                sty = Style{}.with_fg(Color::rgb(255, 255, 255)).with_dim();
            else
                sty = Style{}.with_fg(Color::rgb(pal.tr, pal.tg, pal.tb));
            cells.push_back(text(g, sty).build());
            g_total_glyphs++;
            continue;
        }

        if (dist < 0 || dist >= s.length) {
            // Wave can illuminate empty cells with a faint glyph
            if (wave_boost > 0.1f) {
                int gi = (row * 7 + col_idx * 13) % NUM_GLYPHS;
                uint8_t intensity = static_cast<uint8_t>(wave_boost * 80.f);
                uint8_t wr = lerp8(0, pal.tr, wave_boost * 0.4f);
                uint8_t wg = lerp8(0, pal.tg, wave_boost * 0.4f);
                uint8_t wb = lerp8(0, pal.tb, wave_boost * 0.4f);
                (void)intensity;
                cells.push_back(text(glyph(gi), Style{}.with_fg(Color::rgb(wr, wg, wb))).build());
                g_total_glyphs++;
            } else {
                cells.push_back(text("  ").build());
            }
            continue;
        }

        int gi = (row + static_cast<int>(s.y * 0.5f)) % static_cast<int>(s.glyphs.size());
        if (gi < 0) gi += static_cast<int>(s.glyphs.size());
        const char* gl = glyph(s.glyphs[gi]);

        uint8_t fr, fg, fb;
        bool bold = false;

        if (dist == 0) {
            fr = pal.hr; fg = pal.hg; fb = pal.hb;
            bold = true;
        } else if (dist == 1) {
            fr = pal.br; fg = pal.bg_; fb = pal.bb;
        } else {
            float fade = static_cast<float>(dist - 1) / static_cast<float>(s.length - 1);
            fade = std::clamp(fade, 0.f, 1.f);
            fade = fade * fade;
            fr = lerp8(pal.tr, pal.er, fade);
            fg = lerp8(pal.tg, pal.eg, fade);
            fb = lerp8(pal.tb, pal.eb, fade);
        }

        // Apply wave brightness boost
        if (wave_boost > 0.f) {
            fr = lerp8(fr, 255, wave_boost * 0.6f);
            fg = lerp8(fg, 255, wave_boost * 0.6f);
            fb = lerp8(fb, 255, wave_boost * 0.6f);
            if (wave_boost > 0.5f) bold = true;
        }

        auto sty = Style{}.with_fg(Color::rgb(fr, fg, fb));
        if (bold) sty = sty.with_bold();
        cells.push_back(text(gl, sty).build());
        g_total_glyphs++;
    }

    return vstack().gap(0)(std::move(cells));
}

// ── Status bar ─────────────────────────────────────────────────────────────

static std::string format_count(long long n) {
    if (n >= 1'000'000'000) {
        char buf[32]; std::snprintf(buf, sizeof(buf), "%.1fG", n / 1e9); return buf;
    }
    if (n >= 1'000'000) {
        char buf[32]; std::snprintf(buf, sizeof(buf), "%.1fM", n / 1e6); return buf;
    }
    if (n >= 1'000) {
        char buf[32]; std::snprintf(buf, sizeof(buf), "%.1fK", n / 1e3); return buf;
    }
    return std::to_string(n);
}

static Element build_status_bar(int cols) {
    auto& pal = g_palettes[static_cast<int>(g_mode)];
    auto accent = Color::rgb(pal.tr, pal.tg, pal.tb);
    auto bright = Color::rgb(pal.br, pal.bg_, pal.bb);
    auto dim    = Color::rgb(80, 80, 80);
    auto dark   = Color::rgb(40, 40, 40);
    auto vdark  = Color::rgb(25, 25, 25);

    // ── Left: branding + mode chips ──
    auto mode_chip = [&](int idx, const char* label) -> Element {
        bool active = (static_cast<int>(g_mode) == idx);
        auto& p = g_palettes[idx];
        auto c = Color::rgb(p.tr, p.tg, p.tb);
        if (active)
            return text(std::string(" ") + label + " ",
                        Style{}.with_fg(Color::rgb(0, 0, 0)).with_bg(c).with_bold()).build();
        return text(std::string(" ") + label + " ", Style{}.with_fg(Color::rgb(50, 50, 50))).build();
    };

    // Animated spinner for LIVE indicator
    static const char* spinners[] = {">>", "/>", "//", "</", "<<", "\\<", "\\\\", ">\\"};
    int spin_idx = (g_frame / 4) % 8;

    auto left = hstack().gap(0)(
        text(" MATRIX ", Style{}.with_fg(Color::rgb(0, 0, 0)).with_bg(bright).with_bold()).build(),
        text(" ", Style{}).build(),
        mode_chip(0, "1"),
        mode_chip(1, "2"),
        mode_chip(2, "3"),
        mode_chip(3, "4"),
        mode_chip(4, "5"),
        text(" ", Style{}).build(),
        text(pal.name, Style{}.with_fg(accent).with_bold()).build(),
        text(" ", Style{}).build(),
        text(g_paused ? "||" : spinners[spin_idx],
             Style{}.with_fg(g_paused ? Color::rgb(255, 60, 60) : bright).with_bold()).build(),
        text(g_paused ? " PAUSED " : " LIVE ",
             Style{}.with_fg(g_paused ? Color::rgb(255, 60, 60) : bright).with_bold()).build()
    );

    // ── Center: stats ──

    // Speed gauge
    int bars = static_cast<int>(g_speed_mult * 4);
    std::string speed_bar;
    for (int i = 0; i < 16; ++i) {
        if (i < bars) speed_bar += "\xe2\x96\xae";  // filled ▮
        else speed_bar += "\xe2\x96\xaf";             // empty ▯
    }

    // Density
    std::string dns;
    for (int i = 1; i <= 3; ++i) dns += (i <= g_density) ? "\xe2\x96\x88" : "\xe2\x96\x91";

    // Active streams
    int active = 0;
    for (auto& s : g_streams) {
        int head = static_cast<int>(s.y);
        if (head >= 0 && head - s.length < g_rows) active++;
    }

    int fps = g_elapsed > 0.5f ? static_cast<int>(g_frame / g_elapsed) : 0;
    int mins = static_cast<int>(g_elapsed) / 60;
    int secs = static_cast<int>(g_elapsed) % 60;
    char time_buf[16];
    if (mins > 0) std::snprintf(time_buf, sizeof(time_buf), "%d:%02d", mins, secs);
    else std::snprintf(time_buf, sizeof(time_buf), "%ds", secs);

    auto sep = [&]() { return text(" \xe2\x94\x82 ", Style{}.with_fg(vdark)).build(); };

    auto mid = hstack().gap(0)(
        text("SPD", Style{}.with_fg(dim)).build(),
        text(" ", Style{}).build(),
        text(speed_bar, Style{}.with_fg(accent)).build(),
        sep(),
        text("DNS", Style{}.with_fg(dim)).build(),
        text(" ", Style{}).build(),
        text(dns, Style{}.with_fg(accent)).build(),
        sep(),
        text(std::to_string(active), Style{}.with_fg(accent)).build(),
        text("/", Style{}.with_fg(dark)).build(),
        text(std::to_string(g_streams.size()), Style{}.with_fg(dim)).build(),
        sep(),
        text(format_count(g_total_glyphs), Style{}.with_fg(accent)).build(),
        text(" glyphs", Style{}.with_fg(dark)).build(),
        sep(),
        text(std::to_string(fps), Style{}.with_fg(accent)).build(),
        text("fps", Style{}.with_fg(dark)).build(),
        text(" ", Style{}).build(),
        text(std::string(time_buf), Style{}.with_fg(dim)).build()
    );

    // ── Right: keybindings ──
    auto kb = [&](const char* k, const char* label) -> std::vector<Element> {
        std::vector<Element> v;
        v.push_back(text(k, Style{}.with_fg(accent).with_bold()).build());
        v.push_back(text(label, Style{}.with_fg(dim)).build());
        return v;
    };

    auto right = hstack().gap(0)(
        text("[+/-]", Style{}.with_fg(accent).with_bold()).build(),
        text("spd ", Style{}.with_fg(dim)).build(),
        text("[d]", Style{}.with_fg(accent).with_bold()).build(),
        text("dns ", Style{}.with_fg(dim)).build(),
        text("[m]", Style{}.with_fg(accent).with_bold()).build(),
        text("msg ", Style{}.with_fg(dim)).build(),
        text("[g]", Style{}.with_fg(accent).with_bold()).build(),
        text("glitch ", Style{}.with_fg(dim)).build(),
        text("[w]", Style{}.with_fg(accent).with_bold()).build(),
        text("wave ", Style{}.with_fg(dim)).build(),
        text("[spc]", Style{}.with_fg(accent).with_bold()).build(),
        text("pause ", Style{}.with_fg(dim)).build(),
        text("[q]", Style{}.with_fg(accent).with_bold()).build(),
        text("quit ", Style{}.with_fg(dim)).build()
    );

    return hstack().gap(0)(
        std::move(left),
        Element{BoxElement{.layout = {.grow = 1.0f}}},
        std::move(mid),
        Element{BoxElement{.layout = {.grow = 1.0f}}},
        std::move(right)
    );
}

// ── Build the full UI ──────────────────────────────────────────────────────

static Element build_ui(int cols, int rows) {
    int bar_rows = g_show_bar ? 1 : 0;
    int rain_rows = std::max(1, rows - bar_rows);
    resize(cols, rain_rows);

    std::vector<Element> columns;
    columns.reserve(g_streams.size());
    for (int i = 0; i < static_cast<int>(g_streams.size()); ++i) {
        columns.push_back(build_column(i, g_streams[i], rain_rows));
    }

    auto rain = hstack().gap(0)(std::move(columns));

    if (g_show_bar) {
        return vstack().gap(0)(
            vstack().grow(1).gap(0)(std::move(rain)),
            build_status_bar(cols)
        );
    }
    return vstack().gap(0)(
        vstack().grow(1).gap(0)(std::move(rain))
    );
}

// ── Main ───────────────────────────────────────────────────────────────────

int main() {
    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();

    run(
        {.title = "matrix", .fps = 30, .alt_screen = true},

        [](const Event& ev) -> bool {
            if (key(ev, 'q') || key(ev, SpecialKey::Escape)) return false;
            on(ev, '+', [] { g_speed_mult = std::min(g_speed_mult + 0.25f, 4.f); });
            on(ev, '=', [] { g_speed_mult = std::min(g_speed_mult + 0.25f, 4.f); });
            on(ev, '-', [] { g_speed_mult = std::max(g_speed_mult - 0.25f, 0.25f); });
            on(ev, ' ', [] { g_paused = !g_paused; });
            on(ev, '1', [] { g_mode = ColorMode::Green; });
            on(ev, '2', [] { g_mode = ColorMode::RedPill; });
            on(ev, '3', [] { g_mode = ColorMode::BluePill; });
            on(ev, '4', [] { g_mode = ColorMode::Gold; });
            on(ev, '5', [] { g_mode = ColorMode::Phantom; });
            on(ev, 'd', [] { g_density = (g_density % 3) + 1; g_cols = 0; });
            on(ev, 'm', [] { inject_message(); });
            on(ev, 'g', [] { trigger_glitch(); });
            on(ev, 'w', [] { trigger_wave(); });
            on(ev, 'h', [] { g_show_bar = !g_show_bar; });
            on(ev, 'r', [] {
                g_cols = 0;
                g_elapsed = 0.f;
                g_frame = 0;
                g_total_glyphs = 0;
                g_speed_mult = 1.0f;
                g_paused = false;
                g_messages_active.clear();
                g_glitches.clear();
                g_waves.clear();
            });
            return true;
        },

        [&](const Ctx& ctx) -> Element {
            auto now = Clock::now();
            float dt = std::chrono::duration<float>(now - last).count();
            last = now;
            tick(dt);
            return build_ui(ctx.size.width.raw(), ctx.size.height.raw());
        }
    );
}
