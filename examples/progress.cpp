// maya — Package install progress (inline, no alt screen)
//
// Simulates a package manager installing dependencies with parallel progress
// bars, spinners, download speeds, and a summary.
//
// Uses maya::inline_run() — one function call, no boilerplate.

#include <maya/maya.hpp>

#include <cmath>
#include <cstdio>
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

// ── Spinner ──────────────────────────────────────────────────────────────────

static const char* spin(float t) {
    static const char* f[] = {
        "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
        "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7",
        "\xe2\xa0\x87", "\xe2\xa0\x8f",
    };
    return f[static_cast<int>(t * 8.f) % 10];
}

// ── Package data ─────────────────────────────────────────────────────────────

struct Pkg {
    const char* name;
    const char* version;
    float size_mb, speed, start_delay;
    float progress = 0;
    bool  done = false, started = false;
    float dl_speed = 0;
};

static Pkg g_pkgs[] = {
    {"react",         "19.1.0",  2.4f, 1.2f, 0.0f},
    {"typescript",    "5.8.2",   8.1f, 0.9f, 0.1f},
    {"@maya/core",    "0.3.1",   0.8f, 1.5f, 0.2f},
    {"vite",          "6.2.0",   4.2f, 1.0f, 0.5f},
    {"tailwindcss",   "4.1.0",   6.3f, 0.8f, 0.8f},
    {"@types/node",   "22.14.0", 3.9f, 1.1f, 1.2f},
    {"eslint",        "9.24.0",  5.5f, 0.7f, 1.6f},
    {"prettier",      "3.5.3",   1.2f, 1.4f, 2.0f},
    {"postcss",       "8.5.3",   0.6f, 1.6f, 2.2f},
    {"autoprefixer",  "10.4.21", 0.3f, 1.8f, 2.4f},
};
static constexpr int kN = 10;

// ── State ────────────────────────────────────────────────────────────────────

static float g_time = 0;
static int   g_done = 0;
static float g_total_mb = 0;

static void advance(float dt) {
    g_time += dt;
    g_done = 0;
    g_total_mb = 0;

    for (int i = 0; i < kN; ++i) {
        auto& p = g_pkgs[i];
        g_total_mb += p.size_mb;
        if (g_time < p.start_delay) continue;
        p.started = true;
        if (p.done) { g_done++; continue; }

        float jitter = 1.f + 0.3f * fsin(g_time * (3.f + i * 0.7f));
        float spd = p.speed * 0.3f * jitter;
        p.progress += spd * dt;
        p.dl_speed = p.size_mb * spd;

        if (p.progress >= 1.f) {
            p.progress = 1.f;
            p.done = true;
            p.dl_speed = 0;
            g_done++;
        }
    }

    if (g_done == kN) quit();
}

// ── Styles (compile-time where possible) ─────────────────────────────────────

static const Style sBrand   = Style{}.with_bold().with_fg(Color::rgb(100, 180, 255));
static const Style sWhite   = Style{}.with_bold().with_fg(Color::rgb(220, 220, 240));
static const Style sGreen   = Style{}.with_bold().with_fg(Color::rgb(80, 220, 120));
static const Style sMuted   = Style{}.with_fg(Color::rgb(70, 70, 90));
static const Style sDim     = Style{}.with_fg(Color::rgb(90, 90, 110));
static const Style sName    = Style{}.with_fg(Color::rgb(200, 200, 220));
static const Style sBar     = Style{}.with_fg(Color::rgb(80, 200, 255));
static const Style sBarBg   = Style{}.with_fg(Color::rgb(35, 35, 50));
static const Style sSpin    = Style{}.with_fg(Color::rgb(180, 130, 255));
static const Style sSpeed   = Style{}.with_fg(Color::rgb(140, 140, 170));

// ── Build UI ─────────────────────────────────────────────────────────────────

