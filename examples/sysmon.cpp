// sysmon.cpp — Fullscreen system monitor / hacker console
//
// A nerdy live dashboard showing fake system telemetry: CPU cores with
// sparklines, memory banks, network interfaces, process table, entropy
// pool, and a scrolling activity log — all rendered with the maya DSL.
//
// Controls:
//   q/Esc     quit
//   p         pause/resume simulation
//   l         toggle log panel
//   s         sort processes: cpu / mem / pid
//   1-3       speed: slow / normal / fast
//   space     inject random log burst
//
// Usage:  ./maya_sysmon

#include <maya/dsl.hpp>
#include <maya/app/run.hpp>
#include <maya/app/events.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace maya::dsl;

// ── Helpers ─────────────────────────────────────────────────────────────────

// Runtime color helper — needed because ternary can't mix different StyTag types.
static maya::Style fg_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return maya::Style{}.with_fg(maya::Color::rgb(r, g, b));
}

static maya::Style usage_color(float v) {
    if (v > 0.8f) return fg_rgb(255, 60, 60);
    if (v > 0.5f) return fg_rgb(255, 200, 60);
    return fg_rgb(0, 255, 136);
}

static maya::Style level_color(int lvl) {
    if (lvl == 2) return fg_rgb(255, 60, 60);
    if (lvl == 1) return fg_rgb(255, 200, 60);
    return fg_rgb(80, 80, 100);
}

static std::mt19937 rng{std::random_device{}()};

static int randi(int lo, int hi) {
    return std::uniform_int_distribution<int>(lo, hi)(rng);
}

static float randf(float lo, float hi) {
    return std::uniform_real_distribution<float>(lo, hi)(rng);
}

// ── Braille spark ───────────────────────────────────────────────────────────
// Maps 0.0–1.0 to a braille vertical bar character (8 levels).

static const char* braille_bar(float v) {
    static const char* bars[] = {
        "⠀", "⣀", "⣤", "⣴", "⣶", "⣷", "⣿", "⣿"
    };
    int idx = std::clamp(static_cast<int>(v * 7.0f), 0, 7);
    return bars[idx];
}

// Block bar (for gauges): filled/empty using ▓░

static std::string block_bar(float v, int width) {
    int filled = std::clamp(static_cast<int>(v * static_cast<float>(width)), 0, width);
    std::string s;
    for (int i = 0; i < filled; ++i) s += "▓";
    for (int i = filled; i < width; ++i) s += "░";
    return s;
}

// ── Spinners ────────────────────────────────────────────────────────────────

static const char* dot_spin(int frame) {
    static const char* frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    return frames[frame % 10];
}

static const char* hex_char() {
    static const char hex[] = "0123456789abcdef";
    static char buf[2] = {};
    buf[0] = hex[rng() % 16];
    return buf;
}

// ── Data Model ──────────────────────────────────────────────────────────────

struct Core {
    float usage      = 0;
    float temp       = 45;
    float freq       = 3.2f;
    std::array<float, 20> history{};  // spark line
    int hist_idx     = 0;
};

struct MemBank {
    std::string name;
    float used = 0;    // 0–1
    int total_gb = 0;
};

struct NetIface {
    std::string name;
    float rx_mbps = 0;
    float tx_mbps = 0;
    uint64_t rx_total = 0;
    uint64_t tx_total = 0;
    std::array<float, 16> rx_spark{};
    int spark_idx = 0;
};

struct Process {
    std::string name;
    int pid;
    float cpu;
    float mem_mb;
};

struct LogEntry {
    float timestamp;
    std::string msg;
    int level; // 0=info, 1=warn, 2=err
};

// ── Global State ────────────────────────────────────────────────────────────

static constexpr int NUM_CORES = 8;
static constexpr int NUM_MEM = 4;
static constexpr int NUM_NET = 3;
static constexpr int MAX_LOG = 8;

static std::array<Core, NUM_CORES> cores;
static std::array<MemBank, NUM_MEM> mem_banks;
static std::array<NetIface, NUM_NET> net_ifaces;
static std::vector<Process> processes;
static std::vector<LogEntry> activity_log;
static float uptime = 0;
static int frame_count = 0;
static float entropy_pool = 0.72f;
static uint64_t total_syscalls = 0;

