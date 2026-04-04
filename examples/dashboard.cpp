// maya — live system monitoring dashboard
//
// Canvas-rendered: braille area charts, gradient gauges, per-core bars,
// process table, and live simulated metrics at 30fps.
//
// Keys: q/ESC = quit   t/T = cycle theme

#include <maya/maya.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <string>

using namespace maya;

// ── Fast sin approximation ──────────────────────────────────────────────────

static float fsin(float x) noexcept {
    constexpr float tp = 1.f / (2.f * 3.14159265f);
    x *= tp; x -= 0.25f + std::floor(x + 0.25f);
    x *= 16.f * (std::fabs(x) - 0.5f);
    x += 0.225f * x * (std::fabs(x) - 1.f);
    return x;
}

static float wave(float t, float b, float a1, float f1,
                  float a2 = 0, float f2 = 0, float a3 = 0, float f3 = 0) {
    return b + a1 * fsin(t * f1) + a2 * fsin(t * f2) + a3 * fsin(t * f3);
}

// ── Simulation state ────────────────────────────────────────────────────────

static float g_t     = 0.f;
static int   g_frame = 0;

static constexpr int kCores = 8;
static constexpr int kHist  = 240;

static float             g_cpu[kCores], g_cpu_avg;
static std::deque<float> g_cpu_hist, g_core_hist[kCores];

static float             g_mem_used, g_mem_buff, g_mem_cache;
static constexpr float   kMemGB = 32.f;
static std::deque<float> g_mem_hist;

static float             g_rx, g_tx;
static std::deque<float> g_rx_hist, g_tx_hist;
static int               g_tcp = 842, g_udp = 156, g_err = 0, g_drop = 0;

static float             g_dr, g_dw, g_ior, g_iow, g_du = 34;
static std::deque<float> g_dr_hist, g_dw_hist;

static float g_load[3];
static int   g_tasks = 312, g_threads = 1847;
static float g_up = 7 * 86400.f + 14 * 3600.f + 32 * 60.f;

struct Proc {
    const char *user, *name;
    int pid;
    float base_cpu, base_mem;
    const char *virt, *res;
    int st; // 0=run 1=sleep 2=idle
};

static Proc g_procs[] = {
    {"www",    "node",         1284, 14.2f,  3.2f, "1.2G","512M", 0},
    {"pg",     "postgres",      892,  8.1f, 15.4f, "2.8G","1.1G", 0},
    {"www",    "nginx",        2041,  3.8f,  1.1f, "340M","128M", 0},
    {"redis",  "redis-server", 1567,  2.4f,  4.2f, "890M","672M", 2},
    {"app",    "worker-7",     3201,  1.6f,  0.8f, "256M", "92M", 0},
    {"root",   "containerd",   1102,  1.2f,  2.1f, "1.4G","340M", 0},
    {"app",    "worker-3",     3198,  0.9f,  0.7f, "256M", "88M", 1},
    {"app",    "worker-5",     3199,  0.8f,  0.6f, "256M", "84M", 0},
    {"root",   "systemd",         1,  0.4f,  0.5f, "168M", "12M", 1},
    {"root",   "dockerd",      1340,  0.6f,  1.8f, "920M","280M", 0},
    {"root",   "kubelet",      1401,  0.5f,  1.2f, "680M","190M", 0},
    {"root",   "sshd",         1820,  0.1f,  0.1f,  "72M",  "8M", 1},
    {"nobody", "dnsmasq",      1910,  0.1f,  0.1f,  "48M",  "6M", 1},
    {"root",   "cron",         1055,  0.0f,  0.3f,  "56M",  "4M", 1},
};
static constexpr int kProcs = 14;

static void push(std::deque<float>& h, float v) {
    h.push_back(v);
    while (static_cast<int>(h.size()) > kHist) h.pop_front();
}

