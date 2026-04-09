// space.cpp — NASA-style Mission Control Dashboard
//
// An animated spacecraft telemetry display tracking a journey to Mars.
// Features gauges, sparklines, heatmap, line chart, bar chart, crew
// status, subsystem health, random events, and physics simulation.
//
// Controls:
//   q/Esc     quit
//   space     manual thruster burn
//   a         abort sequence
//   d         diagnostics dump
//   1-3       mission phase (launch/transit/orbit)
//
// Usage:  ./maya_space

#include <maya/maya.hpp>
#include <maya/widget/badge.hpp>
#include <maya/widget/bar_chart.hpp>
#include <maya/widget/callout.hpp>
#include <maya/widget/gauge.hpp>
#include <maya/widget/heatmap.hpp>
#include <maya/widget/line_chart.hpp>
#include <maya/widget/sparkline.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace maya::dsl;

// ── Helpers ─────────────────────────────────────────────────────────────────

static std::mt19937 rng{std::random_device{}()};
static float randf(float lo, float hi) {
    return std::uniform_real_distribution<float>(lo, hi)(rng);
}
static int randi(int lo, int hi) {
    return std::uniform_int_distribution<int>(lo, hi)(rng);
}

static maya::Style fg_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return maya::Style{}.with_fg(maya::Color::rgb(r, g, b));
}

static std::string fmt_f(float v, int decimals = 1) {
    char buf[32];
    if (decimals == 0) std::snprintf(buf, sizeof(buf), "%.0f", static_cast<double>(v));
    else if (decimals == 1) std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(v));
    else std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
    return buf;
}

static std::string fmt_time(float total_secs) {
    int h = static_cast<int>(total_secs) / 3600;
    int m = (static_cast<int>(total_secs) % 3600) / 60;
    int s = static_cast<int>(total_secs) % 60;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%03d:%02d:%02d", h, m, s);
    return buf;
}

// ── Spinners ────────────────────────────────────────────────────────────────

