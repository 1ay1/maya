// maya — live system monitoring dashboard
//
// Simulated metrics rendered at 30 fps with smooth animations.
// Showcases: progress bars, sparklines, tables, status indicators,
// multi-panel layout, gradient colors — all built with canvas primitives.
//
// Keys: q/ESC = quit   t = cycle theme

#include <maya/maya.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>

using namespace maya;

// ── Fast sin (reused from viz) ───────────────────────────────────────────────

static float fsin(float x) noexcept {
    constexpr float tp = 1.f / (2.f * 3.14159265f);
    x *= tp;
    x -= 0.25f + std::floor(x + 0.25f);
    x *= 16.f * (std::fabs(x) - 0.5f);
    x += 0.225f * x * (std::fabs(x) - 1.f);
    return x;
}

// ── Simulated metrics ────────────────────────────────────────────────────────

static float g_time = 0.f;

struct Metric {
    const char* label;
    float       value;               // 0–100
    std::deque<float> history;       // sparkline ring buffer (0–1)
};

static Metric g_metrics[4] = {
    {"CPU ",  0.f, {}},
    {"MEM ",  0.f, {}},
    {"DISK",  0.f, {}},
    {"NET ",  0.f, {}},
};

static float g_upload   = 0.f;       // MB/s
static float g_download = 0.f;
static int   g_latency  = 0;

struct Process {
    const char* name;
    int         pid;
    float       cpu;                 // animated
    float       mem;
    const char* status;
    int         status_kind;         // 0=running 1=idle 2=sleeping
};

static Process g_procs[] = {
    {"node",      1284, 12.4f,  3.2f, "running",  0},
    {"postgres",   892,  8.1f, 15.4f, "running",  0},
    {"nginx",     2041,  2.3f,  1.1f, "running",  0},
    {"redis",     1567,  1.8f,  4.2f, "idle",     1},
    {"worker-7",  3201,  0.4f,  0.8f, "sleeping", 2},
    {"cron",      1102,  0.1f,  0.3f, "sleeping", 2},
};
static constexpr int kProcCount = 6;

struct Service {
    const char* name;
    int         kind;                // 0=healthy 1=degraded 2=down
    int         latency;
};

static Service g_services[] = {
    {"API Gateway ",  0, 142},
    {"Auth Service",  0,  28},
    {"Database    ",  0,  12},
    {"Cache       ",  1, 340},
    {"Queue       ",  0,  45},
    {"Storage     ",  0,  67},
};
static constexpr int kSvcCount = 6;

struct LogEntry {
    const char* time;
    const char* text;
    int         kind;                // 0=info 1=success 2=warning 3=error
};

static const LogEntry g_log[] = {
    {"14:03:12", "Deploy v2.4.1 complete",    1},
    {"14:02:58", "Scale up: 3 \xe2\x86\x92 5 replicas",  0},
    {"14:02:31", "Alert: memory > 80%",       2},
    {"14:01:44", "Health check passed",       0},
    {"14:01:21", "Certificate renewed",       1},
    {"14:00:55", "Config reload: nginx",      0},
    {"14:00:12", "Backup completed",          1},
    {"13:59:38", "Log rotation done",         0},
    {"13:58:51", "Cache flush triggered",     2},
    {"13:58:02", "New connection: db-pool-3", 0},
};
static constexpr int kLogCount = 10;

static constexpr int kSparkLen = 60;