static void tick(float dt) {
    g_t += dt;
    g_up += dt;
    float t = g_t;

    g_cpu[0] = std::clamp(wave(t, 65, 25, 0.7f, 10, 1.9f,  5, 4.1f), 2.f, 99.f);
    g_cpu[1] = std::clamp(wave(t, 30, 20, 0.5f, 12, 2.3f,  4, 3.7f), 2.f, 99.f);
    g_cpu[2] = std::clamp(wave(t, 82, 15, 0.9f,  8, 1.5f,  3, 5.2f), 2.f, 99.f);
    g_cpu[3] = std::clamp(wave(t, 50, 22, 0.6f, 14, 2.8f,  6, 3.3f), 2.f, 99.f);
    g_cpu[4] = std::clamp(wave(t, 60, 18, 1.1f,  9, 1.7f,  5, 4.5f), 2.f, 99.f);
    g_cpu[5] = std::clamp(wave(t, 20, 15, 0.4f,  8, 3.1f,  3, 2.2f), 2.f, 99.f);
    g_cpu[6] = std::clamp(wave(t, 88, 10, 1.3f,  5, 0.8f,  2, 6.1f), 2.f, 99.f);
    g_cpu[7] = std::clamp(wave(t, 38, 24, 0.8f, 11, 2.1f,  7, 3.9f), 2.f, 99.f);

    g_cpu_avg = 0;
    for (int i = 0; i < kCores; ++i) g_cpu_avg += g_cpu[i];
    g_cpu_avg /= kCores;

    g_mem_used  = std::clamp(wave(t, 16.4f, 2.4f, 0.3f, 0.8f, 1.1f), 10.f, 24.f);
    g_mem_buff  = std::clamp(wave(t, 4.2f,  1.f,  0.2f, 0.4f, 0.7f), 1.6f, 7.f);
    g_mem_cache = std::clamp(wave(t, 6.8f,  1.6f, 0.15f, 0.6f, 0.5f), 3.f, 10.f);

    g_tx = std::max(0.1f, wave(t, 12.4f, 6, 1.5f, 3, 3.7f, 1.5f, 0.4f));
    g_rx = std::max(0.1f, wave(t, 48.2f, 18, 0.9f, 8, 2.3f, 4, 0.5f));
    g_tcp  = std::clamp(static_cast<int>(wave(t, 842, 80, 0.3f, 30, 1.2f)), 600, 1100);
    g_udp  = std::clamp(static_cast<int>(wave(t, 156, 40, 0.5f, 15, 1.8f)), 80, 250);
    g_err  = std::clamp(static_cast<int>(wave(t, 2, 3, 0.1f, 1, 0.7f)), 0, 12);
    g_drop = std::clamp(static_cast<int>(wave(t, 1, 2, 0.15f, 1, 0.5f)), 0, 8);

    g_dr  = std::max(0.f, wave(t, 124, 50, 1.2f, 20, 2.8f, 10, 0.3f));
    g_dw  = std::max(0.f, wave(t, 45,  20, 0.8f, 10, 2.1f,  5, 0.5f));
    g_ior = std::max(0.f, wave(t, 28.4f, 12, 1.5f, 4, 3.2f));
    g_iow = std::max(0.f, wave(t, 12.1f,  6, 0.9f, 2, 2.7f));
    g_du  = std::clamp(wave(t, 34, 20, 0.6f, 8, 1.4f), 2.f, 98.f);

    g_load[0] = std::max(0.1f, wave(t, 4.82f, 1.5f, 0.15f, 0.5f, 0.4f));
    g_load[1] = std::max(0.1f, wave(t, 3.21f, 1.f,  0.08f, 0.3f, 0.2f));
    g_load[2] = std::max(0.1f, wave(t, 2.15f, 0.6f, 0.05f, 0.2f, 0.1f));
    g_tasks   = std::clamp(static_cast<int>(wave(t, 312, 15, 0.2f, 5, 0.8f)), 280, 350);
    g_threads = std::clamp(static_cast<int>(wave(t, 1847, 80, 0.3f, 30, 0.9f)), 1700, 2000);

    static int hk = 0;
    if (++hk >= 3) {
        hk = 0;
        push(g_cpu_hist, g_cpu_avg / 100.f);
        for (int i = 0; i < kCores; ++i)
            push(g_core_hist[i], g_cpu[i] / 100.f);
        push(g_mem_hist, std::clamp((g_mem_used + g_mem_buff + g_mem_cache) / kMemGB, 0.f, 1.f));
        push(g_rx_hist, std::min(g_rx / 80.f, 1.f));
        push(g_tx_hist, std::min(g_tx / 30.f, 1.f));
        push(g_dr_hist, std::min(g_dr / 200.f, 1.f));
        push(g_dw_hist, std::min(g_dw / 100.f, 1.f));
    }
}

// ── Themes ──────────────────────────────────────────────────────────────────

struct Palette {
    const char* name;
    uint8_t accent[3], border[3], brand[3];
    uint8_t glo[3], gmid[3], ghi[3];
    uint8_t rx_c[3], tx_c[3];
};

static const Palette kThemes[] = {
    {"midnight", {100,150,240}, {40,45,60}, {80,130,240},
     {40,190,110}, {230,210,50}, {240,55,55},
     {60,185,235}, {150,100,235}},
    {"forest",   {80,210,130}, {38,58,42}, {60,190,100},
     {60,190,100}, {200,200,60}, {240,80,60},
     {70,195,175}, {150,195,80}},
    {"ember",    {245,155,75}, {60,45,38}, {235,115,50},
     {235,175,55}, {235,115,35}, {235,50,45},
     {235,155,75}, {235,95,55}},
};
static constexpr int kThemeN = 3;
static int g_theme = 0;

// ── Style IDs ───────────────────────────────────────────────────────────────

