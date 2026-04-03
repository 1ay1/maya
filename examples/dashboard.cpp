// maya — live system monitoring dashboard
//
// Dense, flashy dashboard with vertical bar charts, per-core CPU gauges,
// network/disk sparklines, process table, and live system stats.
// All data is simulated with multi-harmonic oscillators for realism.
//
// Keys: q/ESC = quit   t = cycle theme

#include <maya/maya.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

using namespace maya;

// ── Fast sin ─────────────────────────────────────────────────────────────────

static float fsin(float x) noexcept {
    constexpr float tp = 1.f / (2.f * 3.14159265f);
    x *= tp; x -= 0.25f + std::floor(x + 0.25f);
    x *= 16.f * (std::fabs(x) - 0.5f);
    x += 0.225f * x * (std::fabs(x) - 1.f);
    return x;
}

// ── Globals ──────────────────────────────────────────────────────────────────

static float g_time = 0.f;

// CPU: 8 cores + history ring buffers
static constexpr int kCores = 8;
static constexpr int kHistMax = 200;

static float g_cpu[kCores];
static float g_cpu_avg;
static std::deque<float> g_cpu_hist;               // avg history for big chart
static std::deque<float> g_core_hist[kCores];      // per-core mini sparklines

// Memory
static float g_mem_used, g_mem_buff, g_mem_cache;
static constexpr float kMemTotal = 16.0f;          // GB
static std::deque<float> g_mem_hist;

// Network
static float g_net_rx, g_net_tx;
static std::deque<float> g_net_rx_hist, g_net_tx_hist;
static int g_tcp = 842, g_udp = 156;

// Disk
static float g_disk_r, g_disk_w;
static std::deque<float> g_disk_r_hist, g_disk_w_hist;
static float g_iops_r, g_iops_w;

// System info
static float g_load[3];
static int   g_tasks = 312, g_threads = 1847;

// Process table
struct Proc {
    const char* user; const char* name; int pid;
    float base_cpu, base_mem;
    const char* virt; const char* res;
    int status; // 0=run 1=sleep 2=idle
};

static Proc g_procs[] = {
    {"www",    "node",          1284, 14.2f,  3.2f, "1.2G", "512M", 0},
    {"pg",     "postgres",       892,  8.1f, 15.4f, "2.8G", "1.1G", 0},
    {"www",    "nginx",         2041,  3.8f,  1.1f, "340M", "128M", 0},
    {"redis",  "redis-server",  1567,  2.4f,  4.2f, "890M", "672M", 2},
    {"app",    "worker-7",      3201,  1.6f,  0.8f, "256M",  "92M", 0},
    {"root",   "containerd",    1102,  1.2f,  2.1f, "1.4G", "340M", 0},
    {"app",    "worker-3",      3198,  0.9f,  0.7f, "256M",  "88M", 1},
    {"app",    "worker-5",      3199,  0.8f,  0.6f, "256M",  "84M", 0},
    {"root",   "systemd",          1,  0.4f,  0.5f, "168M",  "12M", 1},
    {"root",   "dockerd",       1340,  0.6f,  1.8f, "920M", "280M", 0},
    {"root",   "kubelet",       1401,  0.5f,  1.2f, "680M", "190M", 0},
    {"root",   "sshd",          1820,  0.1f,  0.1f,  "72M",   "8M", 1},
    {"nobody", "dnsmasq",       1910,  0.1f,  0.1f,  "48M",   "6M", 1},
    {"root",   "cron",          1055,  0.0f,  0.3f,  "56M",   "4M", 1},
    {"www",    "php-fpm",       2280,  1.1f,  0.9f, "320M", "110M", 0},
    {"app",    "celery",        2410,  0.7f,  0.5f, "280M",  "96M", 1},
    {"root",   "journald",       640,  0.2f,  0.4f, "160M",  "48M", 1},
    {"root",   "udevd",          380,  0.0f,  0.1f,  "40M",   "4M", 1},
};
static constexpr int kProcCount = 18;

// ── Simulation ───────────────────────────────────────────────────────────────

static float wave(float t, float base, float a1, float f1,
                  float a2 = 0, float f2 = 0, float a3 = 0, float f3 = 0) {
    return base + a1 * fsin(t * f1) + a2 * fsin(t * f2) + a3 * fsin(t * f3);
}

static void push_hist(std::deque<float>& h, float v, int max_len) {
    h.push_back(v);
    while (static_cast<int>(h.size()) > max_len) h.pop_front();
}

