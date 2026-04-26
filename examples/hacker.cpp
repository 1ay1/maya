// hacker.cpp — Cyberpunk hacker terminal (simple run() API)
//
// A movie-style "hacking" terminal: rapid scrolling data, flashing alerts,
// network intrusion simulation, hex dumps, progress bars, sparklines,
// heatmaps. Pure eye candy.
//
// Controls:
//   space       initiate breach sequence
//   e           extract data (rapid hex dump)
//   c           cover tracks (dim + delete log entries)
//   1           green theme
//   2           amber theme
//   3           cyan theme
//   q/Esc       quit
//
// Usage:  ./maya_hacker

#include <maya/maya.hpp>
#include <maya/widget/badge.hpp>
#include <maya/widget/heatmap.hpp>
#include <maya/widget/progress.hpp>
#include <maya/widget/sparkline.hpp>
#include <maya/widget/toast.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace maya::dsl;

// ── RNG ────────────────────────────────────────────────────────────────────

static std::mt19937 rng{std::random_device{}()};
static int randi(int lo, int hi) {
    return std::uniform_int_distribution<int>(lo, hi)(rng);
}
static float randf(float lo, float hi) {
    return std::uniform_real_distribution<float>(lo, hi)(rng);
}

// ── Theme ──────────────────────────────────────────────────────────────────

struct Theme {
    const char* name;
    uint8_t primary[3];
    uint8_t bright[3];
    uint8_t dim[3];
    uint8_t accent[3];
    uint8_t alert[3];
    uint8_t border[3];
};

static const Theme themes[] = {
    {"PHOSPHOR",
     {0, 255, 65},    {0, 255, 136},   {0, 100, 30},
     {0, 200, 100},   {255, 50, 50},   {0, 60, 20}},
    {"AMBER",
     {255, 176, 0},   {255, 220, 80},  {140, 90, 0},
     {255, 200, 60},  {255, 60, 60},   {80, 55, 0}},
    {"ICE",
     {0, 200, 255},   {100, 220, 255}, {0, 80, 130},
     {0, 160, 220},   {255, 50, 80},   {0, 40, 70}},
};
static int theme_idx = 0;

static const Theme& thm() { return themes[theme_idx]; }
static maya::Style fg_t(const uint8_t c[3]) {
    return maya::Style{}.with_fg(maya::Color::rgb(c[0], c[1], c[2]));
}
static maya::Color col_t(const uint8_t c[3]) {
    return maya::Color::rgb(c[0], c[1], c[2]);
}

// ── Hex helpers ────────────────────────────────────────────────────────────

static char rand_hex() {
    static const char hx[] = "0123456789abcdef";
    return hx[rng() % 16];
}

static std::string rand_hex_str(int n) {
    std::string s;
    s.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) s += rand_hex();
    return s;
}

static std::string rand_ip() {
    return std::to_string(randi(10, 223)) + "." +
           std::to_string(randi(0, 255)) + "." +
           std::to_string(randi(0, 255)) + "." +
           std::to_string(randi(1, 254));
}

static std::string rand_hostname() {
    static const char* prefixes[] = {"srv","node","db","proxy","gw","vpn","fw","core","edge","cache"};
    static const char* suffixes[] = {".corp.net",".darknet.io",".shadow.sys",".zero.lan",".ghost.onion"};
    return std::string(prefixes[randi(0, 9)]) + "-" +
           std::to_string(randi(1, 99)) + suffixes[randi(0, 4)];
}

// ── Block bar ──────────────────────────────────────────────────────────────

static std::string block_bar(float v, int width) {
    int filled = std::clamp(static_cast<int>(v * static_cast<float>(width)), 0, width);
    std::string s;
    for (int i = 0; i < filled; ++i) s += "\xe2\x96\x88";  // █
    for (int i = filled; i < width; ++i) s += "\xe2\x96\x91"; // ░
    return s;
}

// ── Spinners ───────────────────────────────────────────────────────────────