struct Sty {
    uint16_t dim, muted, label, value, bold_w, num;
    uint16_t accent, brand, brd;
    uint16_t green, yellow, red;
    uint16_t hdr_bg, hdr_host, hdr_dim, hdr_sep;
    uint16_t bar_bg, bar_hint;

    static constexpr int kGrad = 12;
    uint16_t cpu_g[kGrad], cpu_edge[kGrad];
    uint16_t rx_g[6], tx_g[6];
    uint16_t mem_used, mem_buff, mem_cache, mem_free;

    uint16_t proc_hd, proc_nm, proc_us, proc_alt;
    uint16_t core[kCores];
    uint16_t spin_s;
    uint16_t st_run, st_sleep, st_idle;
};
static Sty s;

static Color lerpc(const uint8_t a[3], const uint8_t b[3], float t) {
    return Color::rgb(
        static_cast<uint8_t>(a[0] + (b[0] - a[0]) * t),
        static_cast<uint8_t>(a[1] + (b[1] - a[1]) * t),
        static_cast<uint8_t>(a[2] + (b[2] - a[2]) * t));
}

static Color gauge_col(float pct) {
    float f = std::clamp(pct, 0.f, 100.f) / 100.f;
    if (f < 0.5f) {
        float t = f * 2.f;
        return Color::rgb(
            static_cast<uint8_t>(60 + 180 * t),
            static_cast<uint8_t>(210 - 30 * t),
            static_cast<uint8_t>(100 - 60 * t));
    }
    float t = (f - 0.5f) * 2.f;
    return Color::rgb(240,
        static_cast<uint8_t>(180 - 140 * t),
        static_cast<uint8_t>(40 - 30 * t));
}

static const uint8_t kCoreRGB[kCores][3] = {
    {80,200,255}, {60,210,200}, {80,220,140}, {120,210,80},
    {180,200,60}, {220,180,60}, {240,140,60}, {255,100,80},
};

static void intern_styles(StylePool& pool) {
    auto& th = kThemes[g_theme];

    s.dim    = pool.intern(Style{}.with_fg(Color::rgb(50, 50, 68)));
    s.muted  = pool.intern(Style{}.with_fg(Color::rgb(70, 70, 90)));
    s.label  = pool.intern(Style{}.with_fg(Color::rgb(100, 100, 120)));
    s.value  = pool.intern(Style{}.with_bold().with_fg(Color::rgb(200, 200, 220)));
    s.bold_w = pool.intern(Style{}.with_bold().with_fg(Color::rgb(220, 220, 240)));
    s.num    = pool.intern(Style{}.with_fg(Color::rgb(140, 140, 165)));
    s.accent = pool.intern(Style{}.with_fg(Color::rgb(th.accent[0], th.accent[1], th.accent[2])));
    s.brand  = pool.intern(Style{}.with_bold().with_fg(Color::rgb(th.brand[0], th.brand[1], th.brand[2])));
    s.brd    = pool.intern(Style{}.with_fg(Color::rgb(th.border[0], th.border[1], th.border[2])));
    s.green  = pool.intern(Style{}.with_bold().with_fg(Color::rgb(80, 220, 120)));
    s.yellow = pool.intern(Style{}.with_bold().with_fg(Color::rgb(240, 200, 60)));
    s.red    = pool.intern(Style{}.with_bold().with_fg(Color::rgb(240, 80, 80)));

    auto hbg = Color::rgb(14, 14, 22);
    s.hdr_bg   = pool.intern(Style{}.with_fg(Color::rgb(55, 55, 75)).with_bg(hbg));
    s.hdr_host = pool.intern(Style{}.with_bold()
        .with_fg(Color::rgb(th.accent[0], th.accent[1], th.accent[2])).with_bg(hbg));
    s.hdr_dim  = pool.intern(Style{}.with_fg(Color::rgb(90, 90, 110)).with_bg(hbg));
    s.hdr_sep  = pool.intern(Style{}.with_fg(Color::rgb(42, 42, 58)).with_bg(hbg));

    auto bbg = Color::rgb(12, 12, 18);
    s.bar_bg   = pool.intern(Style{}.with_fg(Color::rgb(55, 55, 72)).with_bg(bbg));
    s.bar_hint = pool.intern(Style{}.with_fg(Color::rgb(50, 50, 65)).with_bg(bbg));

    for (int i = 0; i < Sty::kGrad; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(Sty::kGrad - 1);
        Color c = (t < 0.5f) ? lerpc(th.glo, th.gmid, t * 2.f)
                              : lerpc(th.gmid, th.ghi, (t - 0.5f) * 2.f);
        s.cpu_g[i] = pool.intern(Style{}.with_fg(c));
        // Brighter edge variant
        auto ec = Color::rgb(
            std::min(255, c.r() + 50), std::min(255, c.g() + 50), std::min(255, c.b() + 50));
        s.cpu_edge[i] = pool.intern(Style{}.with_bold().with_fg(ec));
    }

    for (int i = 0; i < 6; ++i) {
        float t = 0.35f + 0.65f * static_cast<float>(i) / 5.f;
        s.rx_g[i] = pool.intern(Style{}.with_fg(Color::rgb(
            static_cast<uint8_t>(th.rx_c[0] * t),
            static_cast<uint8_t>(th.rx_c[1] * t),
            static_cast<uint8_t>(th.rx_c[2] * t))));
        s.tx_g[i] = pool.intern(Style{}.with_fg(Color::rgb(
            static_cast<uint8_t>(th.tx_c[0] * t),
            static_cast<uint8_t>(th.tx_c[1] * t),
            static_cast<uint8_t>(th.tx_c[2] * t))));
    }

    s.mem_used  = pool.intern(Style{}.with_fg(Color::rgb(100, 180, 255)));
    s.mem_buff  = pool.intern(Style{}.with_fg(Color::rgb(180, 130, 255)));
    s.mem_cache = pool.intern(Style{}.with_fg(Color::rgb(255, 180, 80)));
    s.mem_free  = pool.intern(Style{}.with_fg(Color::rgb(45, 45, 60)));

    s.proc_hd  = pool.intern(Style{}.with_bold().with_fg(Color::rgb(115, 115, 140)));
    s.proc_nm  = pool.intern(Style{}.with_fg(Color::rgb(170, 170, 190)));
    s.proc_us  = pool.intern(Style{}.with_fg(Color::rgb(110, 130, 160)));
    s.proc_alt = pool.intern(Style{}.with_bg(Color::rgb(15, 15, 23)));

    for (int i = 0; i < kCores; ++i)
        s.core[i] = pool.intern(Style{}.with_fg(
            Color::rgb(kCoreRGB[i][0], kCoreRGB[i][1], kCoreRGB[i][2])));

    s.spin_s = pool.intern(Style{}.with_fg(
        Color::rgb(th.brand[0], th.brand[1], th.brand[2])));

    s.st_run   = pool.intern(Style{}.with_fg(Color::rgb(80, 220, 120)));
    s.st_sleep = pool.intern(Style{}.with_fg(Color::rgb(85, 85, 105)));
    s.st_idle  = pool.intern(Style{}.with_fg(Color::rgb(200, 180, 60)));
}

