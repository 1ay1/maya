// investigate.cpp — Concurrent investigation widget demo
//
// Spawns three sub-investigations of the Maya codebase in parallel. Each
// sub runs real shell subprocesses (ls, wc, grep) on its own worker thread
// from the Cmd::task pool, streaming output line-by-line back into the
// Investigation widget through dispatched messages.
//
// One Tick subscription advances every spinner in the tree in lockstep —
// the widget recurses advance() through nested sub-investigations so the
// caller never wires per-node timers.
//
// Run from the project root so the relative paths resolve:
//     ./build/maya_investigate
//
// Press 'q' to quit.

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
#include <maya/widget/investigation.hpp>

using namespace maya;
using namespace maya::dsl;
using namespace std::chrono_literals;

namespace {

// Run a shell command, invoke `on_line` for each output line, then `on_done`
// with (exit_code, elapsed_seconds). Blocks the calling thread — meant to
// run inside a Cmd::task worker, never on the main render thread.
//
// `line_delay` artificially throttles emission so the user can perceive
// the streaming. Real-world subprocess output (compilers, test runners)
// already arrives gradually; ls/wc/grep on a small repo finish in ~3ms,
// which would render as an instantaneous Pending→Success transition.
// The throttle restores the visual feedback you'd get from a slower tool.
void run_subprocess(const std::string& cmd,
                    std::function<void(std::string)> on_line,
                    std::function<void(int, float)> on_done,
                    std::chrono::milliseconds       line_delay = 120ms) {
    auto t0 = std::chrono::steady_clock::now();

    // Drain the subprocess into memory first so the artificial throttle
    // doesn't keep the pipe buffer (and the child process) alive longer
    // than necessary.
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

    // Now stream the captured lines back at a paced cadence so each one
    // is visibly the "current" line in the widget for a moment.
    for (auto& line : lines) {
        on_line(std::move(line));
        std::this_thread::sleep_for(line_delay);
    }

    auto t1 = std::chrono::steady_clock::now();
    float elapsed = std::chrono::duration<float>(t1 - t0).count();
    int exit_code = (rc == -1) ? -1 : (WIFEXITED(rc) ? WEXITSTATUS(rc) : 1);
    on_done(exit_code, elapsed);
}

// Detect the project root by walking up from cwd looking for include/maya/.
// Falls back to "." if nothing matches — example still runs, just with
// empty subprocess output.
std::string detect_project_root() {
    if (const char* env = std::getenv("MAYA_ROOT")) return env;
    for (auto cur = std::string{"."}; cur.size() < 256;
         cur += "/..") {
        FILE* probe = std::fopen((cur + "/include/maya/maya.hpp").c_str(), "r");
        if (probe) { std::fclose(probe); return cur; }
    }
    return ".";
}

}  // namespace

// ── Program ──────────────────────────────────────────────────────────────

struct Investigate {
    struct Model {
        // Held by unique_ptr so cached pointers into the tree (subs/actions
        // below) survive Model moves between update() returns. The
        // Investigation widget's internal storage (deque + unique_ptr<Sub>)
        // already guarantees per-element stability — we just need a stable
        // top-level handle.
        std::unique_ptr<Investigation> tree;

        // Cached pointers into tree, populated in Start. Workers post
        // (sub_idx, step_idx) and update() looks up the target node here.
        std::vector<Investigation*>                       subs;
        std::vector<std::vector<Investigation::Action*>>  actions;

        std::string root_path;
        bool        started   = false;
        int         completed_subs = 0;
        float       total_elapsed  = 0.0f;
    };

    // ── Messages ──────────────────────────────────────────────────────────
    struct Tick {};
    struct Start {};
    struct Quit  {};
    struct StepLine { int sub_idx; int step_idx; std::string line; };
    struct StepDone { int sub_idx; int step_idx; int exit_code; float elapsed; };
    struct SubDone  { int sub_idx; std::string conclusion; };
    struct AllDone  { std::string conclusion; };