static const char* dot_spin(int frame) {
    static const char* frames[] = {"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};
    return frames[frame % 10];
}

// ── Data Model ─────────────────────────────────────────────────────────────

struct Target {
    std::string ip;
    std::string hostname;
    int port;
    std::string service;
    std::string status;   // "SCANNING", "OPEN", "EXPLOITED", "LOCKED"
    int vuln;             // 0=none, 1=MED, 2=HIGH, 3=CRITICAL
    float scan_progress;
    float age;
};

struct LogEntry {
    std::string timestamp;
    std::string message;
    int level;       // 0=info, 1=success, 2=warning, 3=error
    float opacity;   // 1.0=normal, fading to 0
};

// ── State ──────────────────────────────────────────────────────────────────

static std::vector<Target> targets;
static std::vector<LogEntry> terminal_log;
static constexpr int MAX_LOG = 18;

// Right panel data
static std::array<float, 20> inbound_spark{};
static std::array<float, 20> outbound_spark{};
static int spark_idx = 0;
static float crack_progress = 0.0f;
static std::vector<std::vector<float>> heatmap_data;
static float cpu_load = 0.45f;
static float mem_load = 0.62f;
static float net_load = 0.38f;
static float disk_io = 0.25f;

// Hex dump for bottom
static std::string hex_dump_line;

// Toast manager
static maya::ToastManager toasts({.duration = 3.5f, .fade_time = 0.8f, .max_visible = 3});

// Timing
static float elapsed = 0;
static int frame = 0;
static float next_target_time = 2.0f;
static float next_toast_time = 4.0f;

// Breach state
static bool breaching = false;
static float breach_timer = 0;
static int breach_phase = 0;

// Cover tracks state
static bool covering = false;
static float cover_timer = 0;

// Extract state
static bool extracting = false;
static float extract_timer = 0;

// ── Log helpers ────────────────────────────────────────────────────────────

static std::string timestamp() {
    int total = static_cast<int>(elapsed);
    int h = (total / 3600) % 24;
    int m = (total / 60) % 60;
    int s = total % 60;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
    return buf;
}

static void add_log(const std::string& msg, int level = 0) {
    terminal_log.push_back({timestamp(), msg, level, 1.0f});
    if (terminal_log.size() > MAX_LOG)
        terminal_log.erase(terminal_log.begin());
}

// ── Init ───────────────────────────────────────────────────────────────────

static const char* services[] = {"ssh","http","https","mysql","redis","postgres","ftp","smtp","dns","telnet"};
static const int ports[] = {22, 80, 443, 3306, 6379, 5432, 21, 25, 53, 23};

static void init_state() {
    // Initial targets
    for (int i = 0; i < 5; ++i) {
        int si = randi(0, 9);
        targets.push_back({
            rand_ip(), rand_hostname(), ports[si], services[si],
            i < 3 ? "OPEN" : "SCANNING",
            randi(0, 3),
            i < 3 ? 1.0f : randf(0.1f, 0.7f),
            randf(10.0f, 120.0f)
        });
    }

    // Initial log
    add_log("NEXUS://BREACH v4.2.0 initialized", 1);
    add_log("Loading exploit database... 2,847 modules", 0);
    add_log("Establishing encrypted tunnel via TOR", 0);
    add_log("Proxy chain: 3 hops active", 1);
    add_log("Target acquisition mode: ACTIVE", 2);

    // Init heatmap (6x8 grid)
    heatmap_data.resize(6);
    for (auto& row : heatmap_data) {
        row.resize(8);
        for (auto& v : row) v = randf(0.0f, 0.6f);
    }

    // Init hex dump
    hex_dump_line.reserve(80);
    for (int i = 0; i < 48; ++i) hex_dump_line += rand_hex();
}

// ── Tick ───────────────────────────────────────────────────────────────────

static const std::array<std::string, 24> log_templates = {
    "Scanning port %PORT%... OPEN",
    "Injecting payload 0x%HEX8%...",
    "Decrypting RSA-4096 block %HEX4%",
    "Brute forcing %SVC% credentials",
    "Intercepted packet from %IP%",
    "Tunneling through proxy node %N%",
    "Buffer overflow at 0x%HEX8%",
    "Shellcode deployed: %N% bytes",
    "Privilege escalation: uid=0(root)",
    "Dumping /etc/shadow... %N% entries",
    "Cracking hash: %HEX8%%HEX4%",
    "SQL injection on port %PORT%",
    "Reverse shell established %IP%:%PORT%",
    "ARP spoofing gateway %IP%",
    "DNS rebinding attack active",
    "Extracting certificates from %SVC%",
    "Patching kernel module 0x%HEX8%",
    "Keylogger installed PID %N%",
    "Exfiltrating %N%MB via covert channel",
    "Side-channel timing attack on AES",
    "Race condition exploit running...",
    "Heap spray: %N% allocations",
    "ROP chain: %N% gadgets linked",
    "Zero-day CVE-2026-%HEX4% triggered",
};

static std::string expand_template(const std::string& tmpl) {
    std::string out;
    for (size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] == '%' && i + 1 < tmpl.size()) {
            size_t end = tmpl.find('%', i + 1);
            if (end != std::string::npos) {
                std::string tag = tmpl.substr(i + 1, end - i - 1);
                if (tag == "PORT") out += std::to_string(ports[randi(0, 9)]);
                else if (tag == "HEX8") out += rand_hex_str(8);
                else if (tag == "HEX4") out += rand_hex_str(4);
                else if (tag == "IP") out += rand_ip();
                else if (tag == "SVC") out += services[randi(0, 9)];
                else if (tag == "N") out += std::to_string(randi(1, 9999));
                else { out += '%'; out += tag; out += '%'; }
                i = end;
                continue;
            }
        }
        out += tmpl[i];
    }
    return out;
}