// ── Helpers ─────────────────────────────────────────────────────────────────

template <typename... A>
static std::string fmt(const char* f, A... a) {
    char b[128];
    std::snprintf(b, sizeof b, f, a...);
    return b;
}

static constexpr char32_t kSpinCh[] = {
    U'\u280B', U'\u2819', U'\u2839', U'\u2838',
    U'\u283C', U'\u2834', U'\u2826', U'\u2827',
    U'\u2807', U'\u280F',
};
static char32_t spin_ch() { return kSpinCh[(g_frame / 3) % 10]; }

// ── Canvas painters ─────────────────────────────────────────────────────────

static void paint_box(Canvas& c, int x0, int y0, int x1, int y1, uint16_t sid) {
    c.set(x0, y0, U'\u256D', sid); c.set(x1, y0, U'\u256E', sid);
    c.set(x0, y1, U'\u2570', sid); c.set(x1, y1, U'\u256F', sid);
    for (int x = x0 + 1; x < x1; ++x) {
        c.set(x, y0, U'\u2500', sid);
        c.set(x, y1, U'\u2500', sid);
    }
    for (int y = y0 + 1; y < y1; ++y) {
        c.set(x0, y, U'\u2502', sid);
        c.set(x1, y, U'\u2502', sid);
    }
}

static void paint_title(Canvas& c, int x0, int y0,
                        char32_t icon, uint16_t icon_sid,
                        const char* text, uint16_t text_sid, uint16_t brd_sid) {
    int x = x0 + 2;
    c.set(x, y0, U'\u2500', brd_sid); x++;
    c.set(x, y0, U' ', brd_sid); x++;
    c.set(x, y0, icon, icon_sid); x++;
    c.set(x, y0, U' ', brd_sid); x++;
    c.write_text(x, y0, text, text_sid); x += static_cast<int>(std::string_view(text).size());
    c.set(x, y0, U' ', brd_sid); x++;
    c.set(x, y0, U'\u2500', brd_sid);
}

