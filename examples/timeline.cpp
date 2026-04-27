// timeline.cpp — EventTimeline hosting mixed widgets, live inline
//
// Demonstrates the EventTimeline widget feeding a Linear/GitHub-PR-style
// activity feed where each entry hosts a different widget:
//   - plain title-only events (the simplest form)
//   - a BashTool card streaming subprocess output line by line
//   - a recursive Investigation tree with concurrent sub-investigations
//   - pending events that transition to active then done
//
// Run with `./build/maya_timeline` from the project root. Press 'q' to quit.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <maya/maya.hpp>
#include <maya/widget/bash_tool.hpp>
#include <maya/widget/event_timeline.hpp>
#include <maya/widget/investigation.hpp>

using namespace maya;
using namespace maya::dsl;
using namespace std::chrono_literals;

namespace {

// Run a shell command; throttle line emission so streaming is visible
// (real-world subprocesses on a small repo finish in ~3ms otherwise).
void run_subprocess(const std::string& cmd,
                    std::function<void(std::string)> on_line,
                    std::function<void(int, float)> on_done,
                    std::chrono::milliseconds       line_delay = 100ms) {
    auto t0 = std::chrono::steady_clock::now();
    FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) { on_done(-1, 0.0f); return; }
    std::vector<std::string> lines;
    char buf[8192];
    while (std::fgets(buf, sizeof(buf), p)) {
        std::string line{buf};
        if (!line.empty() && line.back() == '\n') line.pop_back();
        lines.push_back(std::move(line));
    }
    int rc = ::pclose(p);
    for (auto& line : lines) {
        on_line(std::move(line));
        std::this_thread::sleep_for(line_delay);
    }
    auto t1 = std::chrono::steady_clock::now();
    float elapsed = std::chrono::duration<float>(t1 - t0).count();
    int exit_code = (rc == -1) ? -1 : (WIFEXITED(rc) ? WEXITSTATUS(rc) : 1);
    on_done(exit_code, elapsed);
}

std::string detect_project_root() {
    if (const char* env = std::getenv("MAYA_ROOT")) return env;
    for (auto cur = std::string{"."}; cur.size() < 256; cur += "/..") {
        FILE* probe = std::fopen((cur + "/include/maya/maya.hpp").c_str(), "r");
        if (probe) { std::fclose(probe); return cur; }
    }
    return ".";
}

// Pretty-print elapsed seconds as "120ms" / "1.4s" / "1m12s".
std::string fmt_elapsed(float seconds) {
    char buf[16];
    if (seconds < 1.0f) {
        std::snprintf(buf, sizeof(buf), "%.0fms", static_cast<double>(seconds * 1000.0f));
    } else if (seconds < 60.0f) {
        std::snprintf(buf, sizeof(buf), "%.1fs", static_cast<double>(seconds));
    } else {
        int mins = static_cast<int>(seconds) / 60;
        float secs = seconds - static_cast<float>(mins * 60);
        std::snprintf(buf, sizeof(buf), "%dm%.0fs", mins, static_cast<double>(secs));
    }
    return buf;
}

}  // namespace

// ── Program ──────────────────────────────────────────────────────────────

struct TimelineDemo {
    struct Model {
        EventTimeline                  tl;
        std::unique_ptr<BashTool>      bash;          // event 3 body
        std::unique_ptr<Investigation> investigation; // event 4 body
        std::vector<Investigation*>    sub_ptrs;
        std::vector<std::vector<Investigation::Action*>> action_ptrs;

        // Per-event start time (in seconds since program start). 0 means
        // "not started / no live counter wanted". Used by the Tick handler
        // to refresh elapsed text on still-Active events each frame.
        std::vector<float> event_start;

        std::string root;
        bool        started        = false;
        int         bash_done      = 0;  // 0 pending, 1 done
        int         invest_done    = 0;
        int         tests_idx      = -1; // index of the trailing "Run tests" event
        bool        tests_started  = false;
        bool        tests_done     = false;
        float       elapsed        = 0.0f;
    };

    // ── Messages ──────────────────────────────────────────────────────────
    struct Tick {};
    struct Start {};
    struct Quit {};
    struct BashLine { std::string line; };
    struct BashDone { int rc; float elapsed; };
    struct InvLine { int sub_idx; int step_idx; std::string line; };
    struct InvStepDone { int sub_idx; int step_idx; int rc; float el; };
    struct InvSubDone { int sub_idx; std::string conclusion; };
    struct AllReady {};                  // bash + investigation both done
    struct TestsLine { std::string line; };
    struct TestsDone { int rc; float elapsed; };