// Controls
static bool paused = false;
static bool show_log = true;
static int sort_mode = 0;  // 0=cpu, 1=mem, 2=pid
static float speed = 1.0f;
static const char* sort_names[] = {"cpu", "mem", "pid"};

// ── Init ────────────────────────────────────────────────────────────────────

static void init_state() {
    for (int i = 0; i < NUM_CORES; ++i) {
        cores[i].usage = randf(0.05f, 0.4f);
        cores[i].temp = randf(42, 58);
        cores[i].freq = randf(2.8f, 4.2f);
    }

    mem_banks[0] = {"DIMM-A1", 0.62f, 16};
    mem_banks[1] = {"DIMM-A2", 0.45f, 16};
    mem_banks[2] = {"DIMM-B1", 0.38f, 32};
    mem_banks[3] = {"DIMM-B2", 0.21f, 32};

    net_ifaces[0] = {"eth0",  0, 0, 0, 0, {}, 0};
    net_ifaces[1] = {"wlan0", 0, 0, 0, 0, {}, 0};
    net_ifaces[2] = {"lo",    0, 0, 0, 0, {}, 0};

    processes = {
        {"systemd",     1,    0.1f,  12.4f},
        {"sshd",        892,  0.3f,  8.2f},
        {"postgres",    1204, 4.2f,  256.0f},
        {"nginx",       1567, 1.8f,  64.0f},
        {"node",        2341, 12.5f, 512.0f},
        {"redis-server", 3012, 2.1f, 128.0f},
        {"containerd",  445,  3.7f,  96.0f},
        {"kubelet",     512,  5.2f,  384.0f},
    };
}

// ── Tick ────────────────────────────────────────────────────────────────────

static const std::array<std::string, 16> log_msgs = {
    "kernel: TCP connection established 10.0.3.17:443",
    "sshd[892]: auth accepted publickey for root",
    "nginx: GET /api/v2/health 200 0.003s",
    "postgres: autovacuum launcher started",
    "containerd: snapshot commit sha256:a8f3...",
    "kubelet: pod scheduled kube-system/coredns",
    "kernel: NMI watchdog: perf interrupt took too long",
    "systemd: Started Session 847 of user deploy",
    "redis: RDB snapshot saved to disk",
    "kernel: audit: seccomp filter match pid=2341",
    "node: worker 3 listening on :8080",
    "kernel: nouveau: VRAM low, reclaiming pages",
    "sshd[892]: session closed for user root",
    "nginx: upstream timed out (110: Connection timed out)",
    "postgres: checkpoint starting: wal",
    "kernel: Out of memory: Killed process 9921 (chrome)",
};

static void tick(float dt) {
    if (paused) { frame_count++; return; }
    dt *= speed;
    uptime += dt;
    frame_count++;
    total_syscalls += static_cast<uint64_t>(randi(800, 3000));

    // CPU cores — simulate bursty workloads
    for (auto& c : cores) {
        float target = randf(0.02f, 0.95f);
        // Occasionally spike
        if (randi(0, 60) == 0) target = randf(0.8f, 1.0f);
        c.usage += (target - c.usage) * 0.15f;
        c.temp = 42.0f + c.usage * 38.0f + randf(-2, 2);
        c.freq = 2.4f + c.usage * 2.0f + randf(-0.1f, 0.1f);
        c.history[static_cast<size_t>(c.hist_idx)] = c.usage;
        c.hist_idx = (c.hist_idx + 1) % 20;
    }

    // Memory — slow drift
    for (auto& m : mem_banks) {
        m.used += randf(-0.01f, 0.015f);
        m.used = std::clamp(m.used, 0.05f, 0.95f);
    }

    // Network
    for (auto& n : net_ifaces) {
        float base_rx = (&n == &net_ifaces[2]) ? randf(0, 5) : randf(0, 800);
        float base_tx = (&n == &net_ifaces[2]) ? randf(0, 5) : randf(0, 200);
        n.rx_mbps += (base_rx - n.rx_mbps) * 0.2f;
        n.tx_mbps += (base_tx - n.tx_mbps) * 0.2f;
        n.rx_total += static_cast<uint64_t>(n.rx_mbps * dt * 125000);
        n.tx_total += static_cast<uint64_t>(n.tx_mbps * dt * 125000);
        n.rx_spark[static_cast<size_t>(n.spark_idx)] = n.rx_mbps / 800.0f;
        n.spark_idx = (n.spark_idx + 1) % 16;
    }

    // Processes — jitter CPU
    for (auto& p : processes) {
        p.cpu += randf(-2, 2);
        p.cpu = std::clamp(p.cpu, 0.0f, 100.0f);
        p.mem_mb += randf(-5, 5);
        p.mem_mb = std::max(1.0f, p.mem_mb);
    }

    // Entropy
    entropy_pool += randf(-0.03f, 0.03f);
    entropy_pool = std::clamp(entropy_pool, 0.3f, 1.0f);

    // Log
    if (randi(0, 8) == 0) {
        int lvl = (randi(0, 10) < 7) ? 0 : (randi(0, 3) == 0 ? 2 : 1);
        activity_log.push_back({uptime, log_msgs[static_cast<size_t>(randi(0, 15))], lvl});
        if (activity_log.size() > MAX_LOG)
            activity_log.erase(activity_log.begin());
    }
}