    using Msg = std::variant<Tick, Start, Quit,
                             StepLine, StepDone, SubDone, AllDone>;

    // ── Investigation script (declarative) ────────────────────────────────
    struct StepDef { std::string label; std::string cmd; };
    struct SubDef  { std::string question;
                     std::vector<StepDef> steps;
                     std::string conclusion; };

    static std::vector<SubDef> sub_defs(const std::string& root) {
        return {
            { "How many widget headers exist?",
              {
                  { "ls include/maya/widget/*.hpp",
                    "ls -1 " + root + "/include/maya/widget/ 2>/dev/null"
                    " | grep '\\.hpp$' | head -12" },
                  { "wc -l (count headers)",
                    "ls -1 " + root + "/include/maya/widget/*.hpp 2>/dev/null"
                    " | wc -l | tr -d ' '" },
              },
              "Counted widget headers under include/maya/widget/." },

            { "Which widgets are the largest?",
              {
                  { "wc -l include/maya/widget/*.hpp | sort -nr | head -8",
                    "wc -l " + root + "/include/maya/widget/*.hpp 2>/dev/null"
                    " | sort -nr | head -9 | tail -8" },
              },
              "Top widgets by line count identified." },

            { "Which maya:: types do examples use?",
              {
                  { "grep -ohE 'maya::[A-Z]\\w+' examples/*.cpp",
                    "grep -ohE 'maya::[A-Z][A-Za-z_]+' "
                    + root + "/examples/*.cpp 2>/dev/null"
                    " | sort -u | head -12" },
              },
              "Public types referenced across examples." },
        };
    }

    static auto init() -> std::pair<Model, Cmd<Msg>> {
        Model m;
        m.root_path = detect_project_root();
        m.tree = std::make_unique<Investigation>(
            "What does the Maya widget system look like?");
        m.tree->set_status(InvestigationStatus::Pending);
        return {std::move(m), Cmd<Msg>::after(50ms, Start{})};
    }

