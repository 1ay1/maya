// maya — interactive Matrix digital rain
//
// Mouse: move = highlight column | click = shockwave | scroll = speed
// Keys:  q/ESC=quit  p=pause  g=glitch storm  t=cycle theme
//        space=jolt all drops  r=reset  +/-=speed

#include <maya/maya.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <iostream>
#include <poll.h>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace maya;
using Clock = std::chrono::steady_clock;

static volatile sig_atomic_t g_resize = 0;
static void on_sigwinch(int) noexcept { g_resize = 1; }

// ── Glyphs ────────────────────────────────────────────────────────────────────

static constexpr char32_t kGlyphs[] = {
    U'ｦ', U'ｧ', U'ｨ', U'ｩ', U'ｪ', U'ｫ', U'ｬ', U'ｭ', U'ｮ', U'ｯ',
    U'ｰ', U'ｱ', U'ｲ', U'ｳ', U'ｴ', U'ｵ', U'ｶ', U'ｷ', U'ｸ', U'ｹ',
    U'ｺ', U'ｻ', U'ｼ', U'ｽ', U'ｾ', U'ｿ', U'ﾀ', U'ﾁ', U'ﾂ', U'ﾃ',
    U'ﾄ', U'ﾅ', U'ﾆ', U'ﾇ', U'ﾈ', U'ﾉ', U'ﾊ', U'ﾋ', U'ﾌ', U'ﾍ',
    U'0', U'1', U'2', U'3', U'4', U'5', U'6', U'7', U'8', U'9',
};
static constexpr int kGlyphCount = static_cast<int>(std::size(kGlyphs));
static constexpr int kMaxTrail   = 28;
using GradientIDs = std::array<uint16_t, kMaxTrail>;

// ── Colour themes ─────────────────────────────────────────────────────────────
// Each theme: head (near-white tinted) | bright (vivid second cell) | trail mid

struct ThemeRGB { uint8_t r, g, b; };
struct ThemeDef {
    const char* name;
    ThemeRGB head, bright, trail;
};

static constexpr ThemeDef kThemes[] = {
    { "green",  {210,255,215}, {  0,255, 70}, {  0,170, 38} },
    { "red",    {255,215,215}, {255, 60, 60}, {185, 22, 22} },
    { "blue",   {215,225,255}, { 80,140,255}, { 28, 65,205} },
    { "cyan",   {210,255,255}, { 40,255,240}, { 12,160,180} },
    { "gold",   {255,250,200}, {255,210,  0}, {185,135,  0} },
    { "purple", {240,210,255}, {190, 60,255}, {100, 12,185} },
    { "white",  {255,255,255}, {225,225,225}, {130,130,130} },
};
static constexpr int kThemeCount = static_cast<int>(std::size(kThemes));

// ── Pre-interned style table ──────────────────────────────────────────────────
// Every style ID lives here. The StylePool never grows after intern_all().

struct Styles {
    std::array<GradientIDs, kThemeCount> theme;   // [t][depth] — normal rain
    std::array<uint16_t,    kThemeCount> hover;   // hover-column head per theme
    uint16_t flash;          // ripple shockwave flash (bright white)
    uint16_t glitch;         // glitch-storm head (inverse white)
    uint16_t bar_bg;         // status bar background cells
    uint16_t bar_label;      // "maya" text
    uint16_t bar_theme;      // theme name (tinted to current theme)
    uint16_t bar_info;       // fps / speed text
    uint16_t bar_mode;       // PAUSED / GLITCH indicator
};