// Braille area chart with per-row gradient
static void area_chart(Canvas& c, int x0, int y0, int w, int h,
                       const std::deque<float>& data,
                       const uint16_t* grad, int gn,
                       const uint16_t* edge = nullptr) {
    // Bottom-up fill masks: index = dots filled from bottom (0..4)
    static constexpr uint8_t kL[5] = {0x00, 0x08, 0x0C, 0x0E, 0x0F};
    static constexpr uint8_t kR[5] = {0x00, 0x80, 0xC0, 0xE0, 0xF0};

    int sub = h * 4, dw = w * 2;
    int st = std::max(0, static_cast<int>(data.size()) - dw);

    for (int cy = 0; cy < h; ++cy) {
        int gi = std::clamp((h - 1 - cy) * gn / std::max(1, h), 0, gn - 1);
        uint16_t sid = grad[gi];

        for (int cx = 0; cx < w; ++cx) {
            int il = st + cx * 2, ir = il + 1;
            float vl = (il < static_cast<int>(data.size())) ? data[il] : 0.f;
            float vr = (ir < static_cast<int>(data.size())) ? data[ir] : 0.f;
            int base = (h - 1 - cy) * 4;
            int fl = std::clamp(static_cast<int>(vl * sub) - base, 0, 4);
            int fr = std::clamp(static_cast<int>(vr * sub) - base, 0, 4);
            uint8_t m = kL[fl] | kR[fr];
            if (m == 0) continue;

            // Use brighter edge style at the top of the filled area
            bool is_edge = false;
            if (edge) {
                int next_base = base + 4;
                int nfl = std::clamp(static_cast<int>(vl * sub) - next_base, 0, 4);
                int nfr = std::clamp(static_cast<int>(vr * sub) - next_base, 0, 4);
                if (nfl == 0 && nfr == 0 && (fl < 4 || fr < 4)) is_edge = true;
            }
            c.set(x0 + cx, y0 + cy,
                  static_cast<char32_t>(0x2800 + m),
                  is_edge ? edge[gi] : sid);
        }
    }
}

// Horizontal bar with 1/8th cell precision
static void hbar(Canvas& c, int x, int y, int w, float pct,
                 uint16_t fsid, uint16_t bsid) {
    static constexpr char32_t kP[] = {
        U' ', U'\u258F', U'\u258E', U'\u258D',
        U'\u258C', U'\u258B', U'\u258A', U'\u2589'};
    pct = std::clamp(pct, 0.f, 1.f);
    float f = pct * w;
    int full = static_cast<int>(f);
    int part = static_cast<int>((f - full) * 8);
    for (int i = 0; i < w; ++i) {
        if (i < full) c.set(x + i, y, U'\u2588', fsid);
        else if (i == full && part > 0) c.set(x + i, y, kP[part], fsid);
        else c.set(x + i, y, U'\u2591', bsid);
    }
}

// Sparkline with block elements
static void spark(Canvas& c, int x, int y, int w,
                  const std::deque<float>& d, uint16_t sid) {
    static constexpr char32_t kB[] = {
        U'\u2581', U'\u2582', U'\u2583', U'\u2584',
        U'\u2585', U'\u2586', U'\u2587', U'\u2588'};
    int st = std::max(0, static_cast<int>(d.size()) - w);
    for (int i = st, dx = 0; i < static_cast<int>(d.size()) && dx < w; ++i, ++dx)
        c.set(x + dx, y, kB[std::clamp(static_cast<int>(d[i] * 7.99f), 0, 7)], sid);
}

// ── Panel painters ──────────────────────────────────────────────────────────

static void paint_header(Canvas& c, int W) {
    for (int x = 0; x < W; ++x) c.set(x, 0, U' ', s.hdr_bg);

    int x = 1;
    c.set(x, 0, spin_ch(), s.spin_s); x += 1;
    c.set(x, 0, U' ', s.hdr_bg); x++;
    c.write_text(x, 0, "prod-srv-01", s.hdr_host); x += 11;
    c.write_text(x, 0, " | ", s.hdr_sep); x += 3;

    int d = static_cast<int>(g_up / 86400.f);
    int h = static_cast<int>(std::fmod(g_up, 86400.f) / 3600.f);
    int m = static_cast<int>(std::fmod(g_up, 3600.f) / 60.f);
    auto up = fmt("up %dd %dh %dm", d, h, m);
    c.write_text(x, 0, up, s.hdr_dim); x += static_cast<int>(up.size());
    c.write_text(x, 0, " | ", s.hdr_sep); x += 3;

    c.write_text(x, 0, "load ", s.hdr_dim); x += 5;
    auto ld = fmt("%.2f %.2f %.2f",
        static_cast<double>(g_load[0]),
        static_cast<double>(g_load[1]),
        static_cast<double>(g_load[2]));
    uint16_t lsid = g_load[0] < 4 ? s.green : (g_load[0] < 6 ? s.yellow : s.red);
    c.write_text(x, 0, ld, lsid); x += static_cast<int>(ld.size());
    c.write_text(x, 0, " | ", s.hdr_sep); x += 3;

    auto ti = fmt("%d tasks  %d thr", g_tasks, g_threads);
    c.write_text(x, 0, ti, s.hdr_dim);

    auto sum = fmt("cpu %2.0f%%  mem %2.0f%%",
        static_cast<double>(g_cpu_avg),
        static_cast<double>((g_mem_used + g_mem_buff + g_mem_cache) / kMemGB * 100));
    uint16_t csid = g_cpu_avg < 50 ? s.green : (g_cpu_avg < 80 ? s.yellow : s.red);
    c.write_text(W - static_cast<int>(sum.size()) - 2, 0, sum, csid);
}

