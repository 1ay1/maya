// maya — Package install progress (inline, no alt screen)
//
// Simulates a package manager installing dependencies with parallel progress
// bars, spinners, download speeds, and a summary — all inline.
//
// Demonstrates maya for CLI progress indicators without fullscreen takeover.

#include <maya/maya.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef __unix__
#include <sys/ioctl.h>
#include <unistd.h>
#endif

using namespace maya;

// ── Spinner ──────────────────────────────────────────────────────────────────

static const char* spinner(float t) {
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
    float       size_mb;     // download size
    float       speed;       // how fast (0-1 multiplier)
    float       start_delay; // seconds before this package starts
    float       progress;    // 0..1
    bool        done;
    bool        started;
    float       dl_speed;    // current download speed MB/s
};

static Pkg g_pkgs[] = {
    {"react",              "19.1.0",  2.4f,  1.2f, 0.0f, 0, false, false, 0},
    {"typescript",         "5.8.2",   8.1f,  0.9f, 0.1f, 0, false, false, 0},
    {"@maya/core",         "0.3.1",   0.8f,  1.5f, 0.2f, 0, false, false, 0},
    {"vite",               "6.2.0",   4.2f,  1.0f, 0.5f, 0, false, false, 0},
    {"tailwindcss",        "4.1.0",   6.3f,  0.8f, 0.8f, 0, false, false, 0},
    {"@types/node",        "22.14.0", 3.9f,  1.1f, 1.2f, 0, false, false, 0},
    {"eslint",             "9.24.0",  5.5f,  0.7f, 1.6f, 0, false, false, 0},
    {"prettier",           "3.5.3",   1.2f,  1.4f, 2.0f, 0, false, false, 0},
    {"postcss",            "8.5.3",   0.6f,  1.6f, 2.2f, 0, false, false, 0},
    {"autoprefixer",       "10.4.21", 0.3f,  1.8f, 2.4f, 0, false, false, 0},
};
static constexpr int kPkgCount = 10;

static float fsin(float x) noexcept {
    constexpr float tp = 1.f / (2.f * 3.14159265f);
    x *= tp; x -= 0.25f + std::floor(x + 0.25f);
    x *= 16.f * (std::fabs(x) - 0.5f);
    x += 0.225f * x * (std::fabs(x) - 1.f);
    return x;
}

// ── State ────────────────────────────────────────────────────────────────────

struct State {
    float t = 0;
    bool  all_done = false;
    int   done_count = 0;
    float total_mb = 0;
};

static void advance(State& st, float dt) {
    st.t += dt;
    st.done_count = 0;
    st.total_mb = 0;
    st.all_done = true;

    for (int i = 0; i < kPkgCount; ++i) {
        auto& p = g_pkgs[i];
        st.total_mb += p.size_mb;

        if (st.t < p.start_delay) {
            st.all_done = false;
            continue;
        }

        p.started = true;
        if (p.done) {
            st.done_count++;
            continue;
        }

        st.all_done = false;
        float base_speed = p.speed * 0.3f; // progress/sec
        float jitter = 1.f + 0.3f * fsin(st.t * (3.f + i * 0.7f));
        p.progress += base_speed * jitter * dt;
        p.dl_speed = p.size_mb * base_speed * jitter;

        if (p.progress >= 1.f) {
            p.progress = 1.f;
            p.done = true;
            p.dl_speed = 0;
            st.done_count++;
        }
    }
}

// ── Build UI ─────────────────────────────────────────────────────────────────

static constexpr char32_t kBar[] = {
    U'\u2588', U'\u2589', U'\u258a', U'\u258b', U'\u258c', U'\u258d', U'\u258e', U'\u258f'
};

