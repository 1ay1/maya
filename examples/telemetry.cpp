// maya — TELEMETRY: Live System Telemetry Dashboard
//
// A gorgeous widget-based dashboard with live animated data:
//   - CPU/Memory/Disk/Network gauges with color thresholds
//   - Multi-series line charts with braille rendering
//   - Sparkline history strips for every metric
//   - Bar chart of process CPU usage
//   - Heatmap of hourly load distribution
//   - Live table of top processes
//   - Progress bars for long-running tasks
//
// Keys: q/Esc=quit  1-3=theme  space=pause  r=reset

#include <maya/maya.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using namespace maya;
using namespace maya::dsl;

// ── Themes ───────────────────────────────────────────────────────────────────

struct TelTheme {
    const char* name;
    Color accent, accent2, warm, cool, dim, bright, danger, ok;
    uint8_t bg_r, bg_g, bg_b;
};

static const TelTheme themes[] = {
    {"NEON",
     Color::rgb(0,220,255), Color::rgb(200,80,255), Color::rgb(255,100,60),
     Color::rgb(0,200,180), Color::rgb(60,65,80), Color::rgb(220,225,240),
     Color::rgb(255,60,80), Color::rgb(0,230,118),
     12, 14, 22},
    {"EMBER",
     Color::rgb(255,140,0), Color::rgb(255,60,80), Color::rgb(255,200,60),
     Color::rgb(200,120,255), Color::rgb(70,55,50), Color::rgb(240,220,200),
     Color::rgb(255,40,40), Color::rgb(120,220,80),
     18, 12, 10},
    {"MATRIX",
     Color::rgb(0,255,65), Color::rgb(0,200,120), Color::rgb(180,255,0),
     Color::rgb(0,180,255), Color::rgb(0,60,20), Color::rgb(180,255,180),
     Color::rgb(255,0,60), Color::rgb(0,255,136),
     5, 12, 5},
};

static int g_theme = 0;
static const TelTheme& thm() { return themes[g_theme]; }

static Style fg_t(Color c) { return Style{}.with_fg(c); }

// ── State ────────────────────────────────────────────────────────────────────

static std::mt19937 g_rng{std::random_device{}()};
static float g_time = 0.f;
static bool g_paused = false;
static int g_frame = 0;

// Metrics with history
struct Metric {
    const char* name;
    float value;
    float target;
    std::vector<float> history;
    float speed;
};

static Metric g_cpu     {"CPU",     0.35f, 0.40f, {}, 0.08f};
static Metric g_memory  {"MEM",     0.52f, 0.55f, {}, 0.04f};
static Metric g_disk_io {"DISK",    0.15f, 0.20f, {}, 0.12f};
static Metric g_network {"NET",     0.28f, 0.30f, {}, 0.10f};
static Metric g_gpu     {"GPU",     0.60f, 0.65f, {}, 0.06f};
static Metric g_swap    {"SWAP",    0.10f, 0.12f, {}, 0.03f};

static Metric* g_metrics[] = {&g_cpu, &g_memory, &g_disk_io, &g_network, &g_gpu, &g_swap};

// Process table
struct Process {
    const char* name;
    float cpu;
    float mem_mb;
    int pid;
    float cpu_target;
};

static Process g_procs[] = {
    {"chrome",       18.5f,  2400.f, 1234, 22.f},
    {"node",         12.3f,   850.f, 5678, 15.f},
    {"rust-analyzer", 8.7f,   620.f, 9012, 10.f},
    {"vscode",        6.2f,  1100.f, 3456, 8.f},
    {"docker",        4.8f,   540.f, 7890, 6.f},
    {"postgres",      3.1f,   380.f, 2345, 4.f},
    {"nginx",         1.4f,   120.f, 6789, 2.f},
    {"redis",         0.9f,    85.f, 4321, 1.5f},
};

// Tasks
struct Task {
    const char* name;
    float progress;
    float speed;
};

static Task g_tasks[] = {
    {"Building project...",     0.0f, 0.008f},
    {"Running test suite",      0.0f, 0.012f},
    {"Deploying to staging",    0.0f, 0.005f},
    {"Indexing codebase",       0.0f, 0.015f},
};

// Heatmap data (24h x 7 days)
static float g_heatmap[7][24] = {};

// Network throughput series
static std::vector<float> g_net_rx(60, 0.f);
static std::vector<float> g_net_tx(60, 0.f);

static float randf(float lo, float hi) {
    return std::uniform_real_distribution<float>(lo, hi)(g_rng);
}