static void tick(float dt) {
    g_time += dt;

    // Metric oscillations — each with unique frequency/phase for realism
    g_metrics[0].value = std::clamp(42.f + 28.f * fsin(g_time * 0.7f) + 12.f * fsin(g_time * 1.9f) + 8.f * fsin(g_time * 3.1f), 2.f, 98.f);
    g_metrics[1].value = std::clamp(68.f + 14.f * fsin(g_time * 0.3f) + 6.f * fsin(g_time * 1.1f), 40.f, 95.f);
    g_metrics[2].value = std::clamp(55.f + 8.f * fsin(g_time * 0.15f) + 3.f * fsin(g_time * 0.8f), 30.f, 85.f);
    g_metrics[3].value = std::clamp(25.f + 20.f * fsin(g_time * 1.2f) + 15.f * fsin(g_time * 2.8f), 1.f, 90.f);

    g_upload   = std::max(0.f, 1.2f + 0.8f * fsin(g_time * 1.5f) + 0.4f * fsin(g_time * 3.7f));
    g_download = std::max(0.f, 4.8f + 2.1f * fsin(g_time * 0.9f) + 1.2f * fsin(g_time * 2.3f));
    g_latency  = std::clamp(static_cast<int>(12.f + 8.f * fsin(g_time * 2.f) + 4.f * fsin(g_time * 5.f)), 3, 45);

    // History for sparklines (~2 samples/sec at 30fps → every 15 frames)
    static int spark_tick = 0;
    if (++spark_tick >= 15) {
        spark_tick = 0;
        for (auto& m : g_metrics) {
            m.history.push_back(m.value / 100.f);
            while (static_cast<int>(m.history.size()) > kSparkLen)
                m.history.pop_front();
        }
    }

    // Animate process CPU values
    for (int i = 0; i < kProcCount; ++i) {
        float base = g_procs[i].cpu;
        g_procs[i].cpu = std::max(0.1f, base + base * 0.3f * fsin(g_time * (1.f + i * 0.4f)));
    }

    // Animate service latency
    for (int i = 0; i < kSvcCount; ++i) {
        int base = g_services[i].latency;
        g_services[i].latency = std::max(1, base + static_cast<int>(base * 0.2f * fsin(g_time * (0.8f + i * 0.3f))));
    }
    // Cache sometimes degrades
    g_services[3].kind = (fsin(g_time * 0.4f) > 0.3f) ? 1 : 0;
}

// ── Styles ───────────────────────────────────────────────────────────────────

static constexpr int kThemes = 3;
struct ThemeDef {
    const char* name;
    uint8_t border_r, border_g, border_b;
    uint8_t title_r,  title_g,  title_b;
    uint8_t accent_r, accent_g, accent_b;
};

static constexpr ThemeDef kThemes_def[kThemes] = {
    {"midnight", 55, 65, 90,   100, 140, 220,   80, 140, 255},
    {"forest",   50, 80, 55,   80, 200, 120,    60, 220, 100},
    {"ember",    90, 60, 50,   240, 140, 80,    255, 120, 60},
};

struct Styles {
    // Panel chrome
    uint16_t border, corner, title, label, value, muted, bar_bg;
    // Gauge fills
    uint16_t gauge_green, gauge_yellow, gauge_red, gauge_empty;
    // Sparkline gradient (8 levels)
    uint16_t spark[8];
    // Status dots
    uint16_t dot_ok, dot_warn, dot_err;
    // Process table
    uint16_t proc_head, proc_name, proc_num, proc_status[3];
    // Log kinds
    uint16_t log_time, log_info, log_success, log_warn, log_error;
    // Network
    uint16_t net_up, net_down, net_ping;
    // Bar
    uint16_t bar_brand, bar_hint, bar_fps, bar_theme;
};

static Styles g_sty{};
static int    g_theme = 0;