static const char* dot_spin(int frame) {
    static const char* frames[] = {"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};
    return frames[frame % 10];
}

// ── Data Model ──────────────────────────────────────────────────────────────

struct CrewMember {
    std::string name;
    std::string role;
    float health;
    float stress;
};

struct Subsystem {
    std::string name;
    int status; // 0=ok, 1=warn, 2=fail
    float uptime;
};

struct LogEntry {
    float timestamp;
    std::string msg;
    int severity; // 0=info, 1=warn, 2=error
};

struct EventToast {
    std::string msg;
    maya::Severity severity;
    float ttl;
};

// ── State ───────────────────────────────────────────────────────────────────

// Telemetry
static float fuel = 0.92f;
static float o2 = 0.97f;
static float power = 0.85f;
static float hull = 1.0f;

// Navigation
static float pos_x = 0, pos_y = 0, pos_z = 0;
static float heading = 47.3f;
static float dist_to_target = 225000000.0f; // km to Mars
static float velocity = 11.2f; // km/s
static float altitude = 400.0f; // km
static float temperature = 22.0f;

// History buffers
static constexpr int HIST_SIZE = 30;
static std::vector<float> vel_hist(HIST_SIZE, 11.2f);
static std::vector<float> alt_hist(HIST_SIZE, 400.0f);
static std::vector<float> temp_hist(HIST_SIZE, 22.0f);
static std::vector<float> traj_hist(HIST_SIZE, 400.0f);

// Heatmap 8x8
static std::vector<std::vector<float>> thermal_grid(8, std::vector<float>(8, 0.3f));

// Crew
static std::array<CrewMember, 4> crew = {{
    {"Cmdr. Chen",   "Commander",     0.95f, 0.12f},
    {"Dr. Okafor",   "Flight Surgeon", 0.88f, 0.18f},
    {"Lt. Vasquez",  "Pilot",         0.92f, 0.15f},
    {"Eng. Petrov",  "Engineer",      0.90f, 0.20f},
}};

// Subsystems
static std::array<Subsystem, 8> subsystems = {{
    {"Main Engine",    0, 1.0f},
    {"Life Support",   0, 1.0f},
    {"Comms Array",    0, 1.0f},
    {"Nav Computer",   0, 1.0f},
    {"Solar Panels",   0, 1.0f},
    {"Thermal Ctrl",   0, 1.0f},
    {"Attitude Ctrl",  0, 1.0f},
    {"Rad Shield",     0, 1.0f},
}};

// Power distribution
static float pwr_engines = 0.35f;
static float pwr_lifesup = 0.25f;
static float pwr_comms   = 0.15f;
static float pwr_nav     = 0.10f;
static float pwr_thermal = 0.10f;
static float pwr_shield  = 0.05f;

// Comm log
static constexpr int MAX_LOG = 6;
static std::vector<LogEntry> comm_log;

// Events / toasts
static std::vector<EventToast> toasts;

// Mission
static int mission_phase = 0; // 0=launch, 1=transit, 2=orbit
static const char* phase_names[] = {"LAUNCH", "TRANSIT", "ORBIT INSERTION"};
static float elapsed = 0;
static int frame_count = 0;
static bool abort_sequence = false;
static float abort_timer = 0;
static bool diag_mode = false;
static float diag_timer = 0;

// ── Log messages ────────────────────────────────────────────────────────────

static const std::array<std::string, 12> log_msgs = {
    "CAPCOM: telemetry nominal, all stations go",
    "NAV: trajectory correction delta-v computed",
    "EECOM: power bus voltage within limits",
    "FLIGHT: GO for next burn window",
    "GNC: attitude stable, drift < 0.01 deg/s",
    "FIDO: orbit parameters confirmed",
    "CAPCOM: crew health check satisfactory",
    "RETRO: abort window T+04:32 to T+06:15",
    "INCO: high-gain antenna locked on DSN",
    "SURGEON: crew vitals nominal",
    "BOOSTER: stage separation confirmed",
    "TELMU: thermal margins acceptable",
};

// ── Init ────────────────────────────────────────────────────────────────────

static void init_state() {
    for (int i = 0; i < HIST_SIZE; ++i) {
        vel_hist[static_cast<size_t>(i)] = 11.2f + randf(-0.3f, 0.3f);
        alt_hist[static_cast<size_t>(i)] = 400.0f + randf(-10, 10);
        temp_hist[static_cast<size_t>(i)] = 22.0f + randf(-2, 2);
        traj_hist[static_cast<size_t>(i)] = 400.0f + static_cast<float>(i) * 5.0f + randf(-5, 5);
    }

    // Init thermal grid
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c)
            thermal_grid[static_cast<size_t>(r)][static_cast<size_t>(c)] = randf(0.15f, 0.45f);
}

// ── Tick ────────────────────────────────────────────────────────────────────