// ── Tick ─────────────────────────────────────────────────────────────────────

static void tick(float dt) {
    if (g_paused) return;
    g_time += dt;
    g_frame++;

    // Animate metrics toward targets with jitter
    for (auto* m : g_metrics) {
        // Occasionally pick new targets
        if (g_frame % 60 == 0) {
            m->target = std::clamp(m->target + randf(-0.15f, 0.15f), 0.02f, 0.95f);
        }
        m->value += (m->target - m->value) * m->speed + randf(-0.01f, 0.01f);
        m->value = std::clamp(m->value, 0.0f, 1.0f);

        m->history.push_back(m->value);
        if (m->history.size() > 80) m->history.erase(m->history.begin());
    }

    // Animate processes
    for (auto& p : g_procs) {
        if (g_frame % 45 == 0) {
            p.cpu_target = std::clamp(p.cpu_target + randf(-5.f, 5.f), 0.1f, 40.f);
        }
        p.cpu += (p.cpu_target - p.cpu) * 0.05f + randf(-0.3f, 0.3f);
        p.cpu = std::max(0.1f, p.cpu);
        p.mem_mb += randf(-10.f, 10.f);
        p.mem_mb = std::max(20.f, p.mem_mb);
    }

    // Animate tasks
    for (auto& t : g_tasks) {
        if (t.progress < 1.0f) {
            t.progress += t.speed + randf(0.f, 0.005f);
            if (t.progress >= 1.0f) {
                t.progress = 1.0f;
            }
        } else if (randf(0.f, 1.f) < 0.002f) {
            // Restart completed tasks occasionally
            t.progress = 0.0f;
            t.speed = randf(0.003f, 0.02f);
        }
    }

    // Network throughput
    g_net_rx.push_back(std::clamp(g_net_rx.back() + randf(-8.f, 8.f), 5.f, 95.f));
    g_net_tx.push_back(std::clamp(g_net_tx.back() + randf(-5.f, 5.f), 2.f, 60.f));
    if (g_net_rx.size() > 60) g_net_rx.erase(g_net_rx.begin());
    if (g_net_tx.size() > 60) g_net_tx.erase(g_net_tx.begin());

    // Heatmap (slowly evolve)
    if (g_frame % 30 == 0) {
        int day = g_rng() % 7;
        int hour = g_rng() % 24;
        g_heatmap[day][hour] = std::clamp(g_heatmap[day][hour] + randf(-0.1f, 0.15f), 0.0f, 1.0f);
    }
}

// ── Color helpers ────────────────────────────────────────────────────────────

static Color metric_color(float v) {
    if (v > 0.85f) return thm().danger;
    if (v > 0.65f) return thm().warm;
    return thm().ok;
}

static Style metric_style(float v) {
    return Style{}.with_fg(metric_color(v)).with_bold();
}

// ── Build panels ─────────────────────────────────────────────────────────────

static Element build_header() {
    char ts[32];
    int mins = static_cast<int>(g_time) / 60;
    int secs = static_cast<int>(g_time) % 60;
    std::snprintf(ts, sizeof(ts), "%02d:%02d", mins, secs);

    return (h(
        text(" TELEMETRY") | Bold | fg_t(thm().accent),
        text("  \xe2\x94\x82  ") | fg_t(thm().dim),
        text(themes[g_theme].name) | Bold | fg_t(thm().accent2),
        text("  \xe2\x94\x82  ") | fg_t(thm().dim),
        text("\xe2\x8f\xb1 " + std::string(ts)) | fg_t(thm().bright),
        text(g_paused ? "  \xe2\x96\x90\xe2\x96\x90 PAUSED" : "") | Bold | fg_t(thm().warm),
        space,
        text(" 1-3") | Bold | fg_t(thm().accent), text(":theme") | fg_t(thm().dim),
        text(" \xe2\x90\xa3") | Bold | fg_t(thm().accent), text(":pause") | fg_t(thm().dim),
        text(" r") | Bold | fg_t(thm().accent), text(":reset") | fg_t(thm().dim),
        text(" q") | Bold | fg_t(thm().accent), text(":quit ") | fg_t(thm().dim)
    ) | pad<0, 1, 0, 1> | Bg<30, 30, 42>).build();
}