static void tick(float dt, int hist_w) {
    g_time += dt;
    float t = g_time;

    // Per-core CPU — different phases give them distinct personalities
    g_cpu[0] = std::clamp(wave(t, 65, 25, 0.7f,  10, 1.9f,  5, 4.1f), 2.f, 99.f);
    g_cpu[1] = std::clamp(wave(t, 30, 20, 0.5f,  12, 2.3f,  4, 3.7f), 2.f, 99.f);
    g_cpu[2] = std::clamp(wave(t, 82, 15, 0.9f,   8, 1.5f,  3, 5.2f), 2.f, 99.f);
    g_cpu[3] = std::clamp(wave(t, 50, 22, 0.6f,  14, 2.8f,  6, 3.3f), 2.f, 99.f);
    g_cpu[4] = std::clamp(wave(t, 60, 18, 1.1f,   9, 1.7f,  5, 4.5f), 2.f, 99.f);
    g_cpu[5] = std::clamp(wave(t, 20, 15, 0.4f,   8, 3.1f,  3, 2.2f), 2.f, 99.f);
    g_cpu[6] = std::clamp(wave(t, 88, 10, 1.3f,   5, 0.8f,  2, 6.1f), 2.f, 99.f);
    g_cpu[7] = std::clamp(wave(t, 38, 24, 0.8f,  11, 2.1f,  7, 3.9f), 2.f, 99.f);

    g_cpu_avg = 0;
    for (int i = 0; i < kCores; ++i) g_cpu_avg += g_cpu[i];
    g_cpu_avg /= kCores;

    // Memory
    g_mem_used  = std::clamp(wave(t, 8.2f, 1.2f, 0.3f, 0.4f, 1.1f), 5.f, 12.f);
    g_mem_buff  = std::clamp(wave(t, 2.1f, 0.5f, 0.2f, 0.2f, 0.7f), 0.8f, 3.5f);
    g_mem_cache = std::clamp(wave(t, 3.4f, 0.8f, 0.15f, 0.3f, 0.5f), 1.5f, 5.f);

    // Network
    g_net_tx = std::max(0.1f, wave(t, 12.4f, 6.f, 1.5f, 3.f, 3.7f, 1.5f, 0.4f));
    g_net_rx = std::max(0.1f, wave(t, 48.2f, 18.f, 0.9f, 8.f, 2.3f, 4.f, 0.5f));
    g_tcp = std::clamp(static_cast<int>(wave(t, 842, 80, 0.3f, 30, 1.2f)), 600, 1100);
    g_udp = std::clamp(static_cast<int>(wave(t, 156, 40, 0.5f, 15, 1.8f)), 80, 250);

    // Disk
    g_disk_r = std::max(0.f, wave(t, 124.f, 50.f, 1.2f, 20.f, 2.8f, 10.f, 0.3f));
    g_disk_w = std::max(0.f, wave(t, 45.f, 20.f, 0.8f, 10.f, 2.1f, 5.f, 0.5f));
    g_iops_r = std::max(0.f, wave(t, 28.4f, 12.f, 1.5f, 4.f, 3.2f));
    g_iops_w = std::max(0.f, wave(t, 12.1f, 6.f, 0.9f, 2.f, 2.7f));

    // Load average (slow-moving)
    g_load[0] = std::max(0.1f, wave(t, 4.82f, 1.5f, 0.15f, 0.5f, 0.4f));
    g_load[1] = std::max(0.1f, wave(t, 3.21f, 1.0f, 0.08f, 0.3f, 0.2f));
    g_load[2] = std::max(0.1f, wave(t, 2.15f, 0.6f, 0.05f, 0.2f, 0.1f));

    g_tasks   = std::clamp(static_cast<int>(wave(t, 312, 15, 0.2f, 5, 0.8f)), 280, 350);
    g_threads = std::clamp(static_cast<int>(wave(t, 1847, 80, 0.3f, 30, 0.9f)), 1700, 2000);

    // Process CPU jitter
    for (int i = 0; i < kProcCount; ++i) {
        float b = g_procs[i].base_cpu;
        g_procs[i].base_cpu = b; // keep base
    }

    // History (every ~5 frames at 30fps ≈ 6 samples/sec)
    static int htick = 0;
    if (++htick >= 5) {
        htick = 0;
        push_hist(g_cpu_hist, g_cpu_avg / 100.f, hist_w);
        for (int i = 0; i < kCores; ++i)
            push_hist(g_core_hist[i], g_cpu[i] / 100.f, hist_w);
        push_hist(g_mem_hist, (g_mem_used + g_mem_buff + g_mem_cache) / kMemTotal, hist_w);
        push_hist(g_net_rx_hist, std::min(g_net_rx / 80.f, 1.f), hist_w);
        push_hist(g_net_tx_hist, std::min(g_net_tx / 30.f, 1.f), hist_w);
        push_hist(g_disk_r_hist, std::min(g_disk_r / 200.f, 1.f), hist_w);
        push_hist(g_disk_w_hist, std::min(g_disk_w / 100.f, 1.f), hist_w);
    }
}

// ── Themes ───────────────────────────────────────────────────────────────────