static const std::array<std::string, 10> toast_messages = {
    "FIREWALL DETECTED",
    "ENCRYPTING CHANNEL",
    "BACKDOOR INSTALLED",
    "IDS ALERT BYPASSED",
    "PAYLOAD DELIVERED",
    "ROOT ACCESS OBTAINED",
    "EVIDENCE DESTROYED",
    "PROXY CHAIN ROTATED",
    "MEMORY WIPED",
    "TRACE ELIMINATED",
};

static void tick(float dt) {
    elapsed += dt;
    frame++;

    // Update sparklines
    if (frame % 3 == 0) {
        inbound_spark[static_cast<size_t>(spark_idx)] = randf(0.1f, 1.0f);
        outbound_spark[static_cast<size_t>(spark_idx)] = randf(0.05f, 0.7f);
        spark_idx = (spark_idx + 1) % 20;
    }

    // Update crack progress
    crack_progress += randf(0.001f, 0.004f) * dt * 15.0f;
    if (crack_progress > 1.0f) crack_progress = randf(0.0f, 0.15f);

    // Update heatmap
    for (auto& row : heatmap_data) {
        for (auto& v : row) {
            v += randf(-0.05f, 0.06f);
            v = std::clamp(v, 0.0f, 1.0f);
        }
    }
    // Hot spots that drift
    int hr = randi(0, 5);
    int hc = randi(0, 7);
    heatmap_data[static_cast<size_t>(hr)][static_cast<size_t>(hc)] =
        std::clamp(heatmap_data[static_cast<size_t>(hr)][static_cast<size_t>(hc)] + 0.15f, 0.0f, 1.0f);

    // System loads
    cpu_load += randf(-0.03f, 0.04f); cpu_load = std::clamp(cpu_load, 0.1f, 0.99f);
    mem_load += randf(-0.02f, 0.02f); mem_load = std::clamp(mem_load, 0.3f, 0.95f);
    net_load += randf(-0.04f, 0.05f); net_load = std::clamp(net_load, 0.05f, 0.95f);
    disk_io  += randf(-0.03f, 0.03f); disk_io  = std::clamp(disk_io, 0.05f, 0.80f);

    // Rotate hex dump
    hex_dump_line.clear();
    for (int i = 0; i < 64; ++i) hex_dump_line += rand_hex();

    // Add log entries
    if (randi(0, 4) == 0) {
        int idx = randi(0, 23);
        int lvl = (randi(0, 6) == 0) ? 1 : (randi(0, 8) == 0 ? 2 : 0);
        add_log(expand_template(log_templates[static_cast<size_t>(idx)]), lvl);
    }

    // Occasionally add a progress bar log
    if (randi(0, 20) == 0) {
        float pct = randf(0.2f, 0.98f);
        int filled = static_cast<int>(pct * 20);
        std::string bar;
        for (int i = 0; i < 20; ++i) bar += (i < filled) ? "\xe2\x96\x88" : "\xe2\x96\x91";
        char buf[16];
        std::snprintf(buf, sizeof(buf), " %d%%", static_cast<int>(pct * 100));
        add_log(bar + buf + " Decrypting...", 0);
    }

    // Occasionally add hex dump log
    if (randi(0, 15) == 0) {
        std::string hex = "0x" + rand_hex_str(4) + ": ";
        for (int i = 0; i < 8; ++i) {
            hex += rand_hex_str(2);
            if (i < 7) hex += " ";
        }
        add_log(hex, 0);
    }

    // Discover new targets
    if (elapsed > next_target_time) {
        next_target_time = elapsed + randf(3.0f, 8.0f);
        if (targets.size() < 12) {
            int si = randi(0, 9);
            targets.push_back({
                rand_ip(), rand_hostname(), ports[si], services[si],
                "SCANNING", randi(0, 3), 0.0f, 0.0f
            });
            add_log("New target discovered: " + targets.back().ip + " (" + targets.back().hostname + ")", 1);
        }
    }

    // Advance scanning targets
    for (auto& t : targets) {
        t.age += dt;
        if (t.status == "SCANNING") {
            t.scan_progress += randf(0.01f, 0.05f);
            if (t.scan_progress >= 1.0f) {
                t.scan_progress = 1.0f;
                t.status = "OPEN";
                t.vuln = randi(1, 3);
                add_log("Port " + std::to_string(t.port) + "/" + t.service +
                        " OPEN on " + t.ip, 1);
            }
        }
    }

    // Toast notifications
    if (elapsed > next_toast_time) {
        next_toast_time = elapsed + randf(5.0f, 12.0f);
        auto lvl = randi(0, 3) == 0 ? maya::ToastLevel::Error :
                   randi(0, 2) == 0 ? maya::ToastLevel::Warning :
                                      maya::ToastLevel::Success;
        toasts.push(toast_messages[static_cast<size_t>(randi(0, 9))], lvl);
    }
    toasts.advance(dt);

    // Breach sequence
    if (breaching) {
        breach_timer -= dt;
        if (breach_timer <= 0) {
            breach_phase++;
            breach_timer = randf(0.5f, 1.5f);
            if (breach_phase == 1) {
                add_log(">>> BREACH SEQUENCE INITIATED <<<", 3);
                add_log("Probing target defenses...", 2);
            } else if (breach_phase == 2) {
                add_log("Firewall rule injection: COMPLETE", 1);
                add_log("Escalating privileges...", 2);
            } else if (breach_phase == 3) {
                add_log("Root shell obtained on " + (targets.empty() ? "unknown" : targets[0].ip), 1);
                toasts.push("ACCESS GRANTED", maya::ToastLevel::Success);
            } else if (breach_phase == 4) {
                add_log("Installing persistent backdoor...", 0);
                add_log("Modifying syslog to hide traces", 0);
            } else if (breach_phase >= 5) {
                add_log(">>> BREACH COMPLETE <<<", 1);
                if (!targets.empty()) targets[0].status = "EXPLOITED";
                breaching = false;
                breach_phase = 0;
            }
        }
    }

    // Cover tracks
    if (covering) {
        cover_timer -= dt;
        if (cover_timer <= 0) {
            covering = false;
        }
        // Fade out log entries
        for (auto& e : terminal_log) {
            e.opacity -= dt * 0.8f;
            if (e.opacity < 0.1f) e.opacity = 0.1f;
        }
        if (!covering) {
            // Remove faded entries
            int to_remove = std::min(static_cast<int>(terminal_log.size()), randi(3, 8));
            for (int i = 0; i < to_remove; ++i) {
                if (!terminal_log.empty())
                    terminal_log.erase(terminal_log.begin());
            }
            add_log("Tracks covered. " + std::to_string(to_remove) + " log entries purged.", 1);
            toasts.push("EVIDENCE DESTROYED", maya::ToastLevel::Warning);
        }
    }

    // Extract data
    if (extracting) {
        extract_timer -= dt;
        if (extract_timer <= 0) {
            extracting = false;
            add_log("Extraction complete: " + std::to_string(randi(128, 4096)) + " MB exfiltrated", 1);
            toasts.push("DATA EXFILTRATED", maya::ToastLevel::Success);
        } else {
            // Rapid hex dump lines
            for (int i = 0; i < 3; ++i) {
                std::string line = "0x" + rand_hex_str(8) + ": ";
                for (int j = 0; j < 8; ++j) {
                    line += rand_hex_str(4) + " ";
                }
                add_log(line, 0);
            }
        }
    }
}