static Element build_gauges() {
    std::vector<Element> gauges;
    for (auto* m : g_metrics) {
        gauges.push_back(
            Gauge(m->value, m->name, metric_color(m->value), GaugeStyle::Arc).build()
        );
    }

    return (vstack()(
        text(" SYSTEM METRICS") | Bold | fg_t(thm().accent),
        h(
            v(gauges[0]) | grow_<1>, v(gauges[1]) | grow_<1>, v(gauges[2]) | grow_<1>
        ),
        h(
            v(gauges[3]) | grow_<1>, v(gauges[4]) | grow_<1>, v(gauges[5]) | grow_<1>
        )
    )).build();
}

static Element build_sparklines() {
    std::vector<Element> rows;
    for (auto* m : g_metrics) {
        Sparkline spark(m->history, {
            .color = metric_color(m->value),
            .label_style = fg_t(thm().bright),
            .value_style = metric_style(m->value),
            .show_last = true,
        });
        spark.set_label(m->name);
        rows.push_back(spark.build());
    }
    return (vstack()(
        text(" HISTORY") | Bold | fg_t(thm().accent),
        rows[0], rows[1], rows[2], rows[3], rows[4], rows[5]
    )).build();
}

static Element build_process_chart() {
    std::vector<Bar> bars;
    for (auto& p : g_procs) {
        Color c = p.cpu > 15.f ? thm().danger : (p.cpu > 8.f ? thm().warm : thm().accent);
        bars.push_back({p.name, p.cpu / 40.f, c});
    }
    BarChart chart(std::move(bars), 1.0f);
    chart.set_default_color(thm().accent);
    return (vstack()(
        text(" CPU BY PROCESS") | Bold | fg_t(thm().accent),
        chart.build()
    )).build();
}

static Element build_process_table() {
    Table tbl(
        {
            {"PID", 6, ColumnAlign::Right},
            {"PROCESS", 16, ColumnAlign::Left},
            {"CPU%", 8, ColumnAlign::Right},
            {"MEM", 10, ColumnAlign::Right},
        },
        TableConfig{
            .header_style = Style{}.with_bold().with_fg(thm().accent),
            .row_style = fg_t(thm().bright),
            .alt_row_style = fg_t(thm().dim),
            .separator_style = fg_t(thm().dim),
            .stripe_rows = true,
            .show_border = true,
            .title = "TOP PROCESSES",
            .border_color = thm().dim,
        });

    // Sort by CPU descending
    auto sorted = std::vector<Process*>();
    for (auto& p : g_procs) sorted.push_back(&p);
    std::sort(sorted.begin(), sorted.end(),
              [](auto* a, auto* b) { return a->cpu > b->cpu; });

    for (auto* p : sorted) {
        char cpu_str[16], mem_str[16];
        std::snprintf(cpu_str, sizeof(cpu_str), "%.1f%%", p->cpu);
        std::snprintf(mem_str, sizeof(mem_str), "%.0fMB", p->mem_mb);
        tbl.add_row({
            std::to_string(p->pid),
            p->name,
            cpu_str,
            mem_str,
        });
    }
    return tbl.build();
}

static Element build_network_chart() {
    LineChart rx_chart(g_net_rx, 8);
    rx_chart.set_label("RX");
    rx_chart.set_color(thm().accent);

    LineChart tx_chart(g_net_tx, 8);
    tx_chart.set_label("TX");
    tx_chart.set_color(thm().accent2);

    return (vstack()(
        text(" NETWORK I/O") | Bold | fg_t(thm().accent),
        h(
            v(vstack()(
                text(" RX (Mbps)") | fg_t(thm().accent),
                rx_chart.build()
            )) | grow_<1>,
            v(vstack()(
                text(" TX (Mbps)") | fg_t(thm().accent2),
                tx_chart.build()
            )) | grow_<1>
        )
    )).build();
}

static Element build_heatmap() {
    std::vector<std::vector<float>> data;
    for (auto& row : g_heatmap) {
        data.push_back(std::vector<float>(row, row + 24));
    }
    Heatmap hm(std::move(data));
    hm.set_x_labels({"0","","","3","","","6","","","9","","","12","","","15","","","18","","","21","",""});
    hm.set_y_labels({"Mon","Tue","Wed","Thu","Fri","Sat","Sun"});
    hm.set_low_color(Color::rgb(thm().bg_r, thm().bg_g, thm().bg_b));
    hm.set_high_color(thm().accent);

    return (vstack()(
        text(" LOAD HEATMAP (24h x 7d)") | Bold | fg_t(thm().accent),
        hm.build()
    )).build();
}

