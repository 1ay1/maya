// maya — live system monitoring dashboard (DSL)
//
// Dense, visually rich dashboard with sparklines, gauge bars, per-core stats,
// network/disk panels, process table with inline CPU bars, and live stats.
// All data is simulated with multi-harmonic oscillators for realism.
//
// Built entirely with maya::dsl — compile-time UI tree with dyn() for live data.
//
// Keys: q/ESC = quit   t = cycle theme

#include <maya/maya.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

using namespace maya;
using namespace maya::dsl;

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
static int   g_frame = 0;

static constexpr int kCores = 8;
static constexpr int kHistMax = 120;

static float g_cpu[kCores];
static float g_cpu_avg;
static std::deque<float> g_cpu_hist;
static std::deque<float> g_core_hist[kCores];

static float g_mem_used, g_mem_buff, g_mem_cache;
static constexpr float kMemTotal = 16.0f;
static std::deque<float> g_mem_hist;

static float g_net_rx, g_net_tx;
static std::deque<float> g_net_rx_hist, g_net_tx_hist;
static int g_tcp = 842, g_udp = 156;
static float g_net_rx_total = 284.7f, g_net_tx_total = 42.3f;
static int g_net_err = 0, g_net_drop = 0;

static float g_disk_r, g_disk_w;
static std::deque<float> g_disk_r_hist, g_disk_w_hist;
static float g_iops_r, g_iops_w;
static float g_disk_util = 34.f;
static float g_disk_lat_r = 1.2f, g_disk_lat_w = 2.8f;

static float g_load[3];
static int   g_tasks = 312, g_threads = 1847;
static float g_uptime = 7 * 86400.f + 14 * 3600.f + 32 * 60.f;

static const char* kSpinner[] = {
    "\xe2\xa0\x8b","\xe2\xa0\x99","\xe2\xa0\xb9","\xe2\xa0\xb8",
    "\xe2\xa0\xbc","\xe2\xa0\xb4","\xe2\xa0\xa6","\xe2\xa0\xa7",
    "\xe2\xa0\x87","\xe2\xa0\x8f",
};

struct Proc {
    const char* user; const char* name; int pid;
    float base_cpu, base_mem;
    const char* virt; const char* res;
    int status;
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
};
static constexpr int kProcCount = 14;

// ── Simulation ───────────────────────────────────────────────────────────────

static float wave(float t, float base, float a1, float f1,
                  float a2 = 0, float f2 = 0, float a3 = 0, float f3 = 0) {
    return base + a1 * fsin(t * f1) + a2 * fsin(t * f2) + a3 * fsin(t * f3);
}

static void push_hist(std::deque<float>& h, float v, int max_len) {
    h.push_back(v);
    while (static_cast<int>(h.size()) > max_len) h.pop_front();
}

