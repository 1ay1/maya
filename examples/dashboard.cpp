// examples/dashboard.cpp — fake sysmon dashboard
#include <maya/maya.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

using namespace maya;
using namespace std::chrono_literals;

// ── tunables ──────────────────────────────────────────────────────────────────
static constexpr int kBarW  = 16;
static constexpr int kSprkW = 30;
static constexpr int kHistN = 60;
static constexpr int kLogN  = 4;

// ── model ─────────────────────────────────────────────────────────────────────
struct Proc { int pid; std::string name, st; float cpu, mem; };
struct Log  { std::string ts, lv, msg; };
struct Svc  { std::string name; bool up; };

struct Snap {
    float cpu=42, mem=61, disk=34, gpu=56, temp=51;
    float nu=1.4f, nd=4.2f, ld[3]={1.23f, 0.98f, 0.76f};
    std::deque<float> ch, nh;
    std::vector<Proc> procs;
    std::vector<Log>  logs;
    std::vector<Svc>  svcs = {{"nginx",true},{"postgres",true},
                               {"redis",false},{"docker",true},{"cron",true}};
    std::string ts = "--:--:--";
};

static std::mutex        g_mu;
static Snap              g_snap;
static std::atomic<bool> g_stop{false};

// ── utils ─────────────────────────────────────────────────────────────────────

static std::string ts_now() {
    auto t = std::time(nullptr); auto m = *std::localtime(&t);
    char b[16]; std::strftime(b, sizeof b, "%H:%M:%S", &m); return b;
}

static float drift(std::mt19937& r, float v, float lo, float hi, float s) {
    return std::clamp(v + std::uniform_real_distribution<float>{-s, s}(r), lo, hi);
}

static std::string bar(float p, int w) {
    int n = std::clamp(int(p / 100.f * w), 0, w);
    std::string s;
    for (int i = 0; i < n; ++i) s += "█";
    for (int i = n; i < w; ++i) s += "░";
    return s;
}

static std::string spark(const std::deque<float>& h, int w) {
    static const char* B[] = {"▁","▂","▃","▄","▅","▆","▇","█"};
    std::string s;
    int off = h.size() > (size_t)w ? (int)h.size() - w : 0;
    for (int i = off; i < (int)h.size(); ++i)
        s += B[std::clamp(int(h[i] / 100.f * 7), 0, 7)];
    return s;
}

static std::string pf(float v, int d = 1) {
    char b[32]; std::snprintf(b, 32, "%.*f", d, v); return b;
}

static Color pc(float p, const Theme& t) {
    return p >= 80 ? t.error : p >= 60 ? t.warning : t.success;
}

// ── background thread ─────────────────────────────────────────────────────────