static void tick(float dt) {
    elapsed += dt;
    frame_count++;

    // Abort countdown
    if (abort_sequence) {
        abort_timer -= dt;
        if (abort_timer <= 0) abort_sequence = false;
    }
    if (diag_mode) {
        diag_timer -= dt;
        if (diag_timer <= 0) diag_mode = false;
    }

    // Toast decay
    for (auto& t : toasts) t.ttl -= dt;
    std::erase_if(toasts, [](const EventToast& t) { return t.ttl <= 0; });

    // Fuel depletes slowly
    float fuel_rate = (mission_phase == 0) ? 0.0008f : 0.0002f;
    fuel -= fuel_rate * dt;
    fuel = std::clamp(fuel, 0.0f, 1.0f);

    // O2 fluctuates
    o2 += randf(-0.002f, 0.001f) * dt;
    o2 = std::clamp(o2, 0.3f, 1.0f);

    // Power varies
    power += randf(-0.003f, 0.003f) * dt;
    power = std::clamp(power, 0.2f, 1.0f);

    // Hull stays high unless event
    hull += randf(-0.0001f, 0.00005f) * dt;
    hull = std::clamp(hull, 0.5f, 1.0f);

    // Velocity evolves
    float vel_drift = (mission_phase == 0) ? 0.05f : -0.01f;
    velocity += vel_drift * dt + randf(-0.1f, 0.1f) * dt;
    velocity = std::clamp(velocity, 3.0f, 30.0f);

    // Altitude
    float alt_rate = (mission_phase == 0) ? 15.0f : (mission_phase == 2 ? -2.0f : 5.0f);
    altitude += alt_rate * dt + randf(-2, 2) * dt;
    altitude = std::max(100.0f, altitude);

    // Temperature — sun exposure cycle
    float sun_angle = std::sin(elapsed * 0.1f);
    temperature = 20.0f + sun_angle * 15.0f + randf(-0.5f, 0.5f);

    // Navigation
    pos_x += velocity * 0.7f * dt;
    pos_y += velocity * 0.3f * dt + randf(-0.1f, 0.1f);
    pos_z += randf(-0.05f, 0.05f);
    heading += randf(-0.2f, 0.2f);
    heading = std::fmod(heading + 360.0f, 360.0f);
    dist_to_target -= velocity * dt;
    dist_to_target = std::max(0.0f, dist_to_target);

    // Update histories
    auto push_hist = [](std::vector<float>& h, float v) {
        h.erase(h.begin());
        h.push_back(v);
    };
    push_hist(vel_hist, velocity);
    push_hist(alt_hist, altitude);
    push_hist(temp_hist, temperature);
    push_hist(traj_hist, altitude);

    // Animate thermal grid
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) {
            float& cell = thermal_grid[static_cast<size_t>(r)][static_cast<size_t>(c)];
            // Sun-facing side (top rows) hotter
            float base = static_cast<float>(8 - r) / 8.0f * 0.3f;
            float sun = (sun_angle + 1.0f) / 2.0f * 0.3f;
            cell += (base + sun + randf(-0.05f, 0.05f) - cell) * 0.15f;
            cell = std::clamp(cell, 0.0f, 1.0f);
        }
    }

    // Crew health drift
    for (auto& c : crew) {
        c.health += randf(-0.002f, 0.001f) * dt;
        c.health = std::clamp(c.health, 0.4f, 1.0f);
        c.stress += randf(-0.003f, 0.004f) * dt;
        c.stress = std::clamp(c.stress, 0.0f, 0.8f);
    }

    // Subsystem status jitter
    for (auto& s : subsystems) {
        s.uptime += dt;
        if (randi(0, 500) == 0) {
            s.status = randi(0, 1); // occasional warning
        } else if (randi(0, 2000) == 0) {
            s.status = 2; // rare failure
        }
        if (s.status > 0 && randi(0, 100) == 0) {
            s.status = 0; // auto-recovery
        }
    }

    // Power distribution jitter
    pwr_engines += randf(-0.01f, 0.01f); pwr_engines = std::clamp(pwr_engines, 0.1f, 0.5f);
    pwr_lifesup += randf(-0.005f, 0.005f); pwr_lifesup = std::clamp(pwr_lifesup, 0.15f, 0.35f);
    pwr_comms   += randf(-0.005f, 0.005f); pwr_comms = std::clamp(pwr_comms, 0.05f, 0.25f);

    // Comm log
    if (randi(0, 15) == 0) {
        int lvl = (randi(0, 10) < 7) ? 0 : (randi(0, 3) == 0 ? 2 : 1);
        comm_log.push_back({elapsed, log_msgs[static_cast<size_t>(randi(0, 11))], lvl});
        if (comm_log.size() > MAX_LOG)
            comm_log.erase(comm_log.begin());
    }

    // Random events
    if (randi(0, 200) == 0) {
        static const std::array<std::pair<std::string, maya::Severity>, 6> events = {{
            {"Micrometeorite detected — hull scan initiated", maya::Severity::Warning},
            {"Solar flare warning — radiation spike", maya::Severity::Error},
            {"Comm signal degraded — switching to backup", maya::Severity::Warning},
            {"Course correction burn completed", maya::Severity::Success},
            {"Deep Space Network handover complete", maya::Severity::Info},
            {"Thermal anomaly detected in module B4", maya::Severity::Warning},
        }};
        auto& ev = events[static_cast<size_t>(randi(0, 5))];
        toasts.push_back({ev.first, ev.second, 5.0f});

        // Effects
        if (ev.second == maya::Severity::Error) {
            hull -= 0.02f;
            hull = std::max(0.5f, hull);
        }
        if (ev.second == maya::Severity::Warning && randi(0, 2) == 0) {
            subsystems[static_cast<size_t>(randi(0, 7))].status = 1;
        }
    }
}