static void tick(float dt) {
    g_time += dt;
    g_uptime += dt;
    float t = g_time;

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

    g_mem_used  = std::clamp(wave(t, 8.2f, 1.2f, 0.3f, 0.4f, 1.1f), 5.f, 12.f);
    g_mem_buff  = std::clamp(wave(t, 2.1f, 0.5f, 0.2f, 0.2f, 0.7f), 0.8f, 3.5f);
    g_mem_cache = std::clamp(wave(t, 3.4f, 0.8f, 0.15f, 0.3f, 0.5f), 1.5f, 5.f);

    g_net_tx = std::max(0.1f, wave(t, 12.4f, 6.f, 1.5f, 3.f, 3.7f, 1.5f, 0.4f));
    g_net_rx = std::max(0.1f, wave(t, 48.2f, 18.f, 0.9f, 8.f, 2.3f, 4.f, 0.5f));
    g_tcp = std::clamp(static_cast<int>(wave(t, 842, 80, 0.3f, 30, 1.2f)), 600, 1100);
    g_udp = std::clamp(static_cast<int>(wave(t, 156, 40, 0.5f, 15, 1.8f)), 80, 250);
    g_net_rx_total += g_net_rx * dt;
    g_net_tx_total += g_net_tx * dt;
    g_net_err  = std::clamp(static_cast<int>(wave(t, 2, 3, 0.1f, 1, 0.7f)), 0, 12);
    g_net_drop = std::clamp(static_cast<int>(wave(t, 1, 2, 0.15f, 1, 0.5f)), 0, 8);

    g_disk_r = std::max(0.f, wave(t, 124.f, 50.f, 1.2f, 20.f, 2.8f, 10.f, 0.3f));
    g_disk_w = std::max(0.f, wave(t, 45.f, 20.f, 0.8f, 10.f, 2.1f, 5.f, 0.5f));
    g_iops_r = std::max(0.f, wave(t, 28.4f, 12.f, 1.5f, 4.f, 3.2f));
    g_iops_w = std::max(0.f, wave(t, 12.1f, 6.f, 0.9f, 2.f, 2.7f));
    g_disk_util  = std::clamp(wave(t, 34.f, 20.f, 0.6f, 8.f, 1.4f), 2.f, 98.f);
    g_disk_lat_r = std::max(0.1f, wave(t, 1.2f, 0.8f, 1.0f, 0.3f, 2.5f));
    g_disk_lat_w = std::max(0.1f, wave(t, 2.8f, 1.5f, 0.7f, 0.5f, 1.8f));

    g_load[0] = std::max(0.1f, wave(t, 4.82f, 1.5f, 0.15f, 0.5f, 0.4f));
    g_load[1] = std::max(0.1f, wave(t, 3.21f, 1.0f, 0.08f, 0.3f, 0.2f));
    g_load[2] = std::max(0.1f, wave(t, 2.15f, 0.6f, 0.05f, 0.2f, 0.1f));

    g_tasks   = std::clamp(static_cast<int>(wave(t, 312, 15, 0.2f, 5, 0.8f)), 280, 350);
    g_threads = std::clamp(static_cast<int>(wave(t, 1847, 80, 0.3f, 30, 0.9f)), 1700, 2000);

    static int htick = 0;
    if (++htick >= 5) {
        htick = 0;
        push_hist(g_cpu_hist, g_cpu_avg / 100.f, kHistMax);
        for (int i = 0; i < kCores; ++i)
            push_hist(g_core_hist[i], g_cpu[i] / 100.f, kHistMax);
        push_hist(g_mem_hist, (g_mem_used + g_mem_buff + g_mem_cache) / kMemTotal, kHistMax);
        push_hist(g_net_rx_hist, std::min(g_net_rx / 80.f, 1.f), kHistMax);
        push_hist(g_net_tx_hist, std::min(g_net_tx / 30.f, 1.f), kHistMax);
        push_hist(g_disk_r_hist, std::min(g_disk_r / 200.f, 1.f), kHistMax);
        push_hist(g_disk_w_hist, std::min(g_disk_w / 100.f, 1.f), kHistMax);
    }
}

// ── Themes ───────────────────────────────────────────────────────────────────

static constexpr int kThemeCount = 3;
struct ThemeDef { const char* name; uint8_t ar,ag,ab, br,bg_,bb, cr,cg,cb; };
static constexpr ThemeDef kThemes[kThemeCount] = {
    {"midnight",  80,140,255,  55,65,90,   120,160,255},
    {"forest",    60,220,100,  50,80,55,   100,240,140},
    {"ember",     255,120,60,  90,60,50,   255,180,80},
};
static int g_theme = 0;

// ── Color helpers ────────────────────────────────────────────────────────────

static Color theme_accent() {
    auto& t = kThemes[g_theme];
    return Color::rgb(t.cr, t.cg, t.cb);
}

static Color theme_border() {
    auto& t = kThemes[g_theme];
    return Color::rgb(t.br, t.bg_, t.bb);
}

static Color theme_brand() {
    auto& t = kThemes[g_theme];
    return Color::rgb(t.ar, t.ag, t.ab);
}

static Color gauge_color(float pct) {
    float f = std::clamp(pct, 0.f, 100.f) / 100.f;
    uint8_t r, g, b;
    if (f < 0.5f) {
        float t = f * 2.f;
        r = static_cast<uint8_t>(60 + 180 * t);
        g = static_cast<uint8_t>(210 - 30 * t);
        b = static_cast<uint8_t>(100 - 60 * t);
    } else {
        float t = (f - 0.5f) * 2.f;
        r = 240;
        g = static_cast<uint8_t>(180 - 140 * t);
        b = static_cast<uint8_t>(40 - 30 * t);
    }
    return Color::rgb(r, g, b);
}

static Color load_color(float v) {
    if (v < 4.f) return Color::rgb(80, 220, 120);
    if (v < 6.f) return Color::rgb(240, 200, 60);
    return Color::rgb(240, 80, 80);
}