static Styles intern_all(StylePool& pool)
{
    Styles s{};

    for (int t = 0; t < kThemeCount; ++t) {
        const auto& c = kThemes[t];

        s.theme[t][0] = pool.intern(Style{}.with_bold().with_fg(
            Color::rgb(c.head.r, c.head.g, c.head.b)));

        s.theme[t][1] = pool.intern(Style{}.with_bold().with_fg(
            Color::rgb(c.bright.r, c.bright.g, c.bright.b)));

        for (int d = 2; d < kMaxTrail; ++d) {
            float f  = std::pow(1.0f - static_cast<float>(d - 1)
                              / static_cast<float>(kMaxTrail - 2), 1.7f);
            auto  r  = static_cast<uint8_t>(f * c.trail.r + (1.f - f) * 7);
            auto  g  = static_cast<uint8_t>(f * c.trail.g + (1.f - f) * 7);
            auto  b  = static_cast<uint8_t>(f * c.trail.b + (1.f - f) * 7);
            Style st = Style{}.with_fg(Color::rgb(r, g, b));
            if (d >= kMaxTrail / 2) st = st.with_dim();
            s.theme[t][d] = pool.intern(st);
        }

        // Hover head: brighter/saturated version of the vivid second cell
        auto clamp = [](int v) { return static_cast<uint8_t>(std::min(255, v)); };
        s.hover[t] = pool.intern(Style{}.with_bold().with_fg(Color::rgb(
            clamp(c.bright.r + 50),
            clamp(c.bright.g + 50),
            clamp(c.bright.b + 50))));
    }

    s.flash     = pool.intern(Style{}.with_bold().with_fg(Color::rgb(255, 255, 255)));
    s.glitch    = pool.intern(Style{}.with_bold().with_inverse());
    s.bar_bg    = pool.intern(Style{}.with_fg(Color::rgb(40, 40, 40))
                                     .with_bg(Color::rgb(8,  8,  8)));
    s.bar_label = pool.intern(Style{}.with_bold()
                                     .with_fg(Color::rgb(0, 210, 60))
                                     .with_bg(Color::rgb(8, 8, 8)));
    s.bar_theme = pool.intern(Style{}.with_fg(Color::rgb(120, 200, 120))
                                     .with_bg(Color::rgb(8,   8,   8)));
    s.bar_info  = pool.intern(Style{}.with_fg(Color::rgb(70,  70,  70))
                                     .with_bg(Color::rgb(8,   8,   8)));
    s.bar_mode  = pool.intern(Style{}.with_bold()
                                     .with_fg(Color::rgb(255, 80, 80))
                                     .with_bg(Color::rgb(8,   8,  8)));
    return s;
}

// ── Drop ──────────────────────────────────────────────────────────────────────

struct Drop {
    float pos;
    float speed;
    int   trail;
    std::vector<char32_t> chars;
    std::vector<int>      mutate;
};

// ── Ripple shockwave ──────────────────────────────────────────────────────────

struct Ripple {
    int cx;       // origin column
    int cy;       // origin row (drops jump here when ring hits them)
    int radius;   // current ring radius (expands 1 per frame)
};

// ── Rain ──────────────────────────────────────────────────────────────────────

class Rain {
    int            cols_, rows_;
    std::mt19937   rng_;
    std::vector<Drop> drops_;
    std::vector<int>  flash_;    // per-column flash countdown
    std::vector<Ripple> ripples_;

    static constexpr int   kFlashFrames = 12;
    static constexpr float kMaxSpeed    = 3.5f;

    char32_t rand_glyph() {
        return kGlyphs[std::uniform_int_distribution<int>(0, kGlyphCount - 1)(rng_)];
    }

    void spawn(Drop& d, bool scatter = false) {
        d.speed = std::uniform_real_distribution<float>(0.22f, 1.45f)(rng_);
        d.trail = std::uniform_int_distribution<int>(7, kMaxTrail - 1)(rng_);
        d.pos   = scatter
                ? std::uniform_real_distribution<float>(-d.trail - 4.f, (float)rows_)(rng_)
                : std::uniform_real_distribution<float>(-d.trail - 4.f, -1.f)(rng_);
        if ((int)d.chars.size() != rows_) {
            d.chars.assign(rows_, rand_glyph());
            d.mutate.resize(rows_);
        }
        for (auto& m : d.mutate)
            m = std::uniform_int_distribution<int>(2, 30)(rng_);
    }

public:
    Rain(int cols, int rows)
        : cols_(cols), rows_(rows), rng_(std::random_device{}())
        , drops_(cols), flash_(cols, 0)
    {
        for (int c = 0; c < cols_; ++c) spawn(drops_[c], /*scatter=*/true);
    }