    using Msg = std::variant<Tick, Start, Quit,
                             BashLine, BashDone,
                             InvLine, InvStepDone, InvSubDone,
                             AllReady, TestsLine, TestsDone>;

    static auto init() -> std::pair<Model, Cmd<Msg>> {
        Model m;
        m.root = detect_project_root();
        m.bash = std::make_unique<BashTool>(
            "find " + m.root + "/include -name '*.hpp' | wc -l");
        m.bash->set_status(BashStatus::Pending);
        m.bash->set_expanded(true);

        m.investigation = std::make_unique<Investigation>(
            "Map the Maya widget directory");
        m.investigation->set_status(InvestigationStatus::Pending);

        return {std::move(m), Cmd<Msg>::after(50ms, Start{})};
    }

    static auto update(Model m, Msg msg) -> std::pair<Model, Cmd<Msg>> {
        return std::visit(overload{

            // ── Per-frame tick ─────────────────────────────────────────────
            [&](Tick) -> std::pair<Model, Cmd<Msg>> {
                m.tl.advance(0.05f);
                if (m.investigation) m.investigation->advance(0.05f);
                m.elapsed += 0.05f;
                // Refresh live elapsed text on every Active event whose
                // start time we know — gives the UI a ticking counter
                // until the worker reports its final wall time.
                for (std::size_t i = 0; i < m.tl.size() && i < m.event_start.size(); ++i) {
                    if (m.tl.at(i).status == EventStatus::Active
                        && m.event_start[i] > 0.0f) {
                        m.tl.at(i).elapsed = fmt_elapsed(m.elapsed - m.event_start[i]);
                    }
                }
                return {std::move(m), Cmd<Msg>{}};
            },

            // ── Boot: seed history events + spawn concurrent workers ──────
            [&](Start) -> std::pair<Model, Cmd<Msg>> {
                if (m.started) return {std::move(m), Cmd<Msg>{}};
                m.started = true;

                // History events — already complete
                m.tl.add({
                    .title  = "Session opened",
                    .status = EventStatus::Info,
                });
                m.tl.add({
                    .title   = "Loaded workspace at " + m.root,
                    .elapsed = "12ms",
                    .status  = EventStatus::Done,
                });
                m.tl.add({
                    .title   = "Counting C++ headers under include/",
                    .body    = m.bash->build(),
                    .status  = EventStatus::Active,
                });
                // Investigation event — set up the widget here so we can
                // populate sub-investigations and stash pointers for workers.
                {
                    auto& sub1 = m.investigation->add_sub("List widget files");
                    sub1.set_status(InvestigationStatus::Running);
                    auto& a1 = sub1.add_action("ls include/maya/widget/");
                    a1.set_status(FindingStatus::Pending);

                    auto& sub2 = m.investigation->add_sub("Find the largest");
                    sub2.set_status(InvestigationStatus::Running);
                    auto& a2 = sub2.add_action("wc -l … | sort -nr | head -5");
                    a2.set_status(FindingStatus::Pending);

                    m.sub_ptrs    = {&sub1, &sub2};
                    m.action_ptrs = {{&a1}, {&a2}};
                    m.investigation->set_status(InvestigationStatus::Running);
                }
                m.tl.add({
                    .title  = "Investigating widget surface area",
                    .body   = m.investigation->build(),
                    .status = EventStatus::Active,
                });
                // Pending placeholder for the trailing tests step
                m.tests_idx = static_cast<int>(m.tl.size());
                m.tl.add({
                    .title  = "Run a smoke test once both finish",
                    .status = EventStatus::Pending,
                });

                // Per-event start times: 0 for Info/Done/Pending (no live
                // counter), m.elapsed for the two Active events firing now.
                m.event_start = {0.0f, 0.0f, m.elapsed, m.elapsed, 0.0f};

                // Spawn concurrent workers
                std::vector<Cmd<Msg>> cmds;

                // Worker 1: bash (find | wc -l)
                std::string bash_cmd =
                    "find " + m.root + "/include -name '*.hpp' 2>/dev/null"
                    " | sort | head -10";
                cmds.push_back(Cmd<Msg>::task(
                    [bash_cmd](std::function<void(Msg)> dispatch) {
                        run_subprocess(bash_cmd,
                            [&](std::string l) { dispatch(BashLine{std::move(l)}); },
                            [&](int rc, float el) { dispatch(BashDone{rc, el}); });
                    }));

                // Worker 2: investigation sub 0 — `ls`
                std::string ls_cmd =
                    "ls -1 " + m.root + "/include/maya/widget/ 2>/dev/null"
                    " | grep '\\.hpp$' | head -8";
                cmds.push_back(Cmd<Msg>::task(
                    [ls_cmd](std::function<void(Msg)> dispatch) {
                        run_subprocess(ls_cmd,
                            [&](std::string l) {
                                dispatch(InvLine{0, 0, std::move(l)});
                            },
                            [&](int rc, float el) {
                                dispatch(InvStepDone{0, 0, rc, el});
                                dispatch(InvSubDone{0, "Listed " + std::string{"widget files"}});
                            });
                    }));

                // Worker 3: investigation sub 1 — `wc -l | sort -nr | head`
                std::string wc_cmd =
                    "wc -l " + m.root + "/include/maya/widget/*.hpp 2>/dev/null"
                    " | sort -nr | head -6 | tail -5";
                cmds.push_back(Cmd<Msg>::task(
                    [wc_cmd](std::function<void(Msg)> dispatch) {
                        run_subprocess(wc_cmd,
                            [&](std::string l) {
                                dispatch(InvLine{1, 0, std::move(l)});
                            },
                            [&](int rc, float el) {
                                dispatch(InvStepDone{1, 0, rc, el});
                                dispatch(InvSubDone{1, "Largest widgets identified"});
                            });
                    }));

                return {std::move(m), Cmd<Msg>::batch(std::move(cmds))};
            },

            // ── Bash output streaming ─────────────────────────────────────
            [&](BashLine b) -> std::pair<Model, Cmd<Msg>> {
                if (m.bash) {
                    if (m.bash->status() != BashStatus::Running)
                        m.bash->set_status(BashStatus::Running);
                    m.bash->append_output(b.line + "\n");
                    refresh_event_body(m.tl, /*event_idx=*/2, m.bash->build());
                }
                return {std::move(m), Cmd<Msg>{}};
            },
            [&](BashDone d) -> std::pair<Model, Cmd<Msg>> {
                if (m.bash) {
                    m.bash->set_status(d.rc == 0 ? BashStatus::Success : BashStatus::Failed);
                    m.bash->set_elapsed(d.elapsed);
                    refresh_event_body(m.tl, 2, m.bash->build());
                }
                m.tl.at(2).status  = (d.rc == 0) ? EventStatus::Done : EventStatus::Failed;
                m.tl.at(2).elapsed = fmt_elapsed(d.elapsed);
                m.bash_done = 1;
                return maybe_all_ready(std::move(m));
            },

            // ── Investigation streaming ───────────────────────────────────
            [&](InvLine s) -> std::pair<Model, Cmd<Msg>> {
                if (s.sub_idx >= 0 && s.sub_idx < (int)m.action_ptrs.size()
                    && s.step_idx >= 0
                    && s.step_idx < (int)m.action_ptrs[s.sub_idx].size()) {
                    auto* a = m.action_ptrs[s.sub_idx][s.step_idx];
                    if (a->status == FindingStatus::Pending)
                        a->status = FindingStatus::Running;
                    if (!s.line.empty()) a->detail = std::move(s.line);
                    refresh_event_body(m.tl, 3, m.investigation->build());
                }
                return {std::move(m), Cmd<Msg>{}};
            },
            [&](InvStepDone d) -> std::pair<Model, Cmd<Msg>> {
                if (d.sub_idx >= 0 && d.sub_idx < (int)m.action_ptrs.size()
                    && d.step_idx >= 0
                    && d.step_idx < (int)m.action_ptrs[d.sub_idx].size()) {
                    auto* a = m.action_ptrs[d.sub_idx][d.step_idx];
                    a->elapsed = d.el;
                    a->status = (d.rc == 0) ? FindingStatus::Success : FindingStatus::Failed;
                }
                return {std::move(m), Cmd<Msg>{}};
            },
            [&](InvSubDone s) -> std::pair<Model, Cmd<Msg>> {
                if (s.sub_idx >= 0 && s.sub_idx < (int)m.sub_ptrs.size()) {
                    auto* sub = m.sub_ptrs[s.sub_idx];
                    sub->set_status(InvestigationStatus::Complete);
                    sub->set_conclusion(s.conclusion);
                }
                ++m.invest_done;
                if (m.invest_done == (int)m.sub_ptrs.size()) {
                    m.investigation->set_status(InvestigationStatus::Complete);
                    m.investigation->set_conclusion(
                        "All widget surface area mapped.");
                    m.tl.at(3).status  = EventStatus::Done;
                    m.tl.at(3).elapsed = fmt_elapsed(m.elapsed - m.event_start[3]);
                }
                refresh_event_body(m.tl, 3, m.investigation->build());
                return maybe_all_ready(std::move(m));
            },

            // ── Both upstream events done → start the tests step ──────────
            [&](AllReady) -> std::pair<Model, Cmd<Msg>> {
                if (m.tests_started) return {std::move(m), Cmd<Msg>{}};
                m.tests_started = true;

                auto& ev = m.tl.at(m.tests_idx);
                ev.status = EventStatus::Active;
                ev.title  = "Smoke check: ensure libmaya.a is in build/";
                m.event_start[m.tests_idx] = m.elapsed;

                std::string check_cmd =
                    "ls -lh " + m.root + "/build/libmaya.a 2>/dev/null";
                return {std::move(m), Cmd<Msg>::task(
                    [check_cmd](std::function<void(Msg)> dispatch) {
                        run_subprocess(check_cmd,
                            [&](std::string l) { dispatch(TestsLine{std::move(l)}); },
                            [&](int rc, float el) { dispatch(TestsDone{rc, el}); },
                            150ms);
                    })};
            },
            [&](TestsLine t) -> std::pair<Model, Cmd<Msg>> {
                auto& ev = m.tl.at(m.tests_idx);
                ev.body = Element{TextElement{
                    .content = "  $ ls build/libmaya.a\n  " + t.line,
                    .style   = Style{}.with_dim(),
                    .wrap    = TextWrap::Wrap,
                }};
                return {std::move(m), Cmd<Msg>{}};
            },
            [&](TestsDone d) -> std::pair<Model, Cmd<Msg>> {
                auto& ev = m.tl.at(m.tests_idx);
                ev.status  = (d.rc == 0) ? EventStatus::Done : EventStatus::Failed;
                ev.elapsed = fmt_elapsed(d.elapsed);
                m.tests_done = true;
                // Add a final closing event then schedule quit.
                m.tl.add({
                    .title  = (d.rc == 0)
                        ? std::string{"All checks passed — session done."}
                        : std::string{"Smoke check failed."},
                    .status = (d.rc == 0) ? EventStatus::Info : EventStatus::Warning,
                });
                m.event_start.push_back(0.0f);
                return {std::move(m), Cmd<Msg>::after(1500ms, Quit{})};
            },

            [&](Quit) -> std::pair<Model, Cmd<Msg>> {
                return {std::move(m), Cmd<Msg>::quit()};
            },
        }, msg);
    }