// ── UI Builders ─────────────────────────────────────────────────────────────

static auto panel(std::string label) {
    return vstack().border(maya::BorderStyle::Round)
        .border_color(maya::Color::rgb(40, 45, 60))
        .border_text(std::move(label), maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1);
}

static maya::Style status_color(float v) {
    if (v > 0.7f) return fg_rgb(0, 255, 136);
    if (v > 0.4f) return fg_rgb(255, 200, 60);
    return fg_rgb(255, 60, 60);
}

static maya::Style severity_color(int lvl) {
    if (lvl == 2) return fg_rgb(255, 60, 60);
    if (lvl == 1) return fg_rgb(255, 200, 60);
    return fg_rgb(80, 80, 100);
}

static maya::Element build_header() {
    auto spin = std::string(dot_spin(frame_count));
    std::string met = fmt_time(elapsed);

    std::string phase_str = phase_names[mission_phase];
    auto phase_sty = (mission_phase == 0) ? fg_rgb(255, 140, 50) :
                     (mission_phase == 1) ? fg_rgb(100, 180, 255) :
                                            fg_rgb(0, 255, 136);

    std::string abort_str;
    if (abort_sequence) {
        abort_str = " ABORT T-" + fmt_f(abort_timer, 0) + "s";
    }

    return (h(
        text(spin) | Fg<0, 180, 255>,
        t<" MISSION CONTROL"> | Bold | Fg<0, 180, 255>,
        t<" \xe2\x94\x80 "> | Dim,
        t<"ARES VII"> | Bold | Fg<255, 255, 255>,
        space,
        text("MET " + met) | Fg<100, 180, 255>,
        text("  PHASE: ") | Dim,
        text(phase_str, phase_sty.with_bold()),
        text(abort_str, fg_rgb(255, 50, 50).with_bold()),
        text(diag_mode ? "  [DIAG]" : "") | Fg<255, 200, 60>
    ) | pad<0, 1, 0, 1>).build();
}

static maya::Element build_telemetry_panel() {
    // Four gauges side by side
    maya::Gauge g_fuel(fuel, "FUEL", maya::Color::rgb(0, 255, 136));
    maya::Gauge g_o2(o2, "O2", maya::Color::rgb(100, 180, 255));
    maya::Gauge g_pwr(power, "POWER", maya::Color::rgb(255, 200, 60));
    maya::Gauge g_hull(hull, "HULL", maya::Color::rgb(198, 160, 246));

    return panel(" TELEMETRY ")(
        (h(
            v(g_fuel.build()) | grow_<1>,
            v(g_o2.build()) | grow_<1>,
            v(g_pwr.build()) | grow_<1>,
            v(g_hull.build()) | grow_<1>
        ) | gap_<1>).build()
    );
}