static const uint8_t kCoreColor[kCores][3] = {
    {80,200,255}, {60,210,200}, {80,220,140}, {120,210,80},
    {180,200,60}, {220,180,60}, {240,140,60}, {255,100,80},
};

// ── Text rendering helpers ───────────────────────────────────────────────────

static constexpr const char* kBlk[] = {
    "\xe2\x96\x81","\xe2\x96\x82","\xe2\x96\x83","\xe2\x96\x84",
    "\xe2\x96\x85","\xe2\x96\x86","\xe2\x96\x87","\xe2\x96\x88",
};

static std::string make_sparkline(const std::deque<float>& data, int w) {
    std::string s;
    int start = std::max(0, static_cast<int>(data.size()) - w);
    int dx = 0;
    for (int i = start; i < static_cast<int>(data.size()) && dx < w; ++i, ++dx) {
        int lv = std::clamp(static_cast<int>(data[static_cast<std::size_t>(i)] * 7.99f), 0, 7);
        s += kBlk[lv];
    }
    for (; dx < w; ++dx) s += kBlk[0];
    return s;
}

static std::string make_gauge(int w, float pct) {
    pct = std::clamp(pct, 0.f, 100.f);
    int filled = static_cast<int>(pct / 100.f * static_cast<float>(w));
    std::string s;
    for (int i = 0; i < w; ++i)
        s += (i < filled) ? "\xe2\x96\x88" : "\xe2\x96\x91";
    return s;
}

static std::string make_bar(int w, float value, float scale) {
    int filled = std::clamp(static_cast<int>(value / scale * static_cast<float>(w)), 0, w);
    std::string s;
    for (int i = 0; i < w; ++i)
        s += (i < filled) ? "\xe2\x94\x81" : "\xe2\x94\x84";
    return s;
}

static const char* spin() {
    return kSpinner[(g_frame / 3) % 10];
}

// ── Reusable styles ──────────────────────────────────────────────────────────

static const Style sLabel = Style{}.with_fg(Color::rgb(110, 110, 130));
static const Style sValue = Style{}.with_bold().with_fg(Color::rgb(210, 210, 225));
static const Style sMuted = Style{}.with_fg(Color::rgb(55, 55, 70));
static const Style sDim   = Style{}.with_fg(Color::rgb(80, 80, 100));
static const Style sNum   = Style{}.with_fg(Color::rgb(150, 150, 170));
static const Style sHead  = Style{}.with_bold().with_fg(Color::rgb(130, 130, 155));
static const Style sName  = Style{}.with_fg(Color::rgb(180, 180, 200));
static const Style sUser  = Style{}.with_fg(Color::rgb(120, 140, 170));
static const Style sGreen = Style{}.with_bold().with_fg(Color::rgb(80, 220, 120));
static const Style sRx    = Style{}.with_fg(Color::rgb(80, 200, 255));
static const Style sTx    = Style{}.with_fg(Color::rgb(160, 120, 255));
static const Style sRead  = Style{}.with_fg(Color::rgb(100, 220, 160));
static const Style sWrite = Style{}.with_fg(Color::rgb(255, 160, 80));
static const Style sMem   = Style{}.with_fg(Color::rgb(180, 130, 255));

// ── Panel builders ───────────────────────────────────────────────────────────

static Element build_header() {
    char buf[128];
    int days = static_cast<int>(g_uptime / 86400.f);
    int hrs  = static_cast<int>(std::fmod(g_uptime, 86400.f) / 3600.f);
    int mins = static_cast<int>(std::fmod(g_uptime, 3600.f) / 60.f);

    auto hdr_bg = Style{}.with_fg(Color::rgb(70,70,90)).with_bg(Color::rgb(18,18,28));
    auto hdr_host = Style{}.with_bold().with_fg(theme_accent()).with_bg(Color::rgb(18,18,28));
    auto hdr_up = Style{}.with_fg(Color::rgb(110,110,140)).with_bg(Color::rgb(18,18,28));
    auto hdr_spin = Style{}.with_fg(theme_brand()).with_bg(Color::rgb(18,18,28));

    std::snprintf(buf, sizeof buf, "up %dd %dh %dm", days, hrs, mins);
    std::string up_str = buf;

    char load_buf[48];
    std::snprintf(load_buf, sizeof load_buf, "%.2f %.2f %.2f",
                  static_cast<double>(g_load[0]),
                  static_cast<double>(g_load[1]),
                  static_cast<double>(g_load[2]));

    char right[48];
    float mem_pct = (g_mem_used + g_mem_buff + g_mem_cache) / kMemTotal * 100.f;
    std::snprintf(right, sizeof right, "cpu %2.0f%%  mem %2.0f%%",
                  static_cast<double>(g_cpu_avg), static_cast<double>(mem_pct));

    return hstack()(
        text(std::string(spin()) + " ", hdr_spin),
        text("prod-srv-01", hdr_host),
        text(" \xe2\x94\x82 ", hdr_bg),
        text(up_str, hdr_up),
        text(" \xe2\x94\x82 ", hdr_bg),
        text("load ", hdr_up),
        text(load_buf, Style{}.with_fg(load_color(g_load[0])).with_bg(Color::rgb(18,18,28))),
        text(" \xe2\x94\x82 ", hdr_bg),
        text(std::to_string(g_tasks) + " tasks  " + std::to_string(g_threads) + " thr", hdr_up),
        spacer() | Style{}.with_bg(Color::rgb(18,18,28)),
        text(right, Style{}.with_fg(gauge_color(g_cpu_avg)).with_bg(Color::rgb(18,18,28))),
        text("  ", hdr_bg)
    );
}