static constexpr int kThemeCount = 3;
struct ThemeDef { const char* name; uint8_t ar,ag,ab, br,bg_,bb, cr,cg,cb; };
static constexpr ThemeDef kThemeDefs[kThemeCount] = {
    {"midnight",  80,140,255,  55,65,90,   120,160,255},
    {"forest",    60,220,100,  50,80,55,   100,240,140},
    {"ember",     255,120,60,  90,60,50,   255,180,80},
};
static int g_theme = 0;

// ── Styles ───────────────────────────────────────────────────────────────────

struct Styles {
    uint16_t border, title, label, value, muted, dim_bar;
    // Gauge gradient: 12 steps from green→yellow→red
    uint16_t gauge[12];
    // Bar chart gradient: 16 steps (bottom=cool, top=hot)
    uint16_t bar_grad[16];
    // Sparkline colors
    uint16_t spark_rx, spark_tx, spark_read, spark_write, spark_mem;
    // Process table
    uint16_t tbl_head, tbl_name, tbl_user, tbl_num;
    uint16_t dot[3]; // run, sleep, idle
    // Status bar
    uint16_t bar_bg, bar_brand, bar_hint, bar_theme;
    // System stats
    uint16_t load_ok, load_warn, load_high;
};
static Styles g_sty{};

static void build_styles(StylePool& pool) {
    const auto& t = kThemeDefs[g_theme];
    auto& s = g_sty;

    s.border  = pool.intern(Style{}.with_fg(Color::rgb(t.br, t.bg_, t.bb)));
    s.title   = pool.intern(Style{}.with_bold().with_fg(Color::rgb(t.cr, t.cg, t.cb)));
    s.label   = pool.intern(Style{}.with_fg(Color::rgb(110, 110, 130)));
    s.value   = pool.intern(Style{}.with_bold().with_fg(Color::rgb(210, 210, 225)));
    s.muted   = pool.intern(Style{}.with_fg(Color::rgb(55, 55, 70)));
    s.dim_bar = pool.intern(Style{}.with_fg(Color::rgb(30, 30, 40)));

    // Gauge gradient: green → yellow → red
    for (int i = 0; i < 12; ++i) {
        float f = static_cast<float>(i) / 11.f;
        uint8_t r, g, b;
        if (f < 0.5f) {
            float t2 = f * 2.f;
            r = static_cast<uint8_t>(60 + 180 * t2);
            g = static_cast<uint8_t>(210 - 30 * t2);
            b = static_cast<uint8_t>(100 - 60 * t2);
        } else {
            float t2 = (f - 0.5f) * 2.f;
            r = static_cast<uint8_t>(240);
            g = static_cast<uint8_t>(180 - 140 * t2);
            b = static_cast<uint8_t>(40 - 30 * t2);
        }
        s.gauge[i] = pool.intern(Style{}.with_fg(Color::rgb(r, g, b)));
    }

    // Vertical bar chart gradient: 16 steps (teal→green→yellow→red)
    for (int i = 0; i < 16; ++i) {
        float f = static_cast<float>(i) / 15.f;
        uint8_t r, g, b;
        if (f < 0.33f) {
            float t2 = f / 0.33f;
            r = static_cast<uint8_t>(20 + 40 * t2);
            g = static_cast<uint8_t>(140 + 80 * t2);
            b = static_cast<uint8_t>(160 - 100 * t2);
        } else if (f < 0.66f) {
            float t2 = (f - 0.33f) / 0.33f;
            r = static_cast<uint8_t>(60 + 180 * t2);
            g = static_cast<uint8_t>(220 - 20 * t2);
            b = static_cast<uint8_t>(60 - 40 * t2);
        } else {
            float t2 = (f - 0.66f) / 0.34f;
            r = static_cast<uint8_t>(240);
            g = static_cast<uint8_t>(200 - 160 * t2);
            b = static_cast<uint8_t>(20);
        }
        s.bar_grad[i] = pool.intern(Style{}.with_fg(Color::rgb(r, g, b)));
    }

    s.spark_rx    = pool.intern(Style{}.with_fg(Color::rgb(80, 200, 255)));
    s.spark_tx    = pool.intern(Style{}.with_fg(Color::rgb(160, 120, 255)));
    s.spark_read  = pool.intern(Style{}.with_fg(Color::rgb(100, 220, 160)));
    s.spark_write = pool.intern(Style{}.with_fg(Color::rgb(255, 160, 80)));
    s.spark_mem   = pool.intern(Style{}.with_fg(Color::rgb(180, 130, 255)));

    s.tbl_head = pool.intern(Style{}.with_bold().with_fg(Color::rgb(130, 130, 155)));
    s.tbl_name = pool.intern(Style{}.with_fg(Color::rgb(180, 180, 200)));
    s.tbl_user = pool.intern(Style{}.with_fg(Color::rgb(120, 140, 170)));
    s.tbl_num  = pool.intern(Style{}.with_fg(Color::rgb(150, 150, 170)));
    s.dot[0]   = pool.intern(Style{}.with_fg(Color::rgb(80, 220, 120)));
    s.dot[1]   = pool.intern(Style{}.with_fg(Color::rgb(100, 100, 120)));
    s.dot[2]   = pool.intern(Style{}.with_fg(Color::rgb(200, 180, 60)));

    s.load_ok   = pool.intern(Style{}.with_fg(Color::rgb(80, 220, 120)));
    s.load_warn = pool.intern(Style{}.with_fg(Color::rgb(240, 200, 60)));
    s.load_high = pool.intern(Style{}.with_fg(Color::rgb(240, 80, 80)));

    s.bar_bg    = pool.intern(Style{}.with_fg(Color::rgb(40,40,50)).with_bg(Color::rgb(12,12,18)));
    s.bar_brand = pool.intern(Style{}.with_bold().with_fg(Color::rgb(t.ar,t.ag,t.ab)).with_bg(Color::rgb(12,12,18)));
    s.bar_hint  = pool.intern(Style{}.with_fg(Color::rgb(65,65,80)).with_bg(Color::rgb(12,12,18)));
    s.bar_theme = pool.intern(Style{}.with_fg(Color::rgb(t.cr,t.cg,t.cb)).with_bg(Color::rgb(12,12,18)));
}