static void build_styles(StylePool& pool) {
    const auto& t = kThemes_def[g_theme];
    auto& s = g_sty;

    s.border     = pool.intern(Style{}.with_fg(Color::rgb(t.border_r, t.border_g, t.border_b)));
    s.corner     = pool.intern(Style{}.with_fg(Color::rgb(t.border_r, t.border_g, t.border_b)));
    s.title      = pool.intern(Style{}.with_bold().with_fg(Color::rgb(t.title_r, t.title_g, t.title_b)));
    s.label      = pool.intern(Style{}.with_fg(Color::rgb(120, 120, 140)));
    s.value      = pool.intern(Style{}.with_bold().with_fg(Color::rgb(220, 220, 230)));
    s.muted      = pool.intern(Style{}.with_fg(Color::rgb(70, 70, 85)));
    s.bar_bg     = pool.intern(Style{}.with_fg(Color::rgb(40, 40, 50)).with_bg(Color::rgb(15, 15, 20)));

    s.gauge_green  = pool.intern(Style{}.with_fg(Color::rgb(80, 220, 120)));
    s.gauge_yellow = pool.intern(Style{}.with_fg(Color::rgb(240, 200, 60)));
    s.gauge_red    = pool.intern(Style{}.with_fg(Color::rgb(240, 80, 80)));
    s.gauge_empty  = pool.intern(Style{}.with_fg(Color::rgb(35, 35, 45)));

    // Sparkline: accent color at 8 brightness levels
    for (int i = 0; i < 8; ++i) {
        float f = (static_cast<float>(i) + 1.f) / 8.f;
        auto r = static_cast<uint8_t>(t.accent_r * f);
        auto g = static_cast<uint8_t>(t.accent_g * f);
        auto b = static_cast<uint8_t>(t.accent_b * f);
        s.spark[i] = pool.intern(Style{}.with_fg(Color::rgb(r, g, b)));
    }

    s.dot_ok   = pool.intern(Style{}.with_fg(Color::rgb(80, 220, 120)));
    s.dot_warn = pool.intern(Style{}.with_fg(Color::rgb(240, 200, 60)));
    s.dot_err  = pool.intern(Style{}.with_fg(Color::rgb(240, 80, 80)));

    s.proc_head      = pool.intern(Style{}.with_bold().with_fg(Color::rgb(140, 140, 160)));
    s.proc_name      = pool.intern(Style{}.with_fg(Color::rgb(180, 180, 200)));
    s.proc_num       = pool.intern(Style{}.with_fg(Color::rgb(160, 160, 180)));
    s.proc_status[0] = pool.intern(Style{}.with_fg(Color::rgb(80, 220, 120)));
    s.proc_status[1] = pool.intern(Style{}.with_fg(Color::rgb(240, 200, 60)));
    s.proc_status[2] = pool.intern(Style{}.with_fg(Color::rgb(90, 90, 110)));

    s.log_time    = pool.intern(Style{}.with_fg(Color::rgb(80, 80, 100)));
    s.log_info    = pool.intern(Style{}.with_fg(Color::rgb(160, 160, 180)));
    s.log_success = pool.intern(Style{}.with_fg(Color::rgb(80, 220, 120)));
    s.log_warn    = pool.intern(Style{}.with_fg(Color::rgb(240, 200, 60)));
    s.log_error   = pool.intern(Style{}.with_fg(Color::rgb(240, 80, 80)));

    s.net_up   = pool.intern(Style{}.with_fg(Color::rgb(80, 200, 255)));
    s.net_down = pool.intern(Style{}.with_fg(Color::rgb(160, 120, 255)));
    s.net_ping = pool.intern(Style{}.with_fg(Color::rgb(180, 180, 200)));

    s.bar_brand = pool.intern(Style{}.with_bold().with_fg(Color::rgb(t.accent_r, t.accent_g, t.accent_b)).with_bg(Color::rgb(15, 15, 20)));
    s.bar_hint  = pool.intern(Style{}.with_fg(Color::rgb(70, 70, 85)).with_bg(Color::rgb(15, 15, 20)));
    s.bar_fps   = pool.intern(Style{}.with_fg(Color::rgb(100, 100, 120)).with_bg(Color::rgb(15, 15, 20)));
    s.bar_theme = pool.intern(Style{}.with_fg(Color::rgb(t.title_r, t.title_g, t.title_b)).with_bg(Color::rgb(15, 15, 20)));
}

// ── Drawing primitives ───────────────────────────────────────────────────────

static int wstr(Canvas& c, int x, int y, const char* s, uint16_t sid) {
    int len = static_cast<int>(std::strlen(s));
    c.write_text(x, y, {s, static_cast<std::size_t>(len)}, sid);
    return x + len;
}

static int wstr(Canvas& c, int x, int y, std::string_view s, uint16_t sid) {
    c.write_text(x, y, s, sid);
    return x + static_cast<int>(s.size());
}

// Draw box border with rounded corners
static void draw_box(Canvas& c, int x0, int y0, int x1, int y1, uint16_t sid) {
    // Corners
    c.set(x0, y0, U'╭', sid);
    c.set(x1, y0, U'╮', sid);
    c.set(x0, y1, U'╰', sid);
    c.set(x1, y1, U'╯', sid);
    // Horizontal
    for (int x = x0 + 1; x < x1; ++x) {
        c.set(x, y0, U'─', sid);
        c.set(x, y1, U'─', sid);
    }
    // Vertical
    for (int y = y0 + 1; y < y1; ++y) {
        c.set(x0, y, U'│', sid);
        c.set(x1, y, U'│', sid);
    }
}

// Draw panel with title
static void draw_panel(Canvas& c, int x0, int y0, int x1, int y1,
                        const char* title) {
    draw_box(c, x0, y0, x1, y1, g_sty.border);
    // Title in the top border
    int tx = x0 + 2;
    c.set(tx++, y0, U' ', g_sty.border);
    tx = wstr(c, tx, y0, title, g_sty.title);
    c.set(tx, y0, U' ', g_sty.border);
}