    // Triggered by 't' — randomize all speeds
    void jolt() {
        for (auto& d : drops_)
            d.speed = std::uniform_real_distribution<float>(0.22f, kMaxSpeed)(rng_);
    }

    // Triggered by 'r' — full respawn
    void reset() {
        for (auto& d : drops_) spawn(d, /*scatter=*/true);
        std::fill(flash_.begin(), flash_.end(), 0);
        ripples_.clear();
    }

    // Triggered by mouse click — launch an expanding shockwave
    void shockwave(int cx, int cy) {
        ripples_.push_back({cx, cy, 0});
    }

    void tick(float speed_mul, bool glitch, bool paused) {
        if (paused) return;

        // Expand ripples; trigger flashes on the ring boundary
        for (auto& rip : ripples_) {
            for (int side : {-1, 1}) {
                int c = rip.cx + side * rip.radius;
                if (c >= 0 && c < cols_) {
                    flash_[c] = kFlashFrames;
                    // Jump drop head to click row (minus trail so head lands at cy)
                    drops_[c].pos   = static_cast<float>(rip.cy);
                    drops_[c].speed = std::uniform_real_distribution<float>(1.2f, kMaxSpeed)(rng_);
                }
            }
            // Also flash origin on first frame
            if (rip.radius == 0 && rip.cx >= 0 && rip.cx < cols_)
                flash_[rip.cx] = kFlashFrames;
            ++rip.radius;
        }
        // Remove ripples that have expanded off screen
        std::erase_if(ripples_, [&](const Ripple& r) {
            return r.radius > cols_ / 2 + 4;
        });

        // Simulate drops
        for (int c = 0; c < cols_; ++c) {
            Drop& d = drops_[c];
            d.pos += d.speed * speed_mul;

            // Character mutation (faster during glitch)
            int mut_cap = glitch ? 2 : 38;
            for (int r = 0; r < rows_; ++r) {
                if (--d.mutate[r] <= 0) {
                    d.chars[r]  = rand_glyph();
                    d.mutate[r] = glitch
                        ? 3
                        : std::uniform_int_distribution<int>(3, mut_cap)(rng_);
                }
            }

            if (d.pos - d.trail > rows_) spawn(d);
            if (flash_[c] > 0) --flash_[c];
        }
    }

    void paint(Canvas& canvas, const Styles& st, int theme,
               int hover_col, bool glitch) const
    {
        canvas.clear();
        for (int c = 0; c < cols_; ++c) {
            const Drop& d  = drops_[c];
            int         hd = static_cast<int>(d.pos);
            bool        fl = flash_[c] > 0;
            bool        hv = (c == hover_col && !fl);

            for (int depth = 0; depth < d.trail; ++depth) {
                int row = hd - depth;
                if (row < 0 || row >= rows_) continue;

                uint16_t sid;
                if (depth == 0) {
                    // Head cell: flash > hover > glitch > normal
                    if (fl)            sid = st.flash;
                    else if (glitch)   sid = st.glitch;
                    else if (hv)       sid = st.hover[theme];
                    else               sid = st.theme[theme][0];
                } else {
                    int gi = std::min(depth, kMaxTrail - 1);
                    sid = fl      ? st.theme[theme][std::min(depth, 1)]
                        : glitch  ? st.glitch
                        :           st.theme[theme][gi];
                }
                canvas.set(c, row, d.chars[row], sid);
            }
        }
    }
};

// ── Status bar ────────────────────────────────────────────────────────────────