static maya::Element build_sparklines_panel() {
    maya::Sparkline sp_vel(vel_hist, {.color = maya::Color::rgb(0, 255, 200)});
    sp_vel.set_label("VEL");
    sp_vel.set_show_min_max(true);

    maya::Sparkline sp_alt(alt_hist, {.color = maya::Color::rgb(100, 180, 255)});
    sp_alt.set_label("ALT");
    sp_alt.set_show_min_max(true);

    maya::Sparkline sp_temp(temp_hist, {.color = maya::Color::rgb(255, 140, 50)});
    sp_temp.set_label("TMP");
    sp_temp.set_show_min_max(true);

    return panel(" SENSORS ")(
        sp_vel.build(),
        sp_alt.build(),
        sp_temp.build()
    );
}

static maya::Element build_nav_panel() {
    char coord_buf[64];
    std::snprintf(coord_buf, sizeof(coord_buf), "%.1f, %.1f, %.1f",
        static_cast<double>(pos_x), static_cast<double>(pos_y), static_cast<double>(pos_z));

    auto dist_str = fmt_f(dist_to_target / 1000000.0f, 1) + "M km";

    return panel(" NAVIGATION ")(
        (h(
            t<"COORD"> | Fg<100, 180, 255> | w_<7>,
            text(std::string(coord_buf)) | Dim
        )).build(),
        (h(
            t<"HDG"> | Fg<100, 180, 255> | w_<7>,
            text(fmt_f(heading, 1) + "\xc2\xb0") | Dim
        )).build(),
        (h(
            t<"VEL"> | Fg<100, 180, 255> | w_<7>,
            text(fmt_f(velocity, 2) + " km/s", status_color(velocity / 15.0f))
        )).build(),
        (h(
            t<"DIST"> | Fg<100, 180, 255> | w_<7>,
            text(dist_str) | Fg<255, 200, 60>
        )).build()
    );
}

static maya::Element build_heatmap_panel() {
    maya::Heatmap hm(thermal_grid);
    hm.set_low_color(maya::Color::rgb(20, 20, 80));
    hm.set_high_color(maya::Color::rgb(255, 80, 30));
    hm.set_y_labels({"F1","F2","F3","F4","A1","A2","A3","A4"});

    return panel(" THERMAL SIGNATURE ")(
        hm.build()
    );
}

static maya::Element build_trajectory_panel() {
    maya::LineChart chart(traj_hist, 6);
    chart.set_label("Altitude (km)");
    chart.set_color(maya::Color::rgb(0, 200, 255));

    return panel(" TRAJECTORY ")(
        chart.build()
    );
}

static maya::Element build_comm_log_panel() {
    std::vector<maya::Element> rows;

    for (auto& e : comm_log) {
        int mins = static_cast<int>(e.timestamp) / 60;
        int secs = static_cast<int>(e.timestamp) % 60;
        char ts[16];
        std::snprintf(ts, sizeof(ts), "T+%02d:%02d", mins, secs);

        auto level_tag = (e.severity == 2) ? "ERR " : (e.severity == 1 ? "WARN" : "INFO");

        rows.push_back((h(
            text(std::string(ts)) | Dim | w_<7>,
            text(std::string(level_tag), severity_color(e.severity).with_bold()) | w_<5>,
            text(e.msg) | Dim | clip
        )).build());
    }

    while (rows.size() < MAX_LOG)
        rows.push_back(text("").build());

    return panel(" COMM LOG ")(std::move(rows));
}

static maya::Element build_crew_panel() {
    std::vector<maya::Element> rows;

    for (auto& c : crew) {
        auto health_pct = static_cast<int>(c.health * 100);
        auto badge = (c.role == "Commander") ? maya::Badge::info(c.role) :
                     (c.role == "Pilot") ? maya::Badge::warning(c.role) :
                     (c.role == "Flight Surgeon") ? maya::Badge::success(c.role) :
                     maya::Badge::tool(c.role);

        // Health bar
        std::string bar;
        int filled = static_cast<int>(c.health * 10);
        for (int i = 0; i < filled; ++i) bar += "\xe2\x96\x88";
        for (int i = filled; i < 10; ++i) bar += "\xe2\x94\x80";

        rows.push_back((h(
            text(c.name, fg_rgb(200, 200, 220).with_bold()) | w_<15>,
            v(badge.build()) | w_<18>,
            text(bar, status_color(c.health)) | w_<10>,
            text(std::to_string(health_pct) + "%") | Dim
        ) | gap_<1>).build());
    }

    return panel(" CREW STATUS ")(std::move(rows));
}