// ── UI Builders ────────────────────────────────────────────────────────────

static maya::Element build_header() {
    auto spin = std::string(dot_spin(frame));
    bool blink = (frame / 8) % 2 == 0;

    std::string status_dot = blink ? "●" : "○";
    std::string conn = "TOR x3 | PROXY: " + rand_ip() + " | LAT: " +
                       std::to_string(randi(12, 350)) + "ms";

    return (h(
        text(spin, fg_t(thm().primary)) | w_<2>,
        text("NEXUS://BREACH", fg_t(thm().bright).with_bold()) | w_<16>,
        text("v4.2.0") | Dim | w_<7>,
        text(status_dot, blink ? fg_t(thm().primary) : fg_t(thm().dim)),
        text(" CONNECTED", fg_t(thm().primary)) | w_<11>,
        space,
        text(conn, fg_t(thm().dim)) | clip,
        space,
        text(thm().name, fg_t(thm().accent).with_bold()) | w_<10>,
        text("0x" + rand_hex_str(6), fg_t(thm().dim))
    ) | pad<0, 1, 0, 1>).build();
}

static maya::Element build_targets_panel() {
    std::vector<maya::Element> rows;

    // Header
    rows.push_back((h(
        t<"IP/HOST"> | Bold | Dim | w_<20>,
        t<"PORT"> | Bold | Dim | w_<6>,
        t<"STATUS"> | Bold | Dim | w_<10>,
        t<"VULN"> | Bold | Dim
    ) | gap_<1>).build());

    for (auto& t : targets) {
        // Status color
        maya::Style status_style;
        if (t.status == "EXPLOITED")
            status_style = fg_t(thm().primary).with_bold();
        else if (t.status == "OPEN")
            status_style = maya::Style{}.with_fg(maya::Color::rgb(100, 200, 255));
        else if (t.status == "SCANNING")
            status_style = fg_t(thm().dim);
        else
            status_style = maya::Style{}.with_fg(maya::Color::rgb(255, 60, 60));

        std::string status_str = t.status;
        if (t.status == "SCANNING") {
            int pct = static_cast<int>(t.scan_progress * 100);
            status_str += " " + std::to_string(pct) + "%";
        }

        // Vulnerability badge
        maya::Element vuln_elem = text("").build();
        if (t.vuln == 3) {
            vuln_elem = maya::Badge::error("CRIT").build();
        } else if (t.vuln == 2) {
            vuln_elem = maya::Badge::warning("HIGH").build();
        } else if (t.vuln == 1) {
            maya::Badge::Config cfg;
            cfg.style = maya::Style{}.with_fg(maya::Color::rgb(229, 192, 123));
            vuln_elem = maya::Badge("MED", cfg).build();
        }

        // Truncate hostname if needed
        std::string display = t.ip;
        if (display.size() < 18) {
            display = t.hostname;
            if (display.size() > 18) display = display.substr(0, 18);
        }

        rows.push_back((h(
            text(display, fg_t(thm().accent)) | w_<20>,
            text(std::to_string(t.port), fg_t(thm().dim)) | w_<6>,
            text(status_str, status_style) | w_<10>,
            std::move(vuln_elem)
        ) | gap_<1>).build());
    }

    // Fill empty slots
    while (rows.size() < 10)
        rows.push_back(text("").build());

    return vstack().border(maya::BorderStyle::Round)
        .border_color(col_t(thm().border))
        .border_text(" TARGETS ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static maya::Element build_terminal_panel() {
    std::vector<maya::Element> rows;

    for (auto& e : terminal_log) {
        maya::Style ts_style = fg_t(thm().dim);
        maya::Style msg_style;

        if (e.level == 1) msg_style = fg_t(thm().primary).with_bold();
        else if (e.level == 2) msg_style = maya::Style{}.with_fg(maya::Color::rgb(255, 200, 60)).with_bold();
        else if (e.level == 3) msg_style = maya::Style{}.with_fg(maya::Color::rgb(255, 50, 50)).with_bold();
        else msg_style = fg_t(thm().primary);

        if (e.opacity < 0.5f) {
            ts_style = ts_style.with_dim();
            msg_style = msg_style.with_dim();
        }

        rows.push_back((h(
            text("[" + e.timestamp + "]", ts_style) | w_<12>,
            text(e.message, msg_style) | clip
        ) | gap_<1>).build());
    }

    // Fill remaining with empty
    while (rows.size() < MAX_LOG)
        rows.push_back(text("").build());

    return vstack().border(maya::BorderStyle::Round)
        .border_color(col_t(thm().border))
        .border_text(" TERMINAL ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static maya::Element build_intel_panel() {
    std::vector<maya::Element> rows;

    // --- Network traffic sparklines ---
    rows.push_back(text("NETWORK TRAFFIC", fg_t(thm().bright).with_bold()).build());

    // Reorder sparkline data from ring buffer
    auto reorder_spark = [](const std::array<float, 20>& arr, int idx) {
        std::vector<float> out(20);
        for (int i = 0; i < 20; ++i)
            out[static_cast<size_t>(i)] = arr[static_cast<size_t>((idx + i) % 20)];
        return out;
    };

    auto in_data = reorder_spark(inbound_spark, spark_idx);
    maya::Sparkline in_spark(in_data, {.color = col_t(thm().primary)});
    in_spark.set_label("IN ");
    in_spark.set_show_last(true);
    rows.push_back(in_spark.build());

    auto out_data = reorder_spark(outbound_spark, spark_idx);
    maya::Sparkline out_spark(out_data, {.color = col_t(thm().accent)});
    out_spark.set_label("OUT");
    out_spark.set_show_last(true);
    rows.push_back(out_spark.build());

    rows.push_back(text("").build());

    // --- Password cracking progress ---
    rows.push_back(text("PASSWORD CRACK", fg_t(thm().bright).with_bold()).build());
    maya::ProgressBar crack_bar({
        .width = 24,
        .fill_color = col_t(thm().primary),
        .bg_color = col_t(thm().border),
    });
    crack_bar.set(crack_progress);
    crack_bar.set_label("bcrypt");
    rows.push_back(crack_bar.build());

    rows.push_back(text("").build());

    // --- Network topology heatmap ---
    rows.push_back(text("NET TOPOLOGY", fg_t(thm().bright).with_bold()).build());
    maya::Heatmap hm(heatmap_data);
    hm.set_low_color(col_t(thm().border));
    hm.set_high_color(col_t(thm().primary));
    rows.push_back(hm.build());

    rows.push_back(text("").build());

    // --- System load gauges ---
    rows.push_back(text("SYSTEM LOAD", fg_t(thm().bright).with_bold()).build());

    auto gauge_line = [](const char* label, float val, const uint8_t color[3]) {
        auto bar = block_bar(val, 12);
        char pct[8];
        std::snprintf(pct, sizeof(pct), "%3d%%", static_cast<int>(val * 100));
        maya::Style bar_style;
        if (val > 0.8f) bar_style = maya::Style{}.with_fg(maya::Color::rgb(255, 60, 60));
        else if (val > 0.5f) bar_style = maya::Style{}.with_fg(maya::Color::rgb(255, 200, 60));
        else bar_style = maya::Style{}.with_fg(maya::Color::rgb(color[0], color[1], color[2]));
        return (h(
            text(label) | Dim | w_<5>,
            text(bar, bar_style) | w_<12>,
            text(pct) | Dim
        ) | gap_<1>).build();
    };

    rows.push_back(gauge_line("CPU", cpu_load, thm().primary));
    rows.push_back(gauge_line("MEM", mem_load, thm().primary));
    rows.push_back(gauge_line("NET", net_load, thm().primary));
    rows.push_back(gauge_line("I/O", disk_io, thm().primary));

    return vstack().border(maya::BorderStyle::Round)
        .border_color(col_t(thm().border))
        .border_text(" INTEL ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static maya::Element build_hex_footer() {
    // Format hex dump as "0000: XX XX XX XX XX XX XX XX  |ascii...|"
    std::string addr = "0x" + rand_hex_str(4) + ": ";
    std::string hex_part;
    std::string ascii_part = "|";
    for (int i = 0; i < 16; ++i) {
        size_t idx = static_cast<size_t>(i * 2);
        if (idx + 1 < hex_dump_line.size()) {
            hex_part += hex_dump_line.substr(idx, 2);
            if (i < 15) hex_part += " ";
            // Fake ASCII
            char c = static_cast<char>(randi(33, 126));
            ascii_part += c;
        }
    }
    ascii_part += "|";

    return (h(
        text(addr + hex_part + "  " + ascii_part, fg_t(thm().dim)) | clip,
        space,
        text(" SPC", fg_t(thm().bright).with_bold()) | w_<4>, text(":breach") | Fg<120, 120, 140> | w_<8>,
        text("e", fg_t(thm().bright).with_bold()) | w_<2>, text(":extract") | Fg<120, 120, 140> | w_<9>,
        text("c", fg_t(thm().bright).with_bold()) | w_<2>, text(":cover") | Fg<120, 120, 140> | w_<7>,
        text("1-3", fg_t(thm().bright).with_bold()) | w_<4>, text(":theme") | Fg<120, 120, 140> | w_<7>,
        text("q", fg_t(thm().bright).with_bold()) | w_<2>, text(":quit ") | Fg<120, 120, 140>
    ) | pad<0, 1, 0, 1> | Bg<20, 20, 30>).build();
}

// ── Main ───────────────────────────────────────────────────────────────────

int main() {
    init_state();

    maya::run(
        {.title = "NEXUS://BREACH", .fps = 15, .mode = maya::Mode::Fullscreen},
        [](const maya::Event& ev) {
            if (maya::key(ev, 'q') || maya::key(ev, maya::SpecialKey::Escape))
                return false;
            if (maya::key(ev, '1')) theme_idx = 0;
            if (maya::key(ev, '2')) theme_idx = 1;
            if (maya::key(ev, '3')) theme_idx = 2;
            if (maya::key(ev, ' ') && !breaching) {
                breaching = true;
                breach_timer = 0.3f;
                breach_phase = 0;
                toasts.push("BREACH SEQUENCE INITIATED", maya::ToastLevel::Error);
            }
            if (maya::key(ev, 'e') && !extracting) {
                extracting = true;
                extract_timer = 3.0f;
                add_log(">>> DATA EXTRACTION STARTED <<<", 3);
                toasts.push("EXTRACTING DATA", maya::ToastLevel::Warning);
            }
            if (maya::key(ev, 'c') && !covering) {
                covering = true;
                cover_timer = 2.0f;
                add_log(">>> COVERING TRACKS <<<", 2);
                toasts.push("WIPING EVIDENCE", maya::ToastLevel::Warning);
            }
            return true;
        },
        [] {
            tick(1.0f / 15.0f);

            auto header = build_header();
            auto left = build_targets_panel();
            auto center = build_terminal_panel();
            auto right = build_intel_panel();

            auto main_row = hstack()(
                vstack().grow(1)(std::move(left)),
                vstack().grow(2)(std::move(center)),
                vstack().grow(1)(std::move(right))
            );

            auto footer = build_hex_footer();
            auto toast_elem = toasts.build();

            return vstack()(
                std::move(header),
                std::move(main_row),
                std::move(toast_elem),
                std::move(footer)
            );
        }
    );
}