static void paint_bar(Canvas& canvas, const Styles& st,
                      int W, int H, int theme,
                      double fps, float speed_mul,
                      bool paused, bool glitch)
{
    int y = H - 1;
    for (int x = 0; x < W; ++x) canvas.set(x, y, U' ', st.bar_bg);

    // Left: " maya "
    auto write_str = [&](int x, std::string_view sv, uint16_t sid) {
        for (char c : sv) {
            if (x >= W) break;
            canvas.set(x++, y, static_cast<char32_t>(c), sid);
        }
        return x;
    };

    int x = 0;
    x = write_str(x, " maya ", st.bar_label);
    x = write_str(x, " | ",    st.bar_info);
    x = write_str(x, kThemes[theme].name, st.bar_theme);

    if (paused) {
        x = write_str(x, " | ", st.bar_info);
        x = write_str(x, "PAUSED", st.bar_mode);
    }
    if (glitch) {
        x = write_str(x, " | ", st.bar_info);
        x = write_str(x, "GLITCH", st.bar_mode);
    }

    // Right: speed | fps | hint
    char buf[96];
    char fpsbuf[16];
    auto [fend, _] = std::to_chars(fpsbuf, fpsbuf + sizeof(fpsbuf),
                                   fps, std::chars_format::fixed, 1);
    *fend = '\0';
    char spdbuf[16];
    auto [send, __] = std::to_chars(spdbuf, spdbuf + sizeof(spdbuf),
                                    speed_mul, std::chars_format::fixed, 2);
    *send = '\0';

    int len = std::snprintf(buf, sizeof(buf),
        " [t]heme [g]litch [space]jolt [r]eset  %sx  %s fps ",
        spdbuf, fpsbuf);
    int rx = W - len;
    if (rx > x) write_str(rx, {buf, static_cast<size_t>(len)}, st.bar_info);
}

// ─────────────────────────────────────────────────────────────────────────────