static void paint_cpu(Canvas& c, int x0, int y0, int w, int h) {
    if (w < 12 || h < 6) return;
    int x1 = x0 + w - 1, y1 = y0 + h - 1;
    paint_box(c, x0, y0, x1, y1, s.brd);
    paint_title(c, x0, y0, spin_ch(), s.spin_s, "CPU", s.accent, s.brd);

    int ix = x0 + 2, iw = w - 4;
    int row = y0 + 1;

    // Average + peak/valley
    float mx = *std::max_element(g_cpu, g_cpu + kCores);
    float mn = *std::min_element(g_cpu, g_cpu + kCores);
    auto avg = fmt("avg %4.1f%%", static_cast<double>(g_cpu_avg));
    uint16_t asid = g_cpu_avg < 50 ? s.green : (g_cpu_avg < 80 ? s.yellow : s.red);
    c.write_text(ix, row, avg, asid);
    c.write_text(ix + static_cast<int>(avg.size()) + 1, row, "  ", s.dim);
    auto pk = fmt("peak %.0f%%", static_cast<double>(mx));
    c.write_text(ix + static_cast<int>(avg.size()) + 3, row, pk,
        mx > 80 ? s.red : s.num);
    auto lo = fmt("low %.0f%%", static_cast<double>(mn));
    c.write_text(ix + static_cast<int>(avg.size()) + 3 + static_cast<int>(pk.size()) + 2,
        row, lo, s.green);
    row++;

    // Braille area chart
    int chart_h = std::max(3, h - 6 - (kCores + 1) / 2);
    area_chart(c, ix, row, iw, chart_h, g_cpu_hist, s.cpu_g, Sty::kGrad, s.cpu_edge);
    row += chart_h;

    // Y-axis labels
    c.write_text(x1 - 4, row - chart_h, "100", s.dim);
    c.write_text(x1 - 3, row - 1, "0", s.dim);

    row++; // blank line

    // Per-core bars in 2 columns
    int col_w = iw / 2;
    int bar_w = std::max(6, col_w - 9);
    for (int i = 0; i < kCores; ++i) {
        int col = i / 4, ci = i % 4;
        int bx = ix + col * col_w;
        int by = row + ci;
        if (by >= y1) break;

        auto lbl = fmt("c%d ", i);
        c.write_text(bx, by, lbl, s.muted);
        hbar(c, bx + 3, by, bar_w, g_cpu[i] / 100.f, s.core[i], s.dim);
        auto pct = fmt("%2.0f", static_cast<double>(g_cpu[i]));
        c.write_text(bx + 3 + bar_w + 1, by, pct, s.num);
    }
}

static void paint_mem(Canvas& c, int x0, int y0, int w, int h) {
    if (w < 12 || h < 6) return;
    int x1 = x0 + w - 1, y1 = y0 + h - 1;
    paint_box(c, x0, y0, x1, y1, s.brd);
    paint_title(c, x0, y0, spin_ch(), s.spin_s, "Memory", s.accent, s.brd);

    int ix = x0 + 2, iw = w - 4;
    int row = y0 + 1;

    float total = g_mem_used + g_mem_buff + g_mem_cache;
    float pct = total / kMemGB * 100.f;

    auto hdr = fmt("%.1f / %.1f GB", static_cast<double>(total), static_cast<double>(kMemGB));
    c.write_text(ix, row, hdr, s.value);
    auto pp = fmt("  %.0f%%", static_cast<double>(pct));
    uint16_t psid = pct < 60 ? s.green : (pct < 85 ? s.yellow : s.red);
    c.write_text(ix + static_cast<int>(hdr.size()), row, pp, psid);
    row++;

    // Segmented gauge bar: used | buff | cache | free
    int bar_w = std::max(8, iw);
    float u_f = g_mem_used / kMemGB, b_f = g_mem_buff / kMemGB;
    float c_f = g_mem_cache / kMemGB;
    int u_w = static_cast<int>(u_f * bar_w);
    int b_w = static_cast<int>(b_f * bar_w);
    int c_w = static_cast<int>(c_f * bar_w);
    int f_w = bar_w - u_w - b_w - c_w;
    int bx = ix;
    for (int i = 0; i < u_w && bx + i < x1; ++i)
        c.set(bx + i, row, U'\u2588', s.mem_used);
    bx += u_w;
    for (int i = 0; i < b_w && bx + i < x1; ++i)
        c.set(bx + i, row, U'\u2588', s.mem_buff);
    bx += b_w;
    for (int i = 0; i < c_w && bx + i < x1; ++i)
        c.set(bx + i, row, U'\u2588', s.mem_cache);
    bx += c_w;
    for (int i = 0; i < f_w && bx + i < x1; ++i)
        c.set(bx + i, row, U'\u2591', s.mem_free);
    row += 2;

    // Breakdown with mini bars
    struct { const char* lbl; float gb; uint16_t sid; } segs[] = {
        {"used ", g_mem_used, s.mem_used},
        {"buff ", g_mem_buff, s.mem_buff},
        {"cache", g_mem_cache, s.mem_cache},
        {"free ", kMemGB - total, s.mem_free},
    };
    int mb_w = std::max(4, iw - 14);
    for (auto& seg : segs) {
        if (row >= y1) break;
        c.write_text(ix, row, seg.lbl, s.label);
        auto v = fmt(" %5.1fG ", static_cast<double>(seg.gb));
        c.write_text(ix + 5, row, v, s.num);
        hbar(c, ix + 13, row, mb_w, seg.gb / kMemGB, seg.sid, s.dim);
        row++;
    }
    row++;

    // History sparkline
    if (row < y1) {
        c.write_text(ix, row, "history", s.muted);
        row++;
    }
    if (row < y1) {
        spark(c, ix, row, iw, g_mem_hist, s.mem_used);
    }
}