// ── Drawing primitives ───────────────────────────────────────────────────────

static int wstr(Canvas& c, int x, int y, const char* s, uint16_t sid) {
    auto len = std::strlen(s);
    c.write_text(x, y, {s, len}, sid);
    return x + static_cast<int>(len);
}

static void draw_box(Canvas& c, int x0, int y0, int x1, int y1, uint16_t sid) {
    if (x1 <= x0 || y1 <= y0) return;
    c.set(x0, y0, U'╭', sid); c.set(x1, y0, U'╮', sid);
    c.set(x0, y1, U'╰', sid); c.set(x1, y1, U'╯', sid);
    for (int x = x0+1; x < x1; ++x) { c.set(x, y0, U'─', sid); c.set(x, y1, U'─', sid); }
    for (int y = y0+1; y < y1; ++y) { c.set(x0, y, U'│', sid); c.set(x1, y, U'│', sid); }
}

static void draw_panel(Canvas& c, int x0, int y0, int x1, int y1, const char* title) {
    draw_box(c, x0, y0, x1, y1, g_sty.border);
    int tx = x0 + 2;
    c.set(tx++, y0, U' ', g_sty.border);
    tx = wstr(c, tx, y0, title, g_sty.title);
    c.set(tx, y0, U' ', g_sty.border);
}

// ── Sparkline (single row) ───────────────────────────────────────────────────

static constexpr char32_t kBlk[] = {U'▁',U'▂',U'▃',U'▄',U'▅',U'▆',U'▇',U'█'};

static void draw_spark(Canvas& c, int x, int y, int w,
                       const std::deque<float>& data, uint16_t sid) {
    int start = std::max(0, static_cast<int>(data.size()) - w);
    int dx = 0;
    for (int i = start; i < static_cast<int>(data.size()) && dx < w; ++i, ++dx) {
        int lv = std::clamp(static_cast<int>(data[static_cast<std::size_t>(i)] * 7.99f), 0, 7);
        c.set(x + dx, y, kBlk[lv], sid);
    }
    for (; dx < w; ++dx) c.set(x + dx, y, kBlk[0], g_sty.muted);
}

// ── Vertical bar chart (multi-row, gradient colored) ─────────────────────────

static void draw_vbar_chart(Canvas& c, int x0, int y0, int w, int h,
                            const std::deque<float>& data) {
    // Each column = one data point. Height proportional to value.
    // Colors: gradient from bottom (cool) to top (hot).
    int start = std::max(0, static_cast<int>(data.size()) - w);
    int dx = 0;
    for (int i = start; i < static_cast<int>(data.size()) && dx < w; ++i, ++dx) {
        float v = std::clamp(data[static_cast<std::size_t>(i)], 0.f, 1.f);
        int filled = static_cast<int>(v * static_cast<float>(h) * 8.f); // in 1/8ths

        for (int row = 0; row < h; ++row) {
            int cell_y = y0 + h - 1 - row; // bottom-up
            int cell_eighths = filled - row * 8;

            if (cell_eighths <= 0) {
                // Empty
                c.set(x0 + dx, cell_y, U' ', g_sty.muted);
            } else if (cell_eighths >= 8) {
                // Full block
                float row_f = static_cast<float>(row) / static_cast<float>(h - 1);
                int gi = std::clamp(static_cast<int>(row_f * 15.f), 0, 15);
                c.set(x0 + dx, cell_y, U'█', g_sty.bar_grad[gi]);
            } else {
                // Partial block (top of the bar)
                float row_f = static_cast<float>(row) / static_cast<float>(h - 1);
                int gi = std::clamp(static_cast<int>(row_f * 15.f), 0, 15);
                c.set(x0 + dx, cell_y, kBlk[cell_eighths - 1], g_sty.bar_grad[gi]);
            }
        }
    }
    // Fill remaining columns
    for (; dx < w; ++dx) {
        for (int row = 0; row < h; ++row)
            c.set(x0 + dx, y0 + row, U' ', g_sty.muted);
    }
}