// Progress bar with label
static void draw_gauge(Canvas& c, int x, int y, int width,
                        const char* label, float pct) {
    pct = std::clamp(pct, 0.f, 100.f);
    int lx = wstr(c, x, y, label, g_sty.label);
    lx = wstr(c, lx, y, " ", g_sty.muted);

    int bar_w = width - (lx - x) - 6; // leave room for " XX%"
    if (bar_w < 4) bar_w = 4;
    int filled = static_cast<int>(pct / 100.f * static_cast<float>(bar_w));

    uint16_t fill_sty = pct < 50.f ? g_sty.gauge_green
                      : pct < 75.f ? g_sty.gauge_yellow
                      :              g_sty.gauge_red;

    for (int i = 0; i < bar_w; ++i) {
        if (i < filled)
            c.set(lx + i, y, U'█', fill_sty);
        else
            c.set(lx + i, y, U'░', g_sty.gauge_empty);
    }

    char buf[8];
    std::snprintf(buf, sizeof buf, " %2.0f%%", pct);
    wstr(c, lx + bar_w, y, buf, fill_sty);
}

// Sparkline
static void draw_sparkline(Canvas& c, int x, int y, int width,
                            const std::deque<float>& data) {
    static constexpr char32_t kBlocks[] = {
        U'▁', U'▂', U'▃', U'▄', U'▅', U'▆', U'▇', U'█'
    };

    int start = std::max(0, static_cast<int>(data.size()) - width);
    int dx = 0;
    for (int i = start; i < static_cast<int>(data.size()) && dx < width; ++i, ++dx) {
        int level = std::clamp(static_cast<int>(data[static_cast<std::size_t>(i)] * 7.99f), 0, 7);
        c.set(x + dx, y, kBlocks[level], g_sty.spark[level]);
    }
    // Fill remaining with baseline
    for (; dx < width; ++dx) {
        c.set(x + dx, y, U'▁', g_sty.spark[0]);
    }
}

// Status dot
static uint16_t dot_style(int kind) {
    if (kind == 0) return g_sty.dot_ok;
    if (kind == 1) return g_sty.dot_warn;
    return g_sty.dot_err;
}

static const char* dot_label(int kind) {
    if (kind == 0) return "healthy";
    if (kind == 1) return "degraded";
    return "down";
}

// ── Panel painters ───────────────────────────────────────────────────────────

static void paint_system(Canvas& c, int x0, int y0, int x1, int y1) {
    draw_panel(c, x0, y0, x1, y1, "System");
    int w = x1 - x0 - 3; // inner width
    int ix = x0 + 2;

    for (int i = 0; i < 4 && y0 + 2 + i < y1; ++i) {
        draw_gauge(c, ix, y0 + 2 + i, w, g_metrics[i].label, g_metrics[i].value);
    }

    // Sparkline for CPU below the gauges
    if (y0 + 7 < y1 && !g_metrics[0].history.empty()) {
        wstr(c, ix, y0 + 7, "cpu", g_sty.muted);
        draw_sparkline(c, ix + 4, y0 + 7, w - 4, g_metrics[0].history);
    }
}

static void paint_network(Canvas& c, int x0, int y0, int x1, int y1) {
    draw_panel(c, x0, y0, x1, y1, "Network");
    int ix = x0 + 2;
    int iy = y0 + 2;

    char buf[32];

    // Upload
    int ux = wstr(c, ix, iy, "\xe2\x86\x91 ", g_sty.net_up); // ↑
    std::snprintf(buf, sizeof buf, "%.1f MB/s", g_upload);
    wstr(c, ux, iy, buf, g_sty.net_up);

    // Download
    int dx = wstr(c, ix, iy + 1, "\xe2\x86\x93 ", g_sty.net_down); // ↓
    std::snprintf(buf, sizeof buf, "%.1f MB/s", g_download);
    wstr(c, dx, iy + 1, buf, g_sty.net_down);

    // Latency
    int px = wstr(c, ix, iy + 2, "ping ", g_sty.label);
    std::snprintf(buf, sizeof buf, "%dms", g_latency);
    uint16_t ping_sty = g_latency < 20 ? g_sty.dot_ok
                       : g_latency < 40 ? g_sty.dot_warn
                       : g_sty.dot_err;
    wstr(c, px, iy + 2, buf, ping_sty);

    // Network sparkline
    if (iy + 4 < y1 && !g_metrics[3].history.empty()) {
        wstr(c, ix, iy + 4, "net", g_sty.muted);
        draw_sparkline(c, ix + 4, iy + 4, x1 - x0 - 7, g_metrics[3].history);
    }
}