static Element build_ui() {
    std::vector<Element> rows;

    // Header — static DSL parts mixed with runtime
    rows.push_back(h(
        dyn([] { return text("\xe2\x9a\xa1 ", sBrand); }),
        dyn([] { return text("Installing dependencies...", sWhite); })
    ));
    rows.push_back(text(""));

    for (int i = 0; i < kN; ++i) {
        const auto& p = g_pkgs[i];

        if (!p.started) {
            rows.push_back(h(
                dyn([&p] { return text("  \xe2\x97\x8b ", sMuted); }),
                dyn([&p] { return text(p.name, sMuted); }),
                dyn([&p] { return text(" ", sMuted); }),
                dyn([&p] { return text(p.version, sMuted); })
            ));
            continue;
        }

        if (p.done) {
            char sz[12];
            std::snprintf(sz, sizeof sz, "%.1f MB", static_cast<double>(p.size_mb));
            rows.push_back(h(
                dyn([] { return text("  \xe2\x9c\x93 ", sGreen); }),
                dyn([&p] { return text(p.name, sName); }),
                dyn([&p] { return text(" ", sDim); }),
                dyn([&p] { return text(p.version, sDim); }),
                dyn([] { return text("  ", sDim); }),
                dyn([sz] { return text(sz, sDim); })
            ));
            continue;
        }

        // In-progress: spinner + progress bar + speed
        int bar_w = 24;
        float pct = p.progress;
        int filled_8ths = static_cast<int>(pct * static_cast<float>(bar_w) * 8.f);
        int full = filled_8ths / 8;
        int rem  = filled_8ths % 8;

        const char* partial[] = {
            " ", "\xe2\x96\x8f", "\xe2\x96\x8e", "\xe2\x96\x8d",
            "\xe2\x96\x8c", "\xe2\x96\x8b", "\xe2\x96\x8a", "\xe2\x96\x89"
        };

        std::string bar;
        for (int b = 0; b < bar_w; ++b) {
            if (b < full) bar += "\xe2\x96\x88";
            else if (b == full && rem > 0) bar += partial[rem];
            else bar += "\xe2\x96\x91";
        }

        char name_buf[22];
        std::snprintf(name_buf, sizeof name_buf, "%-20s", p.name);

        char pct_buf[8];
        std::snprintf(pct_buf, sizeof pct_buf, "%3.0f%%", static_cast<double>(pct * 100.f));

        char spd_buf[16];
        std::snprintf(spd_buf, sizeof spd_buf, "%4.1f MB/s", static_cast<double>(p.dl_speed));

        rows.push_back(h(
            dyn([] { return text("  ", sDim); }),
            dyn([i] { return text(std::string(spin(g_time + i * 0.3f)) + " ", sSpin); }),
            dyn([name_buf] { return text(name_buf, sName); }),
            dyn([bar] { return text(bar, sBar); }),
            dyn([] { return text(" ", sDim); }),
            dyn([pct_buf] { return text(pct_buf, sName); }),
            dyn([] { return text("  ", sDim); }),
            dyn([spd_buf] { return text(spd_buf, sSpeed); })
        ));
    }

    rows.push_back(text(""));

    float dl = 0;
    for (int i = 0; i < kN; ++i) dl += g_pkgs[i].size_mb * g_pkgs[i].progress;

    if (g_done == kN) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "\xe2\x9c\x93 %d packages installed (%.1f MB) in %.1fs",
                      kN, static_cast<double>(g_total_mb), static_cast<double>(g_time));
        rows.push_back(text(buf, sGreen));
    } else {
        char buf[48];
        std::snprintf(buf, sizeof buf, "%s %d/%d  %.1f / %.1f MB",
                      spin(g_time), g_done, kN, static_cast<double>(dl),
                      static_cast<double>(g_total_mb));
        rows.push_back(text(buf, sBrand));
    }

    return v(std::move(rows));
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main() {
    inline_run({.fps = 30}, [](float dt) {
        advance(dt);
        return build_ui();
    });
}