// ── Gauge bar (inline, compact) ──────────────────────────────────────────────

static void draw_gauge(Canvas& c, int x, int y, int bar_w, float pct) {
    pct = std::clamp(pct, 0.f, 100.f);
    int filled = static_cast<int>(pct / 100.f * static_cast<float>(bar_w));
    int gi = std::clamp(static_cast<int>(pct / 100.f * 11.f), 0, 11);

    for (int i = 0; i < bar_w; ++i) {
        if (i < filled)
            c.set(x + i, y, U'█', g_sty.gauge[gi]);
        else
            c.set(x + i, y, U'░', g_sty.dim_bar);
    }
}

// ── Panel painters ───────────────────────────────────────────────────────────

static void paint_cpu(Canvas& c, int x0, int y0, int x1, int y1) {
    draw_panel(c, x0, y0, x1, y1, "CPU");
    int iw = x1 - x0 - 2;   // inner width
    int ix = x0 + 1;
    int iy = y0 + 1;

    // Big vertical bar chart — takes most of the panel height
    int chart_h = y1 - y0 - 5; // leave 4 rows for core gauges + stats
    if (chart_h < 3) chart_h = 3;

    // Y-axis labels
    for (int row = 0; row < chart_h; ++row) {
        int pct = 100 - static_cast<int>(static_cast<float>(row) / static_cast<float>(chart_h - 1) * 100.f);
        if (row == 0 || row == chart_h - 1 || row == chart_h / 2) {
            char buf[6];
            std::snprintf(buf, sizeof buf, "%3d", pct);
            wstr(c, ix, iy + row, buf, g_sty.muted);
        }
        c.set(ix + 3, iy + row, U'│', g_sty.muted);
    }

    draw_vbar_chart(c, ix + 4, iy, iw - 4, chart_h, g_cpu_hist);

    // Per-core compact gauges (2 rows of 4 cores each)
    int gy = iy + chart_h;
    int gw = (iw - 2) / 4; // width per core entry

    for (int i = 0; i < kCores; ++i) {
        int row = i / 4;
        int col = i % 4;
        int gx = ix + 1 + col * gw;
        int gy2 = gy + row;

        char label[6];
        std::snprintf(label, sizeof label, "c%d ", i);
        int lx = wstr(c, gx, gy2, label, g_sty.label);

        int bw = gw - (lx - gx) - 5;
        if (bw < 2) bw = 2;
        draw_gauge(c, lx, gy2, bw, g_cpu[i]);

        char pct[6];
        std::snprintf(pct, sizeof pct, "%3.0f%%", g_cpu[i]);
        int gi = std::clamp(static_cast<int>(g_cpu[i] / 100.f * 11.f), 0, 11);
        wstr(c, lx + bw, gy2, pct, g_sty.gauge[gi]);
    }

    // Bottom stats line
    int sy = gy + 2;
    if (sy < y1) {
        char buf[80];
        uint16_t ls0 = g_load[0] < 4.f ? g_sty.load_ok : g_load[0] < 6.f ? g_sty.load_warn : g_sty.load_high;
        uint16_t ls1 = g_load[1] < 4.f ? g_sty.load_ok : g_load[1] < 6.f ? g_sty.load_warn : g_sty.load_high;
        uint16_t ls2 = g_load[2] < 4.f ? g_sty.load_ok : g_load[2] < 6.f ? g_sty.load_warn : g_sty.load_high;

        int sx = wstr(c, ix + 1, sy, "load ", g_sty.label);
        std::snprintf(buf, sizeof buf, "%.2f", g_load[0]); sx = wstr(c, sx, sy, buf, ls0);
        sx = wstr(c, sx, sy, " ", g_sty.muted);
        std::snprintf(buf, sizeof buf, "%.2f", g_load[1]); sx = wstr(c, sx, sy, buf, ls1);
        sx = wstr(c, sx, sy, " ", g_sty.muted);
        std::snprintf(buf, sizeof buf, "%.2f", g_load[2]); sx = wstr(c, sx, sy, buf, ls2);

        sx += 2;
        std::snprintf(buf, sizeof buf, "tasks %d", g_tasks);
        sx = wstr(c, sx, sy, buf, g_sty.label);
        sx += 2;
        std::snprintf(buf, sizeof buf, "thr %d", g_threads);
        wstr(c, sx, sy, buf, g_sty.label);
    }
}