int main()
{
    signal(SIGWINCH, on_sigwinch);

    // ── Terminal: Cooked → Raw → AltScreen ────────────────────────────────────
    auto cooked = Terminal<Cooked>::create(STDIN_FILENO);
    if (!cooked) { std::println(std::cerr, "maya: {}", cooked.error().message); return 1; }

    auto raw = std::move(*cooked).enable_raw_mode();
    if (!raw) { std::println(std::cerr, "maya: {}", raw.error().message); return 1; }

    auto alt = std::move(*raw).enter_alt_screen();
    if (!alt) { std::println(std::cerr, "maya: {}", alt.error().message); return 1; }

    auto term = std::move(*alt);
    int  fd   = term.fd();

    // Enable all-motion mouse tracking (hover events); disable before RAII cleanup
    (void)term.write("\x1b[?1003h");

    // ── Canvas & style setup ──────────────────────────────────────────────────
    auto sz = term.size();
    int W = std::max(1, sz.width.value);
    int H = std::max(2, sz.height.value);

    StylePool pool;
    Styles    styles = intern_all(pool);

    Canvas front(W, H, &pool), back(W, H, &pool);
    front.mark_all_damaged();

    Rain rain(W, H - 1);

    // ── Interactive state ─────────────────────────────────────────────────────
    int   theme       = 0;
    float speed_mul   = 1.0f;
    bool  paused      = false;
    int   glitch_left = 0;           // frames remaining in glitch storm
    int   hover_col   = -1;

    InputParser parser;
    bool running = true;

    // ── Timing ────────────────────────────────────────────────────────────────
    constexpr auto kFrameTime   = std::chrono::microseconds(16'667); // ~60 fps
    constexpr auto kSpinMargin  = std::chrono::microseconds(1'500);
    int    frame_count = 0;
    double fps         = 60.0;
    auto   fps_clock   = Clock::now();

    std::string out;
    out.reserve(static_cast<size_t>(W * H * 14));

    // ─────────────────────────────────────────────────────────────────────────
    while (running) {
        auto frame_start = Clock::now();

        // ── Resize ───────────────────────────────────────────────────────────
        if (g_resize) {
            g_resize = 0;
            sz = term.size();
            W  = std::max(1, sz.width.value);
            H  = std::max(2, sz.height.value);
            front.resize(W, H);
            back .resize(W, H);
            front.mark_all_damaged();
            pool.clear();
            styles = intern_all(pool);
            rain   = Rain(W, H - 1);
        }

        // ── Input ─────────────────────────────────────────────────────────────
        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
            auto bytes = term.read_raw();
            if (bytes && !bytes->empty()) {
                for (const auto& ev : parser.feed(*bytes)) {
                    if (const auto* ke = std::get_if<KeyEvent>(&ev)) {
                        if (auto* sk = std::get_if<SpecialKey>(&ke->key)) {
                            if (*sk == SpecialKey::Escape) running = false;
                        }
                        if (auto* ck = std::get_if<CharKey>(&ke->key)) {
                            switch (ck->codepoint) {
                                case 'q': case 'Q':
                                    running = false; break;
                                case 'p': case 'P':
                                    paused = !paused; break;
                                case 'g': case 'G':
                                    glitch_left = 90; break;  // ~1.5 s at 60fps
                                case 't': case 'T':
                                    theme = (theme + 1) % kThemeCount; break;
                                case 'r': case 'R':
                                    rain.reset(); break;
                                case ' ':
                                    rain.jolt(); break;
                                case '+': case '=':
                                    speed_mul = std::min(5.0f, speed_mul + 0.25f); break;
                                case '-': case '_':
                                    speed_mul = std::max(0.1f, speed_mul - 0.25f); break;
                            }
                        }
                    }
                    if (const auto* me = std::get_if<MouseEvent>(&ev)) {
                        // Clamp to canvas area (exclude status bar)
                        hover_col = std::clamp(me->x.value - 1, 0, W - 1);
                        if (me->kind == MouseEventKind::Press) {
                            int row = std::clamp(me->y.value - 1, 0, H - 2);
                            rain.shockwave(hover_col, row);
                        }
                        if (me->button == MouseButton::ScrollUp)
                            speed_mul = std::min(5.0f, speed_mul + 0.25f);
                        if (me->button == MouseButton::ScrollDown)
                            speed_mul = std::max(0.1f, speed_mul - 0.25f);
                    }
                }
            }
        }

        // ── Simulate ─────────────────────────────────────────────────────────
        bool glitching = glitch_left > 0;
        if (glitch_left > 0) --glitch_left;

        rain.tick(speed_mul, glitching, paused);

        // ── FPS ───────────────────────────────────────────────────────────────
        ++frame_count;
        {
            double el = std::chrono::duration<double>(Clock::now() - fps_clock).count();
            if (el >= 0.5) { fps = frame_count / el; frame_count = 0; fps_clock = Clock::now(); }
        }

        // ── Paint ─────────────────────────────────────────────────────────────
        rain.paint(back, styles, theme, hover_col, glitching);
        paint_bar(back, styles, W, H, theme, fps, speed_mul, paused, glitching);

        // ── Diff → output ─────────────────────────────────────────────────────
        out.clear();
        out += ansi::sync_start;
        diff(front, back, pool, out);
        out += ansi::reset;
        out += ansi::sync_end;

        (void)term.write(out);
        std::swap(front, back);

        // ── Frame cap (hybrid sleep + spin) ───────────────────────────────────
        auto deadline  = frame_start + kFrameTime;
        auto sleep_end = deadline - kSpinMargin;
        if (Clock::now() < sleep_end)
            std::this_thread::sleep_for(sleep_end - Clock::now());
        while (Clock::now() < deadline) {}
    }

    (void)term.write("\x1b[?1003l");  // disable all-motion before RAII cleanup
    return 0;
}