static void paint_processes(Canvas& c, int x0, int y0, int x1, int y1) {
    draw_panel(c, x0, y0, x1, y1, "Processes");
    int ix = x0 + 2;
    int w  = x1 - x0 - 3;

    // Header
    int hy = y0 + 1;
    if (hy >= y1) return;
    int hx = ix;
    hx = wstr(c, hx, hy, "PID   ", g_sty.proc_head);
    hx = wstr(c, hx, hy, "NAME          ", g_sty.proc_head);
    hx = wstr(c, hx, hy, "CPU     ", g_sty.proc_head);
    hx = wstr(c, hx, hy, "MEM     ", g_sty.proc_head);
    wstr(c, hx, hy, "STATUS", g_sty.proc_head);

    // Separator line
    if (hy + 1 < y1) {
        for (int x = ix; x < ix + w && x < x1; ++x)
            c.set(x, hy + 1, U'─', g_sty.muted);
    }

    // Rows
    for (int i = 0; i < kProcCount && hy + 2 + i < y1; ++i) {
        const auto& p = g_procs[i];
        int ry = hy + 2 + i;
        int rx = ix;

        char pid_buf[8], cpu_buf[10], mem_buf[10];
        std::snprintf(pid_buf, sizeof pid_buf, "%-6d", p.pid);
        std::snprintf(cpu_buf, sizeof cpu_buf, "%5.1f%%  ", p.cpu);
        std::snprintf(mem_buf, sizeof mem_buf, "%5.1f%%  ", p.mem);

        char name_buf[16];
        std::snprintf(name_buf, sizeof name_buf, "%-14s", p.name);

        rx = wstr(c, rx, ry, pid_buf,  g_sty.proc_num);
        rx = wstr(c, rx, ry, name_buf, g_sty.proc_name);
        rx = wstr(c, rx, ry, cpu_buf,  g_sty.proc_num);
        rx = wstr(c, rx, ry, mem_buf,  g_sty.proc_num);
        rx = wstr(c, rx, ry, "\xe2\x97\x8f ", dot_style(p.status_kind)); // ●
        wstr(c, rx, ry, p.status, g_sty.proc_status[p.status_kind]);
    }
}

static void paint_events(Canvas& c, int x0, int y0, int x1, int y1) {
    draw_panel(c, x0, y0, x1, y1, "Events");
    int ix = x0 + 2;

    // Show visible log entries, cycling through based on time
    int visible = y1 - y0 - 2;
    int offset  = static_cast<int>(g_time * 0.2f) % kLogCount;

    for (int i = 0; i < visible && i < kLogCount; ++i) {
        int idx = (offset + i) % kLogCount;
        int ry  = y0 + 1 + i;
        if (ry >= y1) break;

        const auto& e = g_log[idx];
        int rx = wstr(c, ix, ry, e.time, g_sty.log_time);
        rx = wstr(c, rx, ry, "  ", g_sty.muted);

        static constexpr uint16_t Styles::* kLogStyles[] = {
            &Styles::log_info, &Styles::log_success,
            &Styles::log_warn, &Styles::log_error,
        };
        uint16_t sty = g_sty.*kLogStyles[e.kind];

        // Truncate text to fit panel
        int avail = x1 - rx - 1;
        if (avail > 0) {
            auto text = std::string_view{e.text};
            if (static_cast<int>(text.size()) > avail)
                text = text.substr(0, static_cast<std::size_t>(avail));
            wstr(c, rx, ry, text, sty);
        }
    }
}

static void paint_services(Canvas& c, int x0, int y0, int x1, int y1) {
    draw_panel(c, x0, y0, x1, y1, "Services");
    int ix = x0 + 2;

    for (int i = 0; i < kSvcCount && y0 + 1 + i < y1; ++i) {
        const auto& svc = g_services[i];
        int ry = y0 + 1 + i;
        int rx = ix;

        rx = wstr(c, rx, ry, svc.name, g_sty.proc_name);
        rx = wstr(c, rx, ry, " \xe2\x97\x8f ", dot_style(svc.kind)); // ●
        rx = wstr(c, rx, ry, dot_label(svc.kind), dot_style(svc.kind));

        char lat[12];
        std::snprintf(lat, sizeof lat, "  %3dms", svc.latency);
        wstr(c, rx, ry, lat, g_sty.proc_num);
    }
}