static void paint_mem(Canvas& c, int x0, int y0, int x1, int y1) {
    draw_panel(c, x0, y0, x1, y1, "Memory");
    int ix = x0 + 2;
    int iy = y0 + 1;
    int iw = x1 - x0 - 3;

    float total_used = g_mem_used + g_mem_buff + g_mem_cache;
    float pct = total_used / kMemTotal * 100.f;

    // Big percentage
    char pct_buf[8];
    std::snprintf(pct_buf, sizeof pct_buf, "%.0f%%", pct);
    int gi = std::clamp(static_cast<int>(pct / 100.f * 11.f), 0, 11);
    int px = wstr(c, ix, iy, pct_buf, g_sty.gauge[gi]);
    wstr(c, px, iy, " used", g_sty.label);

    // Full-width gauge bar
    draw_gauge(c, ix, iy + 1, iw, pct);

    // Breakdown
    char buf[32];
    int by = iy + 3;
    std::snprintf(buf, sizeof buf, "used   %5.1fG", g_mem_used);
    wstr(c, ix, by, buf, g_sty.value);

    std::snprintf(buf, sizeof buf, "buff   %5.1fG", g_mem_buff);
    wstr(c, ix, by + 1, buf, g_sty.spark_mem);

    std::snprintf(buf, sizeof buf, "cache  %5.1fG", g_mem_cache);
    wstr(c, ix, by + 2, buf, g_sty.spark_tx);

    float free_mem = kMemTotal - total_used;
    std::snprintf(buf, sizeof buf, "free   %5.1fG", free_mem);
    wstr(c, ix, by + 3, buf, g_sty.muted);

    std::snprintf(buf, sizeof buf, "total  %5.1fG", kMemTotal);
    wstr(c, ix, by + 4, buf, g_sty.label);

    // Memory vertical bar chart — fill remaining space
    int chart_top = by + 6;
    int chart_h   = y1 - chart_top - 1;
    if (chart_h >= 2) {
        draw_vbar_chart(c, ix, chart_top, iw, chart_h, g_mem_hist);
    }
}

static void paint_net(Canvas& c, int x0, int y0, int x1, int y1) {
    draw_panel(c, x0, y0, x1, y1, "Network");
    int ix = x0 + 2;
    int iy = y0 + 1;
    int iw = x1 - x0 - 3;
    char buf[32];

    // Interface stats
    int rx = wstr(c, ix, iy, "eth0 ", g_sty.label);
    rx = wstr(c, rx, iy, "\xe2\x86\x91", g_sty.spark_tx);  // ↑
    std::snprintf(buf, sizeof buf, " %5.1f MB/s ", g_net_tx);
    rx = wstr(c, rx, iy, buf, g_sty.spark_tx);
    rx = wstr(c, rx, iy, "\xe2\x86\x93", g_sty.spark_rx);  // ↓
    std::snprintf(buf, sizeof buf, " %5.1f MB/s", g_net_rx);
    wstr(c, rx, iy, buf, g_sty.spark_rx);

    // RX sparkline
    wstr(c, ix, iy + 1, " rx ", g_sty.muted);
    draw_spark(c, ix + 4, iy + 1, iw - 4, g_net_rx_hist, g_sty.spark_rx);

    // TX sparkline
    wstr(c, ix, iy + 2, " tx ", g_sty.muted);
    draw_spark(c, ix + 4, iy + 2, iw - 4, g_net_tx_hist, g_sty.spark_tx);

    // Connection counts
    if (iy + 4 < y1) {
        std::snprintf(buf, sizeof buf, "tcp %d  udp %d  rx_err 0  tx_err 0", g_tcp, g_udp);
        wstr(c, ix, iy + 4, buf, g_sty.label);
    }
}

static void paint_disk(Canvas& c, int x0, int y0, int x1, int y1) {
    draw_panel(c, x0, y0, x1, y1, "Disk I/O");
    int ix = x0 + 2;
    int iy = y0 + 1;
    int iw = x1 - x0 - 3;
    char buf[48];

    // Throughput
    std::snprintf(buf, sizeof buf, "sda  R %5.0f MB/s  W %5.0f MB/s", g_disk_r, g_disk_w);
    wstr(c, ix, iy, buf, g_sty.value);

    // Read sparkline
    wstr(c, ix, iy + 1, "read ", g_sty.muted);
    draw_spark(c, ix + 5, iy + 1, iw - 5, g_disk_r_hist, g_sty.spark_read);

    // Write sparkline
    wstr(c, ix, iy + 2, "write", g_sty.muted);
    draw_spark(c, ix + 5, iy + 2, iw - 5, g_disk_w_hist, g_sty.spark_write);

    // IOPS
    if (iy + 4 < y1) {
        std::snprintf(buf, sizeof buf, "iops  %.1fk r/s  %.1fk w/s", g_iops_r, g_iops_w);
        wstr(c, ix, iy + 4, buf, g_sty.label);
    }
}