// ── UI Builders ─────────────────────────────────────────────────────────────

static auto build_header() {
    std::string hash;
    for (int i = 0; i < 8; ++i) hash += hex_char();

    auto spin = std::string(dot_spin(frame_count));

    std::string state_str = paused ? " ⏸ PAUSED" : "";
    std::string speed_str = " ×" + std::to_string(static_cast<int>(speed));

    return h(
        text(spin + " SYSMON") | Bold | Fg<0, 255, 136>,
        text(" v3.7.1") | Dim,
        text(state_str) | Bold | Fg<255, 60, 60>,
        text(speed_str) | Fg<255, 200, 60>,
        space,
        text("node:") | Dim,
        text("prod-us-east-1a") | Fg<100, 180, 255>,
        text(" 0x" + hash) | Fg<80, 80, 100>
    ) | pad<0, 1, 0, 1>;
}

static auto build_cpu_panel() {
    std::vector<maya::Element> rows;

    for (int i = 0; i < NUM_CORES; ++i) {
        auto& c = cores[static_cast<size_t>(i)];

        // Sparkline from history
        std::string spark;
        for (int j = 0; j < 20; ++j) {
            int idx = (c.hist_idx + j) % 20;
            spark += braille_bar(c.history[static_cast<size_t>(idx)]);
        }

        // Color based on usage
        auto usage_pct = static_cast<int>(c.usage * 100);
        auto temp_val = static_cast<int>(c.temp);
        auto freq_str = std::to_string(static_cast<int>(c.freq * 10));
        freq_str.insert(freq_str.size() - 1, ".");

        auto bar = block_bar(c.usage, 12);
        freq_str += "GHz";

        auto row = h(
            text("CPU" + std::to_string(i)) | Bold | Fg<100, 180, 255> | w_<5>,
            text(bar, usage_color(c.usage)) | w_<12>,
            text(std::to_string(usage_pct) + "%") | Bold | w_<5>,
            text(std::to_string(temp_val) + "°C") | Dim | w_<5>,
            text(freq_str) | Dim | w_<8>,
            text(spark) | Fg<80, 200, 255>
        ) | gap_<1>;

        rows.push_back(row.build());
    }

    // Total CPU line
    float avg = 0;
    for (auto& c : cores) avg += c.usage;
    avg /= NUM_CORES;

    rows.push_back((h(
        text("TOTAL") | Bold | Fg<255, 200, 60> | w_<5>,
        text(block_bar(avg, 12)) | Fg<255, 200, 60> | w_<12>,
        text(std::to_string(static_cast<int>(avg * 100)) + "%") | Bold | w_<5>,
        text("load:" + std::to_string(static_cast<int>(avg * NUM_CORES * 1.2f * 10))) | Dim
    ) | gap_<1>).build());

    return vstack().border(maya::BorderStyle::Round)
        .border_color(maya::Color::rgb(50, 55, 70))
        .border_text(" CPU ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static auto build_mem_panel() {
    std::vector<maya::Element> rows;

    float total_used = 0, total_cap = 0;
    for (auto& m : mem_banks) {
        total_used += m.used * static_cast<float>(m.total_gb);
        total_cap += static_cast<float>(m.total_gb);

        auto used_gb = m.used * static_cast<float>(m.total_gb);
        auto bar = block_bar(m.used, 16);

        auto mem_str = std::to_string(static_cast<int>(used_gb)) + "/" + std::to_string(m.total_gb) + "G";

        rows.push_back((h(
            text(m.name) | Fg<100, 180, 255> | w_<8>,
            text(bar, usage_color(m.used)) | w_<16>,
            text(mem_str) | Dim | w_<8>
        ) | gap_<1>).build());
    }

    // Summary
    auto pct = static_cast<int>(total_used / total_cap * 100);
    rows.push_back((h(
        text("TOTAL") | Bold | Fg<255, 200, 60> | w_<8>,
        text(std::to_string(static_cast<int>(total_used)) + "/" + std::to_string(static_cast<int>(total_cap)) + " GB") | Bold,
        text("(" + std::to_string(pct) + "%)") | Dim,
        text("swap: 0K") | Dim
    ) | gap_<1>).build());

    return vstack().border(maya::BorderStyle::Round)
        .border_color(maya::Color::rgb(50, 55, 70))
        .border_text(" MEM ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static auto build_net_panel() {
    std::vector<maya::Element> rows;

    for (auto& n : net_ifaces) {
        // Mini sparkline
        std::string spark;
        for (int j = 0; j < 16; ++j) {
            int idx = (n.spark_idx + j) % 16;
            spark += braille_bar(n.rx_spark[static_cast<size_t>(idx)]);
        }

        auto rx_str = std::to_string(static_cast<int>(n.rx_mbps));
        auto tx_str = std::to_string(static_cast<int>(n.tx_mbps));

        auto fmt_bytes = [](uint64_t b) -> std::string {
            if (b > 1'000'000'000) return std::to_string(b / 1'000'000'000) + "G";
            if (b > 1'000'000) return std::to_string(b / 1'000'000) + "M";
            if (b > 1'000) return std::to_string(b / 1'000) + "K";
            return std::to_string(b) + "B";
        };

        rows.push_back((h(
            text(n.name) | Bold | Fg<100, 180, 255> | w_<6>,
            text("▲ " + tx_str) | Fg<255, 120, 80> | w_<6>,
            text("▼ " + rx_str) | Fg<0, 255, 136> | w_<6>,
            text("Mb/s") | Dim | w_<5>,
            text(fmt_bytes(n.rx_total) + "/" + fmt_bytes(n.tx_total)) | Dim | w_<10>,
            text(spark) | Fg<120, 80, 255>
        ) | gap_<1>).build());
    }

    return vstack().border(maya::BorderStyle::Round)
        .border_color(maya::Color::rgb(50, 55, 70))
        .border_text(" NET ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static auto build_proc_panel() {
    auto sorted = processes;
    if (sort_mode == 0)
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.cpu > b.cpu; });
    else if (sort_mode == 1)
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.mem_mb > b.mem_mb; });
    else
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.pid < b.pid; });

    std::vector<maya::Element> rows;

    rows.push_back((h(
        t<"PID"> | Bold | Dim | w_<6>,
        t<"NAME"> | Bold | Dim | w_<16>,
        t<"CPU%"> | Bold | Dim | w_<6>,
        t<"MEM"> | Bold | Dim | w_<6>
    )).build());

    for (int i = 0; i < std::min(6, static_cast<int>(sorted.size())); ++i) {
        auto& p = sorted[static_cast<size_t>(i)];

        rows.push_back((h(
            text(std::to_string(p.pid)) | Fg<100, 180, 255> | w_<6>,
            text(p.name) | Bold | w_<16>,
            text(std::to_string(static_cast<int>(p.cpu)) + "%", usage_color(p.cpu / 100.0f)) | w_<6>,
            text(std::to_string(static_cast<int>(p.mem_mb)) + "M") | Dim | w_<6>
        )).build());
    }

    return vstack().border(maya::BorderStyle::Round)
        .border_color(maya::Color::rgb(50, 55, 70))
        .border_text(std::string(" PROC [sort:") + sort_names[sort_mode] + "] ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static auto build_log_panel() {
    std::vector<maya::Element> rows;

    for (auto& e : activity_log) {
        int mins = static_cast<int>(e.timestamp) / 60;
        int secs = static_cast<int>(e.timestamp) % 60;
        char ts[16];
        std::snprintf(ts, sizeof(ts), "%02d:%02d", mins, secs);

        auto level_tag = (e.level == 2) ? "ERR " : (e.level == 1 ? "WARN" : "INFO");

        auto row = h(
            text(std::string(ts)) | Dim | w_<6>,
            text(std::string(level_tag), level_color(e.level).with_bold()) | w_<5>,
            text(e.msg) | Dim | clip
        );
        rows.push_back(row.build());
    }

    // Fill empty slots
    while (rows.size() < MAX_LOG)
        rows.push_back((text("") | Dim).build());

    return vstack().border(maya::BorderStyle::Round)
        .border_color(maya::Color::rgb(50, 55, 70))
        .border_text(" LOG ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static auto build_status_bar() {
    int mins = static_cast<int>(uptime) / 60;
    int secs = static_cast<int>(uptime) % 60;
    char ts[16];
    std::snprintf(ts, sizeof(ts), "%02d:%02d", mins, secs);

    auto entropy_bar = block_bar(entropy_pool, 8);

    // Syscall counter
    std::string sc_str;
    if (total_syscalls > 1'000'000'000) sc_str = std::to_string(total_syscalls / 1'000'000'000) + "G";
    else if (total_syscalls > 1'000'000) sc_str = std::to_string(total_syscalls / 1'000'000) + "M";
    else if (total_syscalls > 1'000) sc_str = std::to_string(total_syscalls / 1'000) + "K";
    else sc_str = std::to_string(total_syscalls);

    return h(
        text(" ⏱ " + std::string(ts)) | Fg<100, 180, 255>,
        text("  entropy:") | Fg<140, 140, 160>,
        text(entropy_bar) | Fg<0, 255, 136>,
        text("  syscalls:") | Fg<140, 140, 160>,
        text(sc_str) | Fg<255, 200, 60>,
        text("  f:" + std::to_string(frame_count)) | Fg<100, 100, 120>,
        space,
        text(" q") | Bold | Fg<180, 220, 255>, text(":quit") | Fg<120, 120, 140>,
        text(" p") | Bold | Fg<180, 220, 255>, text(":pause") | Fg<120, 120, 140>,
        text(" s") | Bold | Fg<180, 220, 255>, text(":sort") | Fg<120, 120, 140>,
        text(" l") | Bold | Fg<180, 220, 255>, text(":log") | Fg<120, 120, 140>,
        text(" 1-3") | Bold | Fg<180, 220, 255>, text(":speed") | Fg<120, 120, 140>,
        text(" ␣") | Bold | Fg<180, 220, 255>, text(":burst ") | Fg<120, 120, 140>
    ) | pad<0, 1, 0, 1> | Bg<30, 30, 42>;
}

// ── Render ──────────────────────────────────────────────────────────────────

static maya::Element render() {
    // Tick with a fixed dt (fps-driven)
    tick(1.0f / 15.0f);

    std::vector<maya::Element> panels;
    panels.push_back(build_header().build());
    panels.push_back(build_cpu_panel());
    panels.push_back(build_mem_panel());
    panels.push_back(build_net_panel());
    panels.push_back(build_proc_panel());
    if (show_log) panels.push_back(build_log_panel());
    panels.push_back(build_status_bar().build());

    return vstack()(std::move(panels));
}

// ── Main ────────────────────────────────────────────────────────────────────

int main() {
    init_state();

    maya::run(
        {.title = "sysmon", .fps = 15, .mode = maya::Mode::Inline},
        [&](const maya::Event& ev) {
            maya::on(ev, 'q', [] { maya::quit(); });
            maya::on(ev, maya::SpecialKey::Escape, [] { maya::quit(); });
            maya::on(ev, 'p', [] { paused = !paused; });
            maya::on(ev, 'l', [] { show_log = !show_log; });
            maya::on(ev, 's', [] { sort_mode = (sort_mode + 1) % 3; });
            maya::on(ev, '1', [] { speed = 0.25f; });
            maya::on(ev, '2', [] { speed = 1.0f; });
            maya::on(ev, '3', [] { speed = 4.0f; });
            maya::on(ev, ' ', [] {
                // Burst: inject 5 random log entries
                for (int i = 0; i < 5; ++i) {
                    int lvl = randi(0, 2);
                    activity_log.push_back({uptime, log_msgs[static_cast<size_t>(randi(0, 15))], lvl});
                }
                while (activity_log.size() > MAX_LOG)
                    activity_log.erase(activity_log.begin());
            });
        },
        [&] { return render(); }
    );

    return 0;
}