static void paint_net(Canvas& c, int x0, int y0, int w, int h) {
    if (w < 12 || h < 5) return;
    int x1 = x0 + w - 1, y1 = y0 + h - 1;
    paint_box(c, x0, y0, x1, y1, s.brd);
    paint_title(c, x0, y0, spin_ch(), s.spin_s, "Network", s.accent, s.brd);

    int ix = x0 + 2, iw = w - 4;
    int row = y0 + 1;

    // Rates
    c.set(ix, row, U'\u2193', s.rx_g[5]);
    auto rxs = fmt(" %5.1f MB/s", static_cast<double>(g_rx));
    c.write_text(ix + 1, row, rxs, s.value);
    int off = ix + 1 + static_cast<int>(rxs.size()) + 2;
    c.set(off, row, U'\u2191', s.tx_g[5]);
    auto txs = fmt(" %5.1f MB/s", static_cast<double>(g_tx));
    c.write_text(off + 1, row, txs, s.num);
    row++;

    // RX braille chart
    int chart_h = std::max(1, (h - 5) / 2);
    c.write_text(ix, row, "rx", s.muted);
    area_chart(c, ix + 3, row, iw - 3, chart_h, g_rx_hist, s.rx_g, 6);
    row += chart_h;

    // TX braille chart
    c.write_text(ix, row, "tx", s.muted);
    area_chart(c, ix + 3, row, iw - 3, chart_h, g_tx_hist, s.tx_g, 6);
    row += chart_h;

    // Connection stats
    if (row < y1) {
        auto st = fmt("tcp %-5d  udp %-4d  err %d  drop %d",
            g_tcp, g_udp, g_err, g_drop);
        c.write_text(ix, row, st, s.label);
    }
}

static void paint_disk(Canvas& c, int x0, int y0, int w, int h) {
    if (w < 12 || h < 5) return;
    int x1 = x0 + w - 1, y1 = y0 + h - 1;
    paint_box(c, x0, y0, x1, y1, s.brd);
    paint_title(c, x0, y0, spin_ch(), s.spin_s, "Disk I/O", s.accent, s.brd);

    int ix = x0 + 2, iw = w - 4;
    int row = y0 + 1;

    auto rates = fmt("R %5.0f MB/s  W %5.0f MB/s",
        static_cast<double>(g_dr), static_cast<double>(g_dw));
    c.write_text(ix, row, rates, s.value);
    row++;

    // Sparklines for read/write
    int spark_w = std::max(6, iw - 5);
    c.write_text(ix, row, "read ", s.muted);
    spark(c, ix + 5, row, spark_w, g_dr_hist, s.green);
    row++;
    c.write_text(ix, row, "write", s.muted);
    spark(c, ix + 5, row, spark_w, g_dw_hist, s.yellow);
    row++;

    // IOPS
    if (row < y1) {
        auto iops = fmt("iops %.1fk r  %.1fk w",
            static_cast<double>(g_ior), static_cast<double>(g_iow));
        c.write_text(ix, row, iops, s.label);
        row++;
    }

    // Utilization gauge
    if (row < y1) {
        c.write_text(ix, row, "util ", s.label);
        int uw = std::max(4, iw - 12);
        uint16_t usid = g_du < 50 ? s.green : (g_du < 80 ? s.yellow : s.red);
        hbar(c, ix + 5, row, uw, g_du / 100.f, usid, s.dim);
        auto up = fmt(" %2.0f%%", static_cast<double>(g_du));
        c.write_text(ix + 5 + uw, row, up, s.num);
    }
}