    static auto update(Model m, Msg msg) -> std::pair<Model, Cmd<Msg>> {
        return std::visit(overload{
            [&](Tick) -> std::pair<Model, Cmd<Msg>> {
                m.tree->advance(0.05f);
                if (m.tree->status() == InvestigationStatus::Running) {
                    m.total_elapsed += 0.05f;
                    m.tree->set_elapsed(m.total_elapsed);
                    // Bump elapsed on still-running sub headers so the time
                    // ticks visibly while we wait. Final elapsed comes from
                    // the worker via SubDone (per-step) and the sum here.
                    for (auto* sub : m.subs) {
                        if (sub->status() == InvestigationStatus::Running) {
                            sub->set_elapsed(m.total_elapsed);
                        }
                    }
                }
                return {std::move(m), Cmd<Msg>{}};
            },

            [&](Start) -> std::pair<Model, Cmd<Msg>> {
                if (m.started) return {std::move(m), Cmd<Msg>{}};
                m.started = true;
                m.tree->set_status(InvestigationStatus::Running);

                auto defs = sub_defs(m.root_path);
                std::vector<Cmd<Msg>> cmds;
                for (size_t si = 0; si < defs.size(); ++si) {
                    auto& sub = m.tree->add_sub(defs[si].question);
                    sub.set_status(InvestigationStatus::Running);
                    sub.set_pid(1000 + static_cast<int>(si));
                    m.subs.push_back(&sub);
                    m.actions.emplace_back();
                    for (auto const& st : defs[si].steps) {
                        auto& act = sub.add_action(st.label);
                        act.set_status(FindingStatus::Pending);
                        m.actions.back().push_back(&act);
                    }

                    // One worker per sub. Steps within a sub run sequentially;
                    // subs run concurrently because each is its own task on
                    // the runtime's worker pool.
                    int sub_idx = static_cast<int>(si);
                    auto steps  = defs[si].steps;
                    auto concl  = defs[si].conclusion;
                    cmds.push_back(Cmd<Msg>::task(
                        [sub_idx, steps = std::move(steps),
                         concl = std::move(concl)]
                        (std::function<void(Msg)> dispatch) {
                            for (size_t i = 0; i < steps.size(); ++i) {
                                int step_idx = static_cast<int>(i);
                                run_subprocess(
                                    steps[i].cmd,
                                    [&dispatch, sub_idx, step_idx]
                                    (std::string line) {
                                        dispatch(StepLine{sub_idx, step_idx,
                                                          std::move(line)});
                                    },
                                    [&dispatch, sub_idx, step_idx]
                                    (int rc, float el) {
                                        dispatch(StepDone{sub_idx, step_idx,
                                                          rc, el});
                                    });
                            }
                            dispatch(SubDone{sub_idx, concl});
                        }));
                }
                return {std::move(m), Cmd<Msg>::batch(std::move(cmds))};
            },

            [&](StepLine s) -> std::pair<Model, Cmd<Msg>> {
                if (s.sub_idx >= 0 && s.sub_idx < (int)m.actions.size()
                    && s.step_idx >= 0
                    && s.step_idx < (int)m.actions[s.sub_idx].size()) {
                    auto* act = m.actions[s.sub_idx][s.step_idx];
                    if (act->status == FindingStatus::Pending)
                        act->status = FindingStatus::Running;
                    if (!s.line.empty())
                        act->detail = std::move(s.line);
                }
                return {std::move(m), Cmd<Msg>{}};
            },

            [&](StepDone d) -> std::pair<Model, Cmd<Msg>> {
                if (d.sub_idx >= 0 && d.sub_idx < (int)m.actions.size()
                    && d.step_idx >= 0
                    && d.step_idx < (int)m.actions[d.sub_idx].size()) {
                    auto* act = m.actions[d.sub_idx][d.step_idx];
                    act->elapsed = d.elapsed;
                    act->status = (d.exit_code == 0)
                        ? FindingStatus::Success : FindingStatus::Failed;
                }
                return {std::move(m), Cmd<Msg>{}};
            },

            [&](SubDone d) -> std::pair<Model, Cmd<Msg>> {
                if (d.sub_idx >= 0 && d.sub_idx < (int)m.subs.size()) {
                    auto* sub = m.subs[d.sub_idx];
                    sub->set_status(InvestigationStatus::Complete);
                    sub->set_conclusion(d.conclusion);
                }
                ++m.completed_subs;
                Cmd<Msg> follow{};
                if (m.completed_subs == (int)m.subs.size()) {
                    follow = Cmd<Msg>::after(0ms,
                        AllDone{"All sub-investigations finished — see findings above."});
                }
                return {std::move(m), std::move(follow)};
            },

            [&](AllDone a) -> std::pair<Model, Cmd<Msg>> {
                m.tree->set_status(InvestigationStatus::Complete);
                m.tree->set_conclusion(a.conclusion);
                // Inline mode: hold the final tree on screen briefly, then
                // exit so the completed render gets committed to scrollback.
                return {std::move(m), Cmd<Msg>::after(1500ms, Quit{})};
            },

            [&](Quit) -> std::pair<Model, Cmd<Msg>> {
                return {std::move(m), Cmd<Msg>::quit()};
            },
        }, msg);
    }

    static Element view(const Model& m) {
        return v(
            t<"Maya · Concurrent Investigation"> | Bold | Fg<100, 180, 255>,
            blank_,
            dyn([&] { return m.tree->build(); }),
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
};

static_assert(Program<Investigate>);

int main() {
    run<Investigate>({
        .title = "investigate",
        .fps   = 20,
        .mode  = Mode::Inline,   // Render below the prompt; preserve scrollback.
    });
}