static void paint_procs(Canvas& c, int x0, int y0, int x1, int y1) {
    draw_panel(c, x0, y0, x1, y1, "Processes");
    int ix = x0 + 2;
    int iy = y0 + 1;
    int iw = x1 - x0 - 3;

    // Fixed column offsets from ix
    const int cPid  = 0;
    const int cUser = 7;
    const int cName = 15;
    const int cCpu  = 29;
    const int cMem  = 36;
    const int cVirt = 43;
    const int cRes  = 50;
    const int cBar  = 57;   // CPU bar starts here

    // Header — aligned to column offsets
    wstr(c, ix + cPid,  iy, "PID",  g_sty.tbl_head);
    wstr(c, ix + cUser, iy, "USER", g_sty.tbl_head);
    wstr(c, ix + cName, iy, "NAME", g_sty.tbl_head);
    wstr(c, ix + cCpu,  iy, "CPU%", g_sty.tbl_head);
    wstr(c, ix + cMem,  iy, "MEM%", g_sty.tbl_head);
    wstr(c, ix + cVirt, iy, "VIRT", g_sty.tbl_head);
    wstr(c, ix + cRes,  iy, "RES",  g_sty.tbl_head);
    wstr(c, ix + cBar,  iy, "LOAD", g_sty.tbl_head);

    // Separator
    for (int x = ix; x < ix + iw && x < x1; ++x)
        c.set(x, iy + 1, U'─', g_sty.muted);

    const char* status_str[] = {"\xe2\x97\x8f run", "\xe2\x97\x8b slp", "\xe2\x97\x8b idl"};

    // Show as many processes as fit in available space
    int visible = std::min(kProcCount, y1 - iy - 2);

    for (int i = 0; i < visible; ++i) {
        const auto& p = g_procs[i];
        int ry = iy + 2 + i;

        // Animated CPU
        float cpu = std::max(0.1f, p.base_cpu + p.base_cpu * 0.3f * fsin(g_time * (1.f + i * 0.4f)));
        int cpu_gi = std::clamp(static_cast<int>(cpu / 20.f * 11.f), 0, 11);

        char buf[16];
        std::snprintf(buf, sizeof buf, "%-7d", p.pid);
        wstr(c, ix + cPid, ry, buf, g_sty.tbl_num);

        std::snprintf(buf, sizeof buf, "%-8s", p.user);
        wstr(c, ix + cUser, ry, buf, g_sty.tbl_user);

        char name_buf[16];
        std::snprintf(name_buf, sizeof name_buf, "%-14s", p.name);
        wstr(c, ix + cName, ry, name_buf, g_sty.tbl_name);

        std::snprintf(buf, sizeof buf, "%5.1f", cpu);
        wstr(c, ix + cCpu, ry, buf, g_sty.gauge[cpu_gi]);

        std::snprintf(buf, sizeof buf, "%5.1f", p.base_mem);
        wstr(c, ix + cMem, ry, buf, g_sty.tbl_num);

        std::snprintf(buf, sizeof buf, "%-6s", p.virt);
        wstr(c, ix + cVirt, ry, buf, g_sty.tbl_num);

        std::snprintf(buf, sizeof buf, "%-6s", p.res);
        wstr(c, ix + cRes, ry, buf, g_sty.tbl_num);

        // CPU bar — scales to 25% max so even low-cpu procs get visible bars
        int bar_x = ix + cBar;
        int bar_w = x1 - bar_x - 1;
        if (bar_w > 4) {
            // Status indicator in first 4 chars
            wstr(c, bar_x, ry, status_str[p.status], g_sty.dot[p.status]);
            int bx = bar_x + 5;
            int bw = x1 - bx - 1;
            if (bw > 0) {
                float scale = std::max(15.f, cpu * 2.5f); // adaptive scale: max bar at ~40% real CPU
                int filled = std::clamp(static_cast<int>(cpu / scale * static_cast<float>(bw)), 1, bw);
                for (int b = 0; b < bw; ++b)
                    c.set(bx + b, ry, b < filled ? U'━' : U'╌', b < filled ? g_sty.gauge[cpu_gi] : g_sty.muted);
            }
        }
    }
}

static void paint_bar(Canvas& c, int W, int H, double fps) {
    int y = H - 1;
    for (int x = 0; x < W; ++x) c.set(x, y, U' ', g_sty.bar_bg);

    int x = 0;
    x = wstr(c, x, y, " maya ", g_sty.bar_brand);
    x = wstr(c, x, y, " \xe2\x94\x82 ", g_sty.bar_hint);
    x = wstr(c, x, y, kThemeDefs[g_theme].name, g_sty.bar_theme);
    x = wstr(c, x, y, " \xe2\x94\x82 ", g_sty.bar_hint);

    char buf[16];
    std::snprintf(buf, sizeof buf, "avg %.0f%%", g_cpu_avg);
    int gi = std::clamp(static_cast<int>(g_cpu_avg / 100.f * 11.f), 0, 11);
    x = wstr(c, x, y, buf, g_sty.gauge[gi]);
    x = wstr(c, x, y, " \xe2\x94\x82 ", g_sty.bar_hint);

    float mem_pct = (g_mem_used + g_mem_buff + g_mem_cache) / kMemTotal * 100.f;
    std::snprintf(buf, sizeof buf, "mem %.0f%%", mem_pct);
    int mg = std::clamp(static_cast<int>(mem_pct / 100.f * 11.f), 0, 11);
    x = wstr(c, x, y, buf, g_sty.gauge[mg]);

    char rbuf[48];
    std::snprintf(rbuf, sizeof rbuf, " [t]heme  [q]uit  %.0f fps ", fps);
    int rlen = static_cast<int>(std::strlen(rbuf));
    if (W - rlen > x) wstr(c, W - rlen, y, rbuf, g_sty.bar_hint);
}