static maya::Element build_subsystems_panel() {
    std::vector<maya::Element> rows;

    for (auto& s : subsystems) {
        auto icon = (s.status == 0) ? "\xe2\x9c\x93" : (s.status == 1 ? "\xe2\x9a\xa0" : "\xe2\x9c\x97");
        auto sty = (s.status == 0) ? fg_rgb(0, 255, 136) :
                   (s.status == 1) ? fg_rgb(255, 200, 60) :
                                     fg_rgb(255, 60, 60);

        rows.push_back((h(
            text(std::string(icon), sty) | w_<3>,
            text(s.name) | Dim | w_<16>,
            text(s.status == 0 ? "NOMINAL" : (s.status == 1 ? "CAUTION" : "OFFLINE"), sty)
        )).build());
    }

    return panel(" SUBSYSTEMS ")(std::move(rows));
}

static maya::Element build_power_panel() {
    maya::BarChart chart({
        {"Engines",   pwr_engines, maya::Color::rgb(255, 140, 50)},
        {"Life Sup",  pwr_lifesup, maya::Color::rgb(0, 255, 136)},
        {"Comms",     pwr_comms,   maya::Color::rgb(100, 180, 255)},
        {"Nav",       pwr_nav,     maya::Color::rgb(198, 160, 246)},
        {"Thermal",   pwr_thermal, maya::Color::rgb(255, 200, 60)},
        {"Shielding", pwr_shield,  maya::Color::rgb(255, 80, 100)},
    }, 0.5f);

    return panel(" POWER DIST ")(
        chart.build()
    );
}

static maya::Element build_toasts() {
    if (toasts.empty()) return text("").build();

    std::vector<maya::Element> elems;
    for (auto& t : toasts) {
        elems.push_back(maya::Callout(t.severity, t.msg).build());
    }
    return v(std::move(elems)).build();
}

static maya::Element build_status_bar() {
    // Count subsystem issues
    int warnings = 0, errors = 0;
    for (auto& s : subsystems) {
        if (s.status == 1) warnings++;
        if (s.status == 2) errors++;
    }

    auto overall = (errors > 0) ? "RED" : (warnings > 0 ? "YELLOW" : "GREEN");
    auto overall_sty = (errors > 0) ? fg_rgb(255, 60, 60) :
                       (warnings > 0) ? fg_rgb(255, 200, 60) :
                                        fg_rgb(0, 255, 136);

    return (h(
        text(" STATUS: ") | Fg<140, 140, 160>,
        text(std::string(overall), overall_sty.with_bold()),
        text("  FUEL:" + std::to_string(static_cast<int>(fuel * 100)) + "%",
             status_color(fuel)),
        space,
        text(" \xe2\x90\xa3") | Bold | Fg<180, 220, 255>, text(":burn") | Fg<120, 120, 140>,
        text(" a") | Bold | Fg<180, 220, 255>, text(":abort") | Fg<120, 120, 140>,
        text(" d") | Bold | Fg<180, 220, 255>, text(":diag") | Fg<120, 120, 140>,
        text(" 1-3") | Bold | Fg<180, 220, 255>, text(":phase") | Fg<120, 120, 140>,
        text(" q") | Bold | Fg<180, 220, 255>, text(":quit ") | Fg<120, 120, 140>
    ) | pad<0, 1, 0, 1> | Bg<30, 30, 42>).build();
}

// ── Render ──────────────────────────────────────────────────────────────────

