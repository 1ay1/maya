// maya — interactive Matrix digital rain
//
// Mouse: move = highlight column | click = shockwave | scroll = speed
// Keys:  q/ESC=quit  p=pause  g=glitch storm  t=cycle theme
//        space=jolt all drops  r=reset  +/-=speed

#include <maya/maya.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <random>
#include <string>
#include <vector>

using namespace maya;

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

struct Styles {
    std::array<GradientIDs, kThemeCount> theme;
    std::array<uint16_t,    kThemeCount> hover;
    uint16_t flash;
    uint16_t glitch;
    uint16_t bar_bg;
    uint16_t bar_label;
    uint16_t bar_theme;
    uint16_t bar_info;
    uint16_t bar_mode;
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
    int cx, cy, radius;
};

// ── Rain ──────────────────────────────────────────────────────────────────────

class Rain {
    int            cols_, rows_;
    std::mt19937   rng_;
    std::vector<Drop>   drops_;
    std::vector<int>    flash_;
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

    void jolt() {
        for (auto& d : drops_)
            d.speed = std::uniform_real_distribution<float>(0.22f, kMaxSpeed)(rng_);
    }

    void reset() {
        for (auto& d : drops_) spawn(d, /*scatter=*/true);
        std::fill(flash_.begin(), flash_.end(), 0);
        ripples_.clear();
    }

    void shockwave(int cx, int cy) {
        ripples_.push_back({cx, cy, 0});
    }

    void tick(float speed_mul, bool glitch, bool paused) {
        if (paused) return;

        for (auto& rip : ripples_) {
            for (int side : {-1, 1}) {
                int c = rip.cx + side * rip.radius;
                if (c >= 0 && c < cols_) {
                    flash_[c] = kFlashFrames;
                    drops_[c].pos   = static_cast<float>(rip.cy);
                    drops_[c].speed = std::uniform_real_distribution<float>(1.2f, kMaxSpeed)(rng_);
                }
            }
            if (rip.radius == 0 && rip.cx >= 0 && rip.cx < cols_)
                flash_[rip.cx] = kFlashFrames;
            ++rip.radius;
        }
        std::erase_if(ripples_, [&](const Ripple& r) {
            return r.radius > cols_ / 2 + 4;
        });

        for (int c = 0; c < cols_; ++c) {
            Drop& d = drops_[c];
            d.pos += d.speed * speed_mul;

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
                    if (fl)          sid = st.flash;
                    else if (glitch) sid = st.glitch;
                    else if (hv)     sid = st.hover[theme];
                    else             sid = st.theme[theme][0];
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

    if (paused) { x = write_str(x, " | ", st.bar_info); x = write_str(x, "PAUSED", st.bar_mode); }
    if (glitch) { x = write_str(x, " | ", st.bar_info); x = write_str(x, "GLITCH", st.bar_mode); }

    char buf[96], fpsbuf[16], spdbuf[16];
    auto [fend, _]  = std::to_chars(fpsbuf, fpsbuf + sizeof(fpsbuf), fps, std::chars_format::fixed, 1);
    auto [send, __] = std::to_chars(spdbuf, spdbuf + sizeof(spdbuf), speed_mul, std::chars_format::fixed, 2);
    *fend = '\0'; *send = '\0';

    int len = std::snprintf(buf, sizeof(buf),
        " [t]heme [g]litch [space]jolt [r]eset  %sx  %s fps ", spdbuf, fpsbuf);
    int rx = W - len;
    if (rx > x) write_str(rx, {buf, static_cast<std::size_t>(len)}, st.bar_info);
}

// ── main ──────────────────────────────────────────────────────────────────────

int main()
{
    int   theme       = 0;
    float speed_mul   = 1.0f;
    bool  paused      = false;
    int   glitch_left = 0;
    int   hover_col   = -1;

    // Simulation and style state — rebuilt on resize.
    std::unique_ptr<Rain> rain;
    Styles                styles{};

    // FPS tracking
    using Clock = std::chrono::steady_clock;
    int    frame_count = 0;
    double fps         = 60.0;
    auto   fps_clock   = Clock::now();

    auto result = canvas_run(
        CanvasConfig{.fps = 60, .mouse = true, .title = "matrix · maya"},

        // on_resize: rebuild rain + re-intern styles for the new size
        [&](StylePool& pool, int W, int H) {
            styles = intern_all(pool);
            rain   = std::make_unique<Rain>(W, H - 1);
        },

        // on_event
        [&](const Event& ev) -> bool {
            if (key(ev, 'q') || key(ev, 'Q') || key(ev, SpecialKey::Escape))
                return false;

            on(ev, 'p', [&] { paused = !paused; });
            on(ev, 'P', [&] { paused = !paused; });
            on(ev, 'g', [&] { glitch_left = 90; });
            on(ev, 'G', [&] { glitch_left = 90; });
            on(ev, 't', [&] { theme = (theme + 1) % kThemeCount; });
            on(ev, 'T', [&] { theme = (theme + 1) % kThemeCount; });
            on(ev, 'r', [&] { rain->reset(); });
            on(ev, 'R', [&] { rain->reset(); });
            on(ev, ' ', [&] { rain->jolt(); });
            on(ev, '+', '=', [&] { speed_mul = std::min(5.0f, speed_mul + 0.25f); });
            on(ev, '-', '_', [&] { speed_mul = std::max(0.1f, speed_mul - 0.25f); });

            if (auto pos = mouse_pos(ev)) {
                hover_col = pos->col - 1;
            }
            if (mouse_clicked(ev)) {
                auto pos = mouse_pos(ev);
                if (pos) rain->shockwave(pos->col - 1, std::clamp(pos->row - 1, 0, 9999));
            }
            if (scrolled_up(ev))   speed_mul = std::min(5.0f, speed_mul + 0.25f);
            if (scrolled_down(ev)) speed_mul = std::max(0.1f, speed_mul - 0.25f);

            return true;
        },

        // on_paint: simulate + draw (canvas already cleared by framework)
        [&](Canvas& canvas, int W, int H) {
            bool glitching = glitch_left > 0;
            if (glitch_left > 0) --glitch_left;

            rain->tick(speed_mul, glitching, paused);

            // FPS
            ++frame_count;
            double el = std::chrono::duration<double>(Clock::now() - fps_clock).count();
            if (el >= 0.5) { fps = frame_count / el; frame_count = 0; fps_clock = Clock::now(); }

            rain->paint(canvas, styles, theme, hover_col, glitching);
            paint_bar(canvas, styles, W, H, theme, fps, speed_mul, paused, glitching);
        }
    );

    if (!result) {
        std::println(std::cerr, "maya: {}", result.error().message);
        return 1;
    }
}