// ── Main layout ──────────────────────────────────────────────────────────────

static void paint(Canvas& canvas, int W, int H, double fps) {
    if (W < 60 || H < 20) {
        canvas.write_text(1, 0, "need 60x20+", 0);
        return;
    }

    // Layout: left 65% = CPU (big chart), right 35% = memory
    //         middle row: network + disk side by side
    //         bottom: process table (full width)
    const int bar_h = 1;
    const int ch = H - bar_h;

    // Vertical splits
    const int cpu_w = W * 65 / 100;
    const int mem_x = cpu_w + 1;

    // Horizontal splits: top ~55%, mid ~20%, bottom ~25%
    int top_h = std::max(12, ch * 55 / 100);
    int mid_h = std::max(7, ch * 20 / 100);
    int bot_h = ch - top_h - mid_h;
    if (bot_h < 5) { bot_h = 5; top_h = ch - mid_h - bot_h; }

    int y0 = 0;
    int y1 = top_h;
    int y2 = y1 + mid_h;
    int y3 = ch;

    int mid_split = W / 2;

    // Top: CPU (left) + Memory (right)
    paint_cpu(canvas, 0, y0, cpu_w - 1, y1 - 1);
    paint_mem(canvas, mem_x, y0, W - 1, y1 - 1);

    // Middle: Network (left) + Disk (right)
    paint_net (canvas, 0,         y1, mid_split - 1, y2 - 1);
    paint_disk(canvas, mid_split, y1, W - 1,         y2 - 1);

    // Bottom: Processes (full width)
    paint_procs(canvas, 0, y2, W - 1, y3 - 1);

    // Status bar
    paint_bar(canvas, W, H, fps);
}

// ── main ─────────────────────────────────────────────────────────────────────

int main() {
    using Clock = std::chrono::steady_clock;
    auto  last = Clock::now();
    int   frames = 0;
    double fps = 30.0;
    auto   fps_clock = Clock::now();

    // Warm up history so charts are full on launch
    for (float t = 0.f; t < 40.f; t += 0.17f) {
        g_time = t;
        tick(0.17f, kHistMax);
        // Force history push every iteration during warmup
        for (int i = 0; i < kCores; ++i)
            push_hist(g_core_hist[i], g_cpu[i] / 100.f, kHistMax);
        push_hist(g_cpu_hist, g_cpu_avg / 100.f, kHistMax);
        push_hist(g_mem_hist, (g_mem_used + g_mem_buff + g_mem_cache) / kMemTotal, kHistMax);
        push_hist(g_net_rx_hist, std::min(g_net_rx / 80.f, 1.f), kHistMax);
        push_hist(g_net_tx_hist, std::min(g_net_tx / 30.f, 1.f), kHistMax);
        push_hist(g_disk_r_hist, std::min(g_disk_r / 200.f, 1.f), kHistMax);
        push_hist(g_disk_w_hist, std::min(g_disk_w / 100.f, 1.f), kHistMax);
    }

    auto result = canvas_run(
        CanvasConfig{.fps = 30, .title = "dashboard \xc2\xb7 maya"},

        [](StylePool& pool, int, int) { build_styles(pool); },

        [](const Event& ev) -> bool {
            if (key(ev, 'q') || key(ev, SpecialKey::Escape)) return false;
            on(ev, 't', [] { g_theme = (g_theme + 1) % kThemeCount; });
            on(ev, 'T', [] { g_theme = (g_theme + 1) % kThemeCount; });
            return true;
        },

        [&](Canvas& canvas, int W, int H) {
            auto now = Clock::now();
            float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
            last = now;

            ++frames;
            double el = std::chrono::duration<double>(now - fps_clock).count();
            if (el >= 0.5) { fps = frames / el; frames = 0; fps_clock = now; }

            // Use chart width based on panel size
            int chart_w = std::max(40, W * 65 / 100 - 6);
            tick(dt, chart_w);
            paint(canvas, W, H, fps);
        }
    );

    if (!result) {
        std::fprintf(stderr, "maya: %s\n", result.error().message.c_str());
        return 1;
    }
}