static Element build_ui(const State& st, int w) {
    auto dim   = Style{}.with_fg(Color::rgb(90, 90, 110));
    auto white = Style{}.with_bold().with_fg(Color::rgb(220, 220, 240));
    auto brand = Style{}.with_bold().with_fg(Color::rgb(100, 180, 255));
    auto green = Style{}.with_bold().with_fg(Color::rgb(80, 220, 120));
    auto muted = Style{}.with_fg(Color::rgb(70, 70, 90));
    auto speed_s = Style{}.with_fg(Color::rgb(140, 140, 170));

    std::vector<Element> rows;

    // Header
    rows.push_back(hstack()(
        text("\xe2\x9a\xa1 ", brand),
        text("Installing dependencies...", white)
    ));
    rows.push_back(text(""));

    // Package rows
    int bar_w = w - 48; // space for name + version + pct + speed
    if (bar_w < 10) bar_w = 10;

    for (int i = 0; i < kPkgCount; ++i) {
        const auto& p = g_pkgs[i];

        if (!p.started) {
            rows.push_back(hstack()(
                text("  ", muted),
                text("\xe2\x97\x8b ", muted),  // ○
                text(p.name, muted),
                text(" ", muted),
                text(p.version, muted)
            ));
            continue;
        }

        if (p.done) {
            // Completed: green check
            rows.push_back(hstack()(
                text("  ", dim),
                text("\xe2\x9c\x93 ", green),
                text(p.name, Style{}.with_fg(Color::rgb(180, 180, 200))),
                text(" ", dim),
                text(p.version, dim),
                text("  ", dim),
                text(std::to_string(static_cast<int>(p.size_mb * 10) / 10) + "." +
                     std::to_string(static_cast<int>(p.size_mb * 10) % 10) + " MB", dim)
            ));
        } else {
            // In progress: spinner + bar
            std::string spin = std::string(spinner(st.t + static_cast<float>(i) * 0.3f)) + " ";

            // Build progress bar string
            float pct = p.progress;
            int filled_eighths = static_cast<int>(pct * static_cast<float>(bar_w) * 8.f);
            int full_blocks = filled_eighths / 8;
            int remainder   = filled_eighths % 8;

            // Color: cyan when downloading
            Style bar_fill = Style{}.with_fg(Color::rgb(80, 200, 255));
            Style bar_bg   = Style{}.with_fg(Color::rgb(35, 35, 50));

            // We'll render the bar as text with the right chars
            std::string bar_str;
            for (int b = 0; b < bar_w; ++b) {
                if (b < full_blocks) bar_str += "\xe2\x96\x88";       // █
                else if (b == full_blocks && remainder > 0) {
                    // partial block — use the appropriate character
                    // Block elements go from full (█=8/8) down to thin (▏=1/8)
                    // We want left-aligned fill so use left blocks
                    const char* partial[] = {
                        " ", "\xe2\x96\x8f", "\xe2\x96\x8e", "\xe2\x96\x8d",
                        "\xe2\x96\x8c", "\xe2\x96\x8b", "\xe2\x96\x8a", "\xe2\x96\x89"
                    };
                    bar_str += partial[remainder];
                }
                else bar_str += "\xe2\x96\x91"; // ░
            }

            char pct_buf[8];
            std::snprintf(pct_buf, sizeof pct_buf, "%3.0f%%", static_cast<double>(pct * 100.f));

            char speed_buf[16];
            std::snprintf(speed_buf, sizeof speed_buf, "%4.1f MB/s", static_cast<double>(p.dl_speed));

            // Name column (fixed width)
            char name_buf[24];
            std::snprintf(name_buf, sizeof name_buf, "%-20s", p.name);

            rows.push_back(hstack()(
                text("  ", dim),
                text(spin, Style{}.with_fg(Color::rgb(180, 130, 255))),
                text(name_buf, Style{}.with_fg(Color::rgb(200, 200, 220))),
                text(bar_str, bar_fill),
                text(" ", dim),
                text(pct_buf, Style{}.with_fg(Color::rgb(200, 200, 220))),
                text("  ", dim),
                text(speed_buf, speed_s)
            ));
        }
    }

    rows.push_back(text(""));

    // Overall progress
    char summary[64];
    float total_dl = 0;
    for (int i = 0; i < kPkgCount; ++i)
        total_dl += g_pkgs[i].size_mb * g_pkgs[i].progress;

    if (st.all_done) {
        std::snprintf(summary, sizeof summary,
            "\xe2\x9c\x93 %d packages installed (%.1f MB) in %.1fs",
            kPkgCount, static_cast<double>(st.total_mb), static_cast<double>(st.t));
        rows.push_back(text(summary, green));
    } else {
        std::snprintf(summary, sizeof summary,
            "%s %d/%d  %.1f / %.1f MB",
            spinner(st.t), st.done_count, kPkgCount,
            static_cast<double>(total_dl), static_cast<double>(st.total_mb));
        rows.push_back(text(summary, brand));
    }

    return vstack().width(w)(std::move(rows));
}

// ── Render ───────────────────────────────────────────────────────────────────

static int find_height(const Canvas& c, int w, int h) {
    for (int y = h - 1; y >= 0; --y)
        for (int x = 0; x < w; ++x) {
            auto cell = Cell::unpack(c.cells()[y * w + x]);
            if (cell.character != U' ' && cell.character != 0) return y + 1;
        }
    return 1;
}

static void render_frame(const Element& root, int w, StylePool& pool,
                         std::string& out, int& prev_h) {
    Canvas canvas{w, 30, &pool};
    render_tree(root, canvas, pool, theme::dark);
    int h = find_height(canvas, w, 30);

    Canvas trimmed{w, h, &pool};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            auto cell = Cell::unpack(canvas.cells()[y * w + x]);
            trimmed.set(x, y, cell.character, cell.style_id);
        }

    out.clear();
    if (prev_h > 0) ansi::erase_lines(prev_h, out);
    serialize(trimmed, pool, out);
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
    prev_h = h;
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main() {
    int term_w = 80;
    #ifdef __unix__
    {
        struct winsize ws{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
            term_w = ws.ws_col;
    }
    #endif
    int w = std::min(term_w - 1, 100);
    if (w < 50) w = 50;

    State st;
    StylePool pool;
    std::string out;
    int prev_h = 0;

    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();

    std::fputs("\x1b[?25l", stdout);

    while (!st.all_done) {
        auto now = Clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;

        advance(st, dt);
        Element root = build_ui(st, w);
        render_frame(root, w, pool, out, prev_h);

        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    // Final
    {
        Element root = build_ui(st, w);
        render_frame(root, w, pool, out, prev_h);
    }

    std::fputs("\x1b[?25h\n", stdout);
    std::fflush(stdout);
}