    static Element view(const Model& m) {
        return v(
            t<"Maya · Activity Timeline"> | Bold | Fg<100, 180, 255>,
            blank_,
            dyn([&] { return m.tl.build(); }),
            blank_,
            t<"q to quit"> | Dim
        ) | pad<1>;
    }

    static auto subscribe(const Model&) -> Sub<Msg> {
        return Sub<Msg>::batch(
            Sub<Msg>::on_key([](const KeyEvent& k) -> std::optional<Msg> {
                if (key_is(k, 'q')) return Msg{Quit{}};
                return std::nullopt;
            }),
            Sub<Msg>::every(50ms, Msg{Tick{}})
        );
    }

private:
    // Replace the body of an event in-place with a freshly built Element.
    // EventTimeline holds events by value, so rebuilding the embedded
    // widget's element keeps the timeline rendering in sync with the
    // underlying widget state without re-adding the event.
    static void refresh_event_body(EventTimeline& tl, std::size_t idx,
                                   Element new_body) {
        if (idx < tl.size()) tl.at(idx).body = std::move(new_body);
    }

    static auto maybe_all_ready(Model m) -> std::pair<Model, Cmd<Msg>> {
        if (m.bash_done && m.invest_done == (int)m.sub_ptrs.size()
            && !m.tests_started) {
            return {std::move(m), Cmd<Msg>::after(0ms, AllReady{})};
        }
        return {std::move(m), Cmd<Msg>{}};
    }
};

static_assert(Program<TimelineDemo>);

int main() {
    run<TimelineDemo>({
        .title = "timeline",
        .fps   = 20,
        .mode  = Mode::Inline,
    });
}