static void paint_procs(Canvas& c, int x0, int y0, int w, int h) {
    if (w < 40 || h < 3) return;
    int x1 = x0 + w - 1, y1 = y0 + h - 1;
    paint_box(c, x0, y0, x1, y1, s.brd);
    paint_title(c, x0, y0, spin_ch(), s.spin_s, "Processes", s.accent, s.brd);

    int ix = x0 + 2, row = y0 + 1;

    // Header
    c.write_text(ix, row,
        "  PID    USER     PROCESS           CPU%  MEM%  VIRT   RES", s.proc_hd);
    row++;

    // Separator line
    for (int x = ix; x < x1 - 1; ++x) c.set(x, row, U'\u2500', s.dim);
    row++;

    int show = std::clamp(h - 4, 0, kProcs);
    static constexpr char32_t kSt[] = {U'\u25CF', U'\u25CB', U'\u25CB'};
    static const uint16_t kStSid[] = {s.st_run, s.st_sleep, s.st_idle};

    for (int i = 0; i < show && row < y1; ++i, ++row) {
        auto& p = g_procs[i];
        float cpu = std::max(0.1f,
            p.base_cpu + p.base_cpu * 0.3f * fsin(g_t * (1.f + i * 0.4f)));

        // Alternating row background
        if (i % 2 == 1) {
            for (int x = ix; x < x1; ++x) c.set(x, row, U' ', s.proc_alt);
        }

        auto line = fmt("%5d    %-8s %-18s %5.1f  %5.1f  %-6s %-6s",
            p.pid, p.user, p.name,
            static_cast<double>(cpu), static_cast<double>(p.base_mem),
            p.virt, p.res);
        uint16_t rsid = (i % 2 == 1) ? s.proc_alt : s.dim;
        // Write the line
        c.write_text(ix, row, line, s.proc_nm);

        // Color the CPU value specifically
        int cpu_off = 33;
        auto cpu_str = fmt("%5.1f", static_cast<double>(cpu));
        uint16_t cpu_sid = s.num;
        if (cpu > 10) cpu_sid = s.yellow;
        if (cpu > 50) cpu_sid = s.red;
        c.write_text(ix + cpu_off, row, cpu_str, cpu_sid);

        // Status indicator
        int si = x1 - 3;
        c.set(si, row, kSt[p.st], kStSid[p.st]);
    }
}

static void paint_status(Canvas& c, int W, int H) {
    int y = H - 1;
    auto bbg = Color::rgb(12, 12, 18);
    for (int x = 0; x < W; ++x) c.set(x, y, U' ', s.bar_bg);

    int x = 1;
    c.write_text(x, y, " maya ", s.brand); x += 6;
    c.write_text(x, y, " | ", s.bar_hint); x += 3;
    c.write_text(x, y, kThemes[g_theme].name, s.accent); x += static_cast<int>(
        std::string_view(kThemes[g_theme].name).size());
    c.write_text(x, y, " | ", s.bar_hint); x += 3;

    spark(c, x, y, 20, g_cpu_hist, s.brand);
    c.write_text(W - 18, y, " [t]heme  [q]uit ", s.bar_hint);
}

// ── Main ────────────────────────────────────────────────────────────────────

int main() {
    // Warm up history
    for (float t = 0.f; t < 40.f; t += 0.17f) {
        g_t = t;
        tick(0.17f);
    }

    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();

    auto result = canvas_run(
        {.fps = 30, .title = "dashboard \xc2\xb7 maya"},

        [&](StylePool& pool, int, int) {
            intern_styles(pool);
        },

        [](const Event& ev) -> bool {
            if (key(ev, 'q') || key(ev, SpecialKey::Escape)) return false;
            on(ev, 't', [] { g_theme = (g_theme + 1) % kThemeN; });
            on(ev, 'T', [] { g_theme = (g_theme + kThemeN - 1) % kThemeN; });
            return true;
        },

        [&](Canvas& canvas, int W, int H) {
            auto now = Clock::now();
            float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
            last = now;
            ++g_frame;
            tick(dt);

            // Adaptive layout
            int split_x = W * 3 / 5;
            int body = H - 2;
            int top_h = body * 2 / 5;
            int mid_h = body * 1 / 4;
            int bot_h = body - top_h - mid_h;

            paint_header(canvas, W);
            paint_cpu(canvas, 0, 1, split_x, top_h);
            paint_mem(canvas, split_x, 1, W - split_x, top_h);
            paint_net(canvas, 0, 1 + top_h, split_x, mid_h);
            paint_disk(canvas, split_x, 1 + top_h, W - split_x, mid_h);
            paint_procs(canvas, 0, 1 + top_h + mid_h, W, bot_h);
            paint_status(canvas, W, H);
        }
    );

    if (!result) {
        std::fprintf(stderr, "maya: %s\n", result.error().message.c_str());
        return 1;
    }
}