static maya::Element render() {
    tick(1.0f / 15.0f);

    // Left column: telemetry, sparklines, nav
    auto left = (v(
        build_telemetry_panel(),
        build_sparklines_panel(),
        build_nav_panel()
    ) | grow_<1>).build();

    // Center column: heatmap, trajectory, comm log
    auto center = (v(
        build_heatmap_panel(),
        build_trajectory_panel(),
        build_comm_log_panel()
    ) | grow_<1>).build();

    // Right column: crew, subsystems, power
    auto right = (v(
        build_crew_panel(),
        build_subsystems_panel(),
        build_power_panel()
    ) | grow_<1>).build();

    auto main_area = (h(
        std::move(left),
        std::move(center),
        std::move(right)
    ) | gap_<1>).build();

    // Build final layout
    std::vector<maya::Element> layout;
    layout.push_back(build_header());
    layout.push_back(std::move(main_area));

    // Toasts overlay
    if (!toasts.empty())
        layout.push_back(build_toasts());

    layout.push_back(build_status_bar());

    return vstack()(std::move(layout));
}

// ── Main ────────────────────────────────────────────────────────────────────

int main() {
    init_state();

    maya::run(
        {.title = "ARES VII Mission Control", .fps = 15, .mode = maya::Mode::Fullscreen},
        [](const maya::Event& ev) {
            maya::on(ev, 'q', [] { maya::quit(); });
            maya::on(ev, maya::SpecialKey::Escape, [] { maya::quit(); });

            // Thruster burn
            maya::on(ev, ' ', [] {
                if (fuel > 0.05f) {
                    fuel -= 0.03f;
                    velocity += 1.5f;
                    toasts.push_back({"Manual thruster burn executed — delta-v +1.5 km/s",
                                      maya::Severity::Success, 3.0f});
                    comm_log.push_back({elapsed, "FLIGHT: Manual burn confirmed, delta-v applied", 0});
                    if (comm_log.size() > MAX_LOG)
                        comm_log.erase(comm_log.begin());
                } else {
                    toasts.push_back({"FUEL CRITICAL — burn denied",
                                      maya::Severity::Error, 4.0f});
                }
            });

            // Abort
            maya::on(ev, 'a', [] {
                if (!abort_sequence) {
                    abort_sequence = true;
                    abort_timer = 10.0f;
                    toasts.push_back({"ABORT SEQUENCE INITIATED — T-10s",
                                      maya::Severity::Error, 5.0f});
                    comm_log.push_back({elapsed, "FLIGHT: ABORT ABORT ABORT — all stations standby", 2});
                    if (comm_log.size() > MAX_LOG)
                        comm_log.erase(comm_log.begin());
                    // Stress crew
                    for (auto& c : crew) c.stress += 0.15f;
                } else {
                    abort_sequence = false;
                    toasts.push_back({"Abort sequence cancelled",
                                      maya::Severity::Info, 3.0f});
                }
            });

            // Diagnostics
            maya::on(ev, 'd', [] {
                diag_mode = !diag_mode;
                diag_timer = 5.0f;
                if (diag_mode) {
                    toasts.push_back({"Diagnostics scan in progress...",
                                      maya::Severity::Info, 3.0f});
                    // Fix a random subsystem
                    for (auto& s : subsystems) {
                        if (s.status > 0) { s.status = 0; break; }
                    }
                }
            });

            // Mission phases
            maya::on(ev, '1', [] {
                mission_phase = 0;
                toasts.push_back({"Phase set: LAUNCH", maya::Severity::Info, 2.0f});
            });
            maya::on(ev, '2', [] {
                mission_phase = 1;
                toasts.push_back({"Phase set: TRANSIT", maya::Severity::Info, 2.0f});
            });
            maya::on(ev, '3', [] {
                mission_phase = 2;
                toasts.push_back({"Phase set: ORBIT INSERTION", maya::Severity::Info, 2.0f});
            });
        },
        [] { return render(); }
    );

    return 0;
}