static void paint_bar(Canvas& c, int W, int H, double fps) {
    int y = H - 1;
    for (int x = 0; x < W; ++x)
        c.set(x, y, U' ', g_sty.bar_bg);

    int x = 0;
    x = wstr(c, x, y, " maya ", g_sty.bar_brand);
    x = wstr(c, x, y, " \xe2\x94\x82 ", g_sty.bar_hint); // │
    x = wstr(c, x, y, kThemes_def[g_theme].name, g_sty.bar_theme);

    char rbuf[48];
    std::snprintf(rbuf, sizeof rbuf, " [t]heme  [q]uit  %.0f fps ", fps);
    int rlen = static_cast<int>(std::strlen(rbuf));
    if (W - rlen > x)
        wstr(c, W - rlen, y, rbuf, g_sty.bar_hint);
}

// ── Main paint ───────────────────────────────────────────────────────────────

static void paint(Canvas& canvas, int W, int H, double fps) {
    if (W < 40 || H < 12) {
        canvas.write_text(0, 0, "Terminal too small", 0);
        return;
    }

    const int bar_h    = 1;
    const int content_h = H - bar_h;

    // Layout: top row (system + network), middle (processes), bottom (events + services)
    const int mid_x = W / 2;

    // Vertical distribution: top ~40%, middle ~30%, bottom ~30%
    int top_h    = std::max(9, content_h * 4 / 10);
    int middle_h = std::max(kProcCount + 4, content_h * 3 / 10);
    int bottom_h = content_h - top_h - middle_h;
    if (bottom_h < 4) { bottom_h = 4; top_h = content_h - middle_h - bottom_h; }

    int y0 = 0;
    int y1 = top_h;
    int y2 = y1 + middle_h;
    int y3 = content_h;

    // Top row: System (left) + Network (right)
    paint_system (canvas, 0,     y0, mid_x - 1, y1 - 1);
    paint_network(canvas, mid_x, y0, W - 1,     y1 - 1);

    // Middle: Processes (full width)
    paint_processes(canvas, 0, y1, W - 1, y2 - 1);

    // Bottom row: Events (left) + Services (right)
    paint_events  (canvas, 0,     y2, mid_x - 1, y3 - 1);
    paint_services(canvas, mid_x, y2, W - 1,     y3 - 1);

    // Status bar
    paint_bar(canvas, W, H, fps);
}

// ── main ─────────────────────────────────────────────────────────────────────

int main() {
    using Clock = std::chrono::steady_clock;
    auto  last  = Clock::now();
    int   frames = 0;
    double fps   = 30.0;
    auto   fps_clock = Clock::now();

    // Warm up sparkline history
    for (float t = 0.f; t < 30.f; t += 0.5f) {
        g_time = t;
        tick(0.5f);
        for (auto& m : g_metrics) {
            m.history.push_back(m.value / 100.f);
            while (static_cast<int>(m.history.size()) > kSparkLen)
                m.history.pop_front();
        }
    }

    auto result = canvas_run(
        CanvasConfig{.fps = 30, .title = "dashboard \xc2\xb7 maya"}, // ·

        [](StylePool& pool, int, int) {
            build_styles(pool);
        },

        [](const Event& ev) -> bool {
            if (key(ev, 'q') || key(ev, SpecialKey::Escape)) return false;
            on(ev, 't', [] { g_theme = (g_theme + 1) % kThemes; });
            on(ev, 'T', [] { g_theme = (g_theme + 1) % kThemes; });
            return true;
        },

        [&](Canvas& canvas, int W, int H) {
            const auto now = Clock::now();
            float dt = std::min(
                std::chrono::duration<float>(now - last).count(), 0.1f);
            last = now;

            ++frames;
            double el = std::chrono::duration<double>(now - fps_clock).count();
            if (el >= 0.5) { fps = frames / el; frames = 0; fps_clock = now; }

            tick(dt);
            paint(canvas, W, H, fps);
        }
    );

    if (!result) {
        std::fprintf(stderr, "maya: %s\n", result.error().message.c_str());
        return 1;
    }
}