static Element build_cpu_panel() {
    std::vector<Element> rows;

    // CPU avg sparkline
    auto spark = make_sparkline(g_cpu_hist, 60);
    rows.push_back(hstack()(
        text("avg ", sLabel),
        text(spark, Style{}.with_fg(theme_brand()))
    ));

    char buf[32];
    std::snprintf(buf, sizeof buf, " %4.1f%%", static_cast<double>(g_cpu_avg));
    int gi_avg = std::clamp(static_cast<int>(g_cpu_avg / 100.f * 11.f), 0, 11);
    rows.push_back(hstack()(
        text(buf, Style{}.with_bold().with_fg(gauge_color(g_cpu_avg))),
        text("  \xe2\x96\xb2 ", sDim),
        text(([&]{
            std::snprintf(buf, sizeof buf, "%.0f%%", static_cast<double>(*std::max_element(g_cpu, g_cpu+kCores)));
            return std::string(buf);
        })(), Style{}.with_fg(Color::rgb(240,80,80))),
        text("  \xe2\x96\xbc ", sDim),
        text(([&]{
            std::snprintf(buf, sizeof buf, "%.0f%%", static_cast<double>(*std::min_element(g_cpu, g_cpu+kCores)));
            return std::string(buf);
        })(), sGreen)
    ));

    rows.push_back(text(""));

    // Per-core sparklines
    for (int i = 0; i < kCores; ++i) {
        char label[8];
        std::snprintf(label, sizeof label, "c%d ", i);
        auto core_spark = make_sparkline(g_core_hist[i], 40);
        char pct[8];
        std::snprintf(pct, sizeof pct, " %3.0f%%", static_cast<double>(g_cpu[i]));
        auto cc = Color::rgb(kCoreColor[i][0], kCoreColor[i][1], kCoreColor[i][2]);
        rows.push_back(hstack()(
            text(label, sLabel),
            text(core_spark, Style{}.with_fg(cc)),
            text(pct, Style{}.with_fg(gauge_color(g_cpu[i])))
        ));
    }

    return vstack().border(BorderStyle::Round).border_color(theme_border())
        .border_text(std::string(spin()) + " CPU", BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static Element build_mem_panel() {
    float total_used = g_mem_used + g_mem_buff + g_mem_cache;
    float pct = total_used / kMemTotal * 100.f;
    char buf[32];

    std::vector<Element> rows;

    // Big percentage + gauge
    std::snprintf(buf, sizeof buf, "%.0f%% used", static_cast<double>(pct));
    rows.push_back(text(buf, Style{}.with_bold().with_fg(gauge_color(pct))));
    rows.push_back(text(make_gauge(28, pct), Style{}.with_fg(gauge_color(pct))));
    rows.push_back(text(""));

    // Breakdown
    std::snprintf(buf, sizeof buf, "used  %5.1fG", static_cast<double>(g_mem_used));
    rows.push_back(text(buf, sValue));
    std::snprintf(buf, sizeof buf, "buff  %5.1fG", static_cast<double>(g_mem_buff));
    rows.push_back(text(buf, sMem));
    std::snprintf(buf, sizeof buf, "cache %5.1fG", static_cast<double>(g_mem_cache));
    rows.push_back(text(buf, sTx));
    std::snprintf(buf, sizeof buf, "free  %5.1fG", static_cast<double>(kMemTotal - total_used));
    rows.push_back(text(buf, sMuted));
    std::snprintf(buf, sizeof buf, "total %5.1fG", static_cast<double>(kMemTotal));
    rows.push_back(text(buf, sLabel));

    rows.push_back(text(""));
    auto spark = make_sparkline(g_mem_hist, 28);
    rows.push_back(text(spark, sMem));

    return vstack().border(BorderStyle::Round).border_color(theme_border())
        .border_text(std::string(spin()) + " Memory", BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static Element build_net_panel() {
    char buf[64];
    std::vector<Element> rows;

    // Interface + speeds
    std::snprintf(buf, sizeof buf, "eth0  \xe2\x86\x91%5.1f MB/s  \xe2\x86\x93%5.1f MB/s",
                  static_cast<double>(g_net_tx), static_cast<double>(g_net_rx));
    rows.push_back(text(buf, sValue));

    // RX/TX sparklines
    auto rx_spark = make_sparkline(g_net_rx_hist, 40);
    auto tx_spark = make_sparkline(g_net_tx_hist, 40);
    rows.push_back(hstack()(text(" rx ", sMuted), text(rx_spark, sRx)));
    rows.push_back(hstack()(text(" tx ", sMuted), text(tx_spark, sTx)));

    // Stats
    std::snprintf(buf, sizeof buf, "tcp %-5d  udp %-4d  err %d  drop %d",
                  g_tcp, g_udp, g_net_err, g_net_drop);
    rows.push_back(text(buf, sLabel));

    std::snprintf(buf, sizeof buf, "rx %.1fG  tx %.1fG",
                  static_cast<double>(g_net_rx_total / 1024.f),
                  static_cast<double>(g_net_tx_total / 1024.f));
    rows.push_back(text(buf, sNum));

    return vstack().border(BorderStyle::Round).border_color(theme_border())
        .border_text(std::string(spin()) + " Network", BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static Element build_disk_panel() {
    char buf[64];
    std::vector<Element> rows;

    std::snprintf(buf, sizeof buf, "sda  R %5.0f MB/s  W %5.0f MB/s",
                  static_cast<double>(g_disk_r), static_cast<double>(g_disk_w));
    rows.push_back(text(buf, sValue));

    auto r_spark = make_sparkline(g_disk_r_hist, 40);
    auto w_spark = make_sparkline(g_disk_w_hist, 40);
    rows.push_back(hstack()(text("read ", sMuted), text(r_spark, sRead)));
    rows.push_back(hstack()(text("write", sMuted), text(w_spark, sWrite)));

    std::snprintf(buf, sizeof buf, "iops  %.1fk r/s  %.1fk w/s",
                  static_cast<double>(g_iops_r), static_cast<double>(g_iops_w));
    rows.push_back(text(buf, sLabel));

    std::snprintf(buf, sizeof buf, "lat r %.1fms  w %.1fms",
                  static_cast<double>(g_disk_lat_r), static_cast<double>(g_disk_lat_w));
    rows.push_back(text(buf, sNum));

    // Util gauge
    std::snprintf(buf, sizeof buf, "%3.0f%%", static_cast<double>(g_disk_util));
    rows.push_back(hstack()(
        text("util ", sLabel),
        text(make_gauge(20, g_disk_util), Style{}.with_fg(gauge_color(g_disk_util))),
        text(buf, Style{}.with_fg(gauge_color(g_disk_util)))
    ));

    return vstack().border(BorderStyle::Round).border_color(theme_border())
        .border_text(std::string(spin()) + " Disk I/O", BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static Element build_procs() {
    std::vector<Element> rows;

    // Header
    char hdr[96];
    std::snprintf(hdr, sizeof hdr, "%-7s %-8s %-14s %5s %5s %-6s %-6s  %s",
                  "PID", "USER", "NAME", "CPU%", "MEM%", "VIRT", "RES", "LOAD");
    rows.push_back(text(hdr, sHead));

    // Separator
    std::string sep_str(80, '\xe2');  // placeholder
    sep_str.clear();
    for (int i = 0; i < 80; ++i) sep_str += "\xe2\x94\x80";
    rows.push_back(text(sep_str, sMuted));

    const char* status_str[] = {"\xe2\x97\x8f", "\xe2\x97\x8b", "\xe2\x97\x8b"};
    static const Color dot_colors[] = {
        Color::rgb(80, 220, 120),
        Color::rgb(100, 100, 120),
        Color::rgb(200, 180, 60),
    };

    for (int i = 0; i < kProcCount; ++i) {
        const auto& p = g_procs[i];
        float cpu = std::max(0.1f, p.base_cpu + p.base_cpu * 0.3f * fsin(g_time * (1.f + i * 0.4f)));

        char pid_s[8], user_s[10], name_s[16], cpu_s[8], mem_s[8], virt_s[8], res_s[8];
        std::snprintf(pid_s,  sizeof pid_s,  "%-7d", p.pid);
        std::snprintf(user_s, sizeof user_s, "%-8s", p.user);
        std::snprintf(name_s, sizeof name_s, "%-14s", p.name);
        std::snprintf(cpu_s,  sizeof cpu_s,  "%5.1f", static_cast<double>(cpu));
        std::snprintf(mem_s,  sizeof mem_s,  "%5.1f", static_cast<double>(p.base_mem));
        std::snprintf(virt_s, sizeof virt_s, "%-6s", p.virt);
        std::snprintf(res_s,  sizeof res_s,  "%-6s", p.res);

        auto bar = make_bar(12, cpu, std::max(15.f, cpu * 2.5f));

        auto row_style = (i % 2 == 1) ? Style{}.with_bg(Color::rgb(18, 18, 26)) : Style{};

        rows.push_back(hstack().style(row_style)(
            text(pid_s, sNum),
            text(user_s, sUser),
            text(name_s, sName),
            text(cpu_s, Style{}.with_fg(gauge_color(cpu))),
            text(" ", sDim),
            text(mem_s, sNum),
            text(" ", sDim),
            text(virt_s, sNum),
            text(res_s, sNum),
            text(std::string(" ") + status_str[p.status] + " ", Style{}.with_fg(dot_colors[p.status])),
            text(bar, Style{}.with_fg(gauge_color(cpu)))
        ));
    }

    return vstack().border(BorderStyle::Round).border_color(theme_border())
        .border_text("Processes", BorderTextPos::Top)
        .padding(0, 1, 0, 1).grow()(std::move(rows));
}

static Element build_status_bar() {
    auto bar_bg = Style{}.with_fg(Color::rgb(40,40,50)).with_bg(Color::rgb(12,12,18));
    auto bar_brand = Style{}.with_bold().with_fg(theme_brand()).with_bg(Color::rgb(12,12,18));
    auto bar_hint = Style{}.with_fg(Color::rgb(65,65,80)).with_bg(Color::rgb(12,12,18));
    auto bar_theme = Style{}.with_fg(theme_accent()).with_bg(Color::rgb(12,12,18));

    auto spark = make_sparkline(g_cpu_hist, 20);

    return hstack()(
        text(" maya ", bar_brand),
        text(" \xe2\x94\x82 ", bar_hint),
        text(kThemes[g_theme].name, bar_theme),
        text(" \xe2\x94\x82 ", bar_hint),
        text(spark, bar_brand),
        spacer() | Style{}.with_bg(Color::rgb(12,12,18)),
        text(" [t]heme  [q]uit ", bar_hint)
    );
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main() {
    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();

    // Warm up history
    for (float t = 0.f; t < 40.f; t += 0.17f) {
        g_time = t;
        tick(0.17f);
    }

    run(
        {.title = "dashboard \xc2\xb7 maya"},

        [&](const Event& ev) {
            if (key(ev, 'q') || key(ev, SpecialKey::Escape)) return false;
            on(ev, 't', [] { g_theme = (g_theme + 1) % kThemeCount; });
            on(ev, 'T', [] { g_theme = (g_theme + 1) % kThemeCount; });
            return true;
        },

        [&](const Ctx&) {
            auto now = Clock::now();
            float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
            last = now;
            ++g_frame;
            tick(dt);

            // DSL tree: header, panels in 2x2 grid, process table, status bar
            return (v(
                dyn([] { return build_header(); }),
                dyn([] {
                    return (h(
                        dyn([] { return (v(
                            dyn([] { return build_cpu_panel(); }),
                            dyn([] { return build_net_panel(); })
                        ) | grow_<2>).build(); }),
                        dyn([] { return (v(
                            dyn([] { return build_mem_panel(); }),
                            dyn([] { return build_disk_panel(); })
                        ) | grow_<1>).build(); })
                    ) | grow_<1>).build();
                }),
                dyn([] { return build_procs(); }),
                dyn([] { return build_status_bar(); })
            )).build();
        }
    );
}