static void bg_thread() {
    std::mt19937 rng{std::random_device{}()};
    {
        std::lock_guard lk(g_mu); auto& s = g_snap;
        for (int i = 0; i < kHistN; ++i) {
            s.ch.push_back(35.f + sinf(i * .25f) * 18.f +
                std::uniform_real_distribution<float>{-6, 6}(rng));
            s.nh.push_back(20.f + cosf(i * .18f) * 15.f +
                std::uniform_real_distribution<float>{-5, 5}(rng));
        }
        s.procs = {
            {1337, "chrome",     "R", 23.4f,  8.2f},
            {2048, "node",       "R", 12.1f,  4.1f},
            {3141, "postgres",   "S",  8.7f,  2.9f},
            {4096, "rust-analy", "R",  5.2f,  1.4f},
            {5000, "nvim",       "R",  3.1f,  0.8f},
            {6502, "zsh",        "S",  0.4f,  0.2f},
            {7777, "Xorg",       "R",  2.8f,  3.5f},
            {8080, "nginx",      "S",  1.2f,  0.6f},
        };
        s.logs = {{ts_now(), "INFO", "Dashboard started"}};
        s.ts = ts_now();
    }

    static const char* M[3][4] = {
        {"Heartbeat OK",   "Cache hit 94%",   "Auth renewed",        "Flush done"},
        {"CPU spike 87%",  "Mem pressure",    "Slow query 340ms",    "Pool near limit"},
        {"redis timeout",  "Disk I/O error",  "OOM: worker killed",  "Upstream down"},
    };
    static const char* LV[] = {"INFO", "WARN", "ERROR"};
    std::uniform_int_distribution<int> r10(0, 9), r4(0, 3);

    while (!g_stop) {
        std::this_thread::sleep_for(320ms);
        if (g_stop) break;
        std::lock_guard lk(g_mu); auto& s = g_snap;

        s.cpu  = drift(rng, s.cpu,    5, 95,  8.f);
        s.mem  = drift(rng, s.mem,   30, 90,  2.f);
        s.disk = drift(rng, s.disk,  10, 80,  .5f);
        s.gpu  = drift(rng, s.gpu,    5, 95, 10.f);
        s.temp = drift(rng, s.temp,  35, 90, 1.5f);
        s.nu   = std::max(.1f, drift(rng, s.nu, 0, 20, .4f));
        s.nd   = std::max(.1f, drift(rng, s.nd, 0, 50, 1.2f));
        s.ld[0] = std::max(0.f, drift(rng, s.ld[0], 0, 8, .3f));
        s.ld[1] = s.ld[1] * .9f  + s.ld[0] * .1f;
        s.ld[2] = s.ld[2] * .95f + s.ld[1] * .05f;

        s.ch.push_back(s.cpu);
        s.nh.push_back(s.nd / 50.f * 100.f);
        if ((int)s.ch.size() > kHistN) s.ch.pop_front();
        if ((int)s.nh.size() > kHistN) s.nh.pop_front();

        for (auto& p : s.procs) {
            p.cpu = std::max(0.f, drift(rng, p.cpu, 0, 45, 3.f));
            p.mem = std::max(0.f, drift(rng, p.mem, 0, 12, .4f));
        }
        std::sort(s.procs.begin(), s.procs.end(),
                  [](const Proc& a, const Proc& b) { return a.cpu > b.cpu; });

        int roll = r10(rng), lv = roll < 6 ? 0 : roll < 9 ? 1 : 2;
        s.logs.push_back({ts_now(), LV[lv], M[lv][r4(rng)]});
        if ((int)s.logs.size() > kLogN) s.logs.erase(s.logs.begin());
        s.ts = ts_now();
    }
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    std::thread thr(bg_thread);
    Signal<int> sel{0};

    run(
        {.title = "sysmon · maya"},

        [&](const Event& ev) {
            if (key(ev, 'q') || key(ev, SpecialKey::Escape)) return false;
            if (key(ev, SpecialKey::Down))
                sel.update([](int& n) { n = std::min(n + 1, 7); });
            if (key(ev, SpecialKey::Up))
                sel.update([](int& n) { n = std::max(n - 1, 0); });
            return true;
        },

        [&](const Ctx& ctx) {
            Snap s;
            { std::lock_guard lk(g_mu); s = g_snap; }
            const auto& t = ctx.theme;
            const int   si = sel.get();

            // ── gauge row ──────────────────────────────────────────────────────
            // Fixed 4-char label + [bar] + pct + optional annotation.
            // All elements are on one Row so the bar always aligns.
            auto G = [&](const char* lbl, float p, std::string ann = {}) -> Element {
                char pb[8]; std::snprintf(pb, 8, " %3.0f%%", p);
                return box().direction(Row)(
                    text(lbl,             Style{}.with_fg(t.muted)),
                    text(" [",            dim_style),
                    text(bar(p, kBarW),   Style{}.with_fg(pc(p, t))),
                    text("] ",            dim_style),
                    text(std::string(pb), Style{}.with_bold().with_fg(pc(p, t))),
                    !ann.empty() ? text("  " + ann, Style{}.with_fg(t.muted).with_dim())
                                 : text("")
                );
            };

            // ── header ────────────────────────────────────────────────────────
            auto hdr = box().direction(Row).padding(0, 1)(
                text("◆ SYSMON", Style{}.with_bold().with_fg(t.accent)),
                spacer(),
                text("load " + pf(s.ld[0]) + "  " + pf(s.ld[1]) + "  " + pf(s.ld[2]),
                     Style{}.with_fg(t.muted)),
                text("    " + s.ts, Style{}.with_fg(t.text))
            );

            // ── resources panel ───────────────────────────────────────────────
            // grow(1): this panel and the right panel each take 50% of the row.
            auto res = box()
                .border(BorderStyle::Round).border_color(t.border)
                .border_text(" RESOURCES ")
                .direction(Column).padding(1).gap(1)
                .grow(1)(
                G("CPU ", s.cpu),
                text(spark(s.ch, kSprkW), Style{}.with_fg(pc(s.cpu, t)).with_dim()),
                G("MEM ", s.mem,
                    (pf(s.mem / 100.f * 16.f, 1) + " / 16 GB").c_str()),
                G("DISK", s.disk,
                    (pf(s.disk / 100.f * 500.f, 0) + " / 500 GB").c_str()),
                G("GPU ", s.gpu),
                separator(),
                box().direction(Row)(
                    text("NET  ", Style{}.with_fg(t.muted)),
                    text("↑ " + pf(s.nu) + " MB/s",   Style{}.with_fg(t.success)),
                    text("   ↓ " + pf(s.nd) + " MB/s", Style{}.with_fg(t.info)),
                    spacer(),
                    text(pf(s.temp, 0) + "°C",
                         Style{}.with_fg(s.temp >= 75 ? t.error
                                       : s.temp >= 60 ? t.warning : t.success))
                ),
                text(spark(s.nh, kSprkW), Style{}.with_fg(t.info).with_dim())
            );

            // ── process list ──────────────────────────────────────────────────
            // Each row is two elements: a 2-char indicator + a single snprintf
            // string. snprintf guarantees column alignment across all rows.
            std::vector<Element> prows;
            prows.reserve(s.procs.size());
            for (int i = 0; i < (int)s.procs.size(); ++i) {
                const auto& p = s.procs[i];
                bool hi = (i == si);
                char row[64];
                std::snprintf(row, 64, "%-5d  %-12s  %5.1f%%  %5.1f%%",
                    p.pid, p.name.c_str(), p.cpu, p.mem);
                prows.push_back(box().direction(Row)(
                    hi ? text("▶ ", Style{}.with_fg(t.accent))
                       : text("  ", Style{}.with_fg(t.muted)),
                    text(std::string(row),
                         Style{}.with_fg(hi ? t.accent : pc(p.cpu, t))
                                .with_bold(hi))
                ));
            }

            // grow(1): procs fills remaining vertical space inside right column.
            auto procs = box()
                .border(BorderStyle::Round).border_color(t.border)
                .border_text(" PROCESSES ")
                .direction(Column).padding(1)
                .grow(1)(
                // Header row — same format string layout as data rows.
                text("  PID    NAME            CPU%    MEM%",
                     Style{}.with_bold().with_fg(t.muted)),
                separator(),
                prows
            );

            // ── services panel ────────────────────────────────────────────────
            auto svcs = box()
                .border(BorderStyle::Round).border_color(t.border)
                .border_text(" SERVICES ")
                .direction(Column).padding(1)(
                map_elements(s.svcs, [&](const Svc& sv) -> Element {
                    return box().direction(Row)(
                        text(sv.up ? "● " : "○ ",
                             Style{}.with_fg(sv.up ? t.success : t.error)),
                        text(sv.name, Style{}.with_fg(t.text)),
                        spacer(),
                        text(sv.up ? "active " : "stopped",
                             Style{}.with_fg(sv.up ? t.success : t.muted)
                                    .with_dim())
                    );
                })
            );

            // ── log stream ────────────────────────────────────────────────────
            auto logs = box()
                .border(BorderStyle::Round).border_color(t.border)
                .border_text(" LOG STREAM ")
                .direction(Column).padding(1)(
                map_elements(s.logs, [&](const Log& l) -> Element {
                    Color lc = l.lv == "ERROR" ? t.error
                             : l.lv == "WARN"  ? t.warning
                             : t.muted;
                    char row[96];
                    std::snprintf(row, 96, "[%s]  %-5s  %s",
                        l.ts.c_str(), l.lv.c_str(), l.msg.c_str());
                    return text(std::string(row), Style{}.with_fg(lc));
                })
            );

            // ── footer ────────────────────────────────────────────────────────
            auto ftr = box().direction(Row).padding(0, 1)(
                text("[↑↓] select  [q] quit", dim_style)
            );

            // ── root ──────────────────────────────────────────────────────────
            //
            // Column layout (top to bottom):
            //   hdr  (1 row)
            //   ─── (separator, 1 row)
            //   Row (grow=1 → takes all remaining height)
            //     res         (grow=1 → 50% width, full height)
            //     Column      (grow=1 → 50% width, full height)
            //       procs     (grow=1 → fills height after svcs)
            //       svcs      (content height)
            //   ─── (separator, 1 row)
            //   logs (content height = kLogN rows + border + padding)
            //   ftr  (1 row)
            //
            return box()
                .border(BorderStyle::Round).border_color(t.primary)
                .direction(Column)(
                hdr,
                separator(),
                box().direction(Row).grow(1).gap(1)(
                    res,
                    // right column: grow(1) so it takes same width as res
                    box().direction(Column).grow(1).gap(1)(
                        procs,
                        svcs
                    )
                ),
                separator(),
                logs,
                ftr
            );
        }
    );

    g_stop = true;
    thr.join();
}