static Element build_tasks() {
    std::vector<Element> rows;
    for (auto& t : g_tasks) {
        ProgressBar bar(ProgressConfig{
            .fill_color = t.progress >= 1.0f ? thm().ok : thm().accent,
            .bg_color = Color::rgb(40, 42, 54),
            .show_percentage = true,
        });
        bar.set(t.progress);
        bar.set_label(t.name);
        rows.push_back(bar.build());
    }
    return (vstack()(
        text(" ACTIVE TASKS") | Bold | fg_t(thm().accent),
        rows[0], rows[1], rows[2], rows[3]
    )).build();
}

static Element build_status_bar() {
    float total_cpu = 0.f;
    for (auto& p : g_procs) total_cpu += p.cpu;

    char buf[64];
    std::snprintf(buf, sizeof(buf), " load:%.0f%%", total_cpu);

    int errors = 0, warns = 0;
    for (auto* m : g_metrics) {
        if (m->value > 0.85f) errors++;
        else if (m->value > 0.65f) warns++;
    }

    return (h(
        text(buf) | Bold | fg_t(thm().bright),
        text("  alerts:") | fg_t(Color::rgb(140, 140, 160)),
        text(std::to_string(errors), errors > 0 ? fg_t(thm().danger).with_bold() : fg_t(thm().dim)),
        text("/") | fg_t(thm().dim),
        text(std::to_string(warns), warns > 0 ? fg_t(thm().warm).with_bold() : fg_t(thm().dim)),
        text("  procs:" + std::to_string(sizeof(g_procs) / sizeof(g_procs[0]))) | fg_t(Color::rgb(140, 140, 160)),
        space,
        text(" f:" + std::to_string(g_frame)) | fg_t(Color::rgb(100, 100, 120)),
        text("  powered by ") | fg_t(Color::rgb(55, 55, 70)),
        text("maya") | fg_t(thm().accent),
        text(" ")
    ) | pad<0, 1, 0, 1> | Bg<25, 25, 38>).build();
}

// ── Render ───────────────────────────────────────────────────────────────────

static Element render() {
    tick(1.0f / 15.0f);

    return vstack()(
        build_header(),
        h(
            // Left column: gauges + sparklines
            v(vstack()(
                build_gauges(),
                build_sparklines()
            )) | grow_<1>,
            // Right column: process chart + tasks
            v(vstack()(
                build_process_chart(),
                build_tasks()
            )) | grow_<1>
        ),
        h(
            // Bottom left: network
            v(build_network_chart()) | grow_<1>,
            // Bottom right: heatmap
            v(build_heatmap()) | grow_<1>
        ),
        build_process_table(),
        build_status_bar()
    );
}

// ── Main ─────────────────────────────────────────────────────────────────────

static void init() {
    // Seed heatmap with realistic patterns
    for (int d = 0; d < 7; ++d) {
        for (int h = 0; h < 24; ++h) {
            // Business hours get more load
            float base = (h >= 9 && h <= 17) ? 0.5f : 0.15f;
            // Weekends are lighter
            if (d >= 5) base *= 0.4f;
            // Lunch dip
            if (h == 12 || h == 13) base *= 0.7f;
            g_heatmap[d][h] = std::clamp(base + randf(-0.1f, 0.1f), 0.0f, 1.0f);
        }
    }
    // Seed metric histories
    for (auto* m : g_metrics) {
        m->history.reserve(80);
        for (int i = 0; i < 40; ++i) {
            m->history.push_back(std::clamp(m->value + randf(-0.1f, 0.1f), 0.0f, 1.0f));
        }
    }
    // Seed network data
    for (auto& v : g_net_rx) v = randf(20.f, 50.f);
    for (auto& v : g_net_tx) v = randf(10.f, 30.f);
}

int main() {
    init();

    maya::run(
        {.title = "telemetry", .fps = 15, .mode = maya::Mode::Fullscreen},
        [](const maya::Event& ev) {
            using SK = maya::SpecialKey;
            maya::on(ev, 'q', [] { maya::quit(); });
            maya::on(ev, SK::Escape, [] { maya::quit(); });
            maya::on(ev, ' ', [] { g_paused = !g_paused; });
            maya::on(ev, 'r', [] {
                for (auto* m : g_metrics) { m->history.clear(); m->value = 0.3f; }
                g_time = 0.f; g_frame = 0;
            });
            maya::on(ev, '1', [] { g_theme = 0; });
            maya::on(ev, '2', [] { g_theme = 1; });
            maya::on(ev, '3', [] { g_theme = 2; });
        },
        render
    );
}
