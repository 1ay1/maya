// test_host — terminal_host's scheduling logic, against a fake device.
//
// This is the code that had no unit test: frame debt, the animation
// deadline, the fps clock, the ack window, and the input held back across a
// navigation key. It ran only inside a real pty, so a regression showed up
// as "the smoke suite is weird today" rather than a failing assert.
//
// terminal_host is templated on its device (maya::Device, host/device.hpp),
// so here it drives a FakeDevice that records calls and replays scripted
// answers. Nothing opens a terminal; every test is a few microseconds.
//
// What each case pins:
//   1. an idle program asks for no frame and no wakeup (the 45%-of-a-core
//      bug class: a host that always owes a frame is a busy loop)
//   2. Presented::redraw_at becomes a wait hint, so an animation wakes
//      exactly once, when it asked to
//   3. redraw_now means the frame is owed immediately
//   4. a backpressured tty stops frames until the device says ready
//   5. fps > 0 owes frames on a period, event-driven owes none
//   6. every terminal effect is one device call, with its payload intact

#undef NDEBUG
#include <maya/host/terminal.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <exception>
#include <variant>
#include <vector>

// Standalone (no doctest): this TU links maya_app, and a tiny harness keeps
// it independent of the consolidated runner.
#define MAYA_CAT_(a, b) a##b
#define MAYA_CAT(a, b) MAYA_CAT_(a, b)

struct Case { const char* name; void (*fn)(); };
static std::vector<Case>& cases() { static std::vector<Case> v; return v; }
static int g_fails = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) { ++g_fails; std::printf("  FAIL"); std::printf(__VA_ARGS__); } \
    } while (0)

#define TEST_CASE(nm)                                                          \
    static void MAYA_CAT(tc_, __LINE__)();                                     \
    static const struct MAYA_CAT(reg_, __LINE__) {                             \
        MAYA_CAT(reg_, __LINE__)() { cases().push_back({nm, MAYA_CAT(tc_, __LINE__)}); } \
    } MAYA_CAT(reg_inst_, __LINE__);                                           \
    static void MAYA_CAT(tc_, __LINE__)()

using namespace maya;
using namespace std::chrono_literals;

// ── the fake device ────────────────────────────────────────────────────────
// Models maya::Device. Answers come from public fields the test sets; calls
// land in `log` so a test can assert what the host asked for.
namespace {

struct FakeDevice {
    // what the host asked for, in order
    std::vector<std::string> log;
    // scripted answers
    Presented next_present{};
    Size      sz{Columns{80}, Rows{24}};
    bool      backpressured_v = false;
    bool      pending_v       = false;
    bool      ready_v         = true;
    std::optional<Presented::clock::time_point> ready_deadline_v{};
    std::vector<Event> next_read;
    bool      pending_input_v = false;
    std::string last_title, last_clip, last_seq;
    int       commits = 0, overflows = 0, redraws = 0, resets = 0;
    bool      mouse_on = false;
    int       presents = 0, warms = 0, flushes = 0;

    [[nodiscard]] Size size() const noexcept { return sz; }
    [[nodiscard]] platform::NativeHandle input_handle() const noexcept { return {}; }
    [[nodiscard]] platform::NativeHandle output_handle() const noexcept { return {}; }

    Result<std::vector<Event>> read() { return std::move(next_read); }
    [[nodiscard]] bool has_pending_input() const noexcept { return pending_input_v; }
    std::vector<Event> resolve_pending_input() { pending_input_v = false; return {}; }
    void on_resize() { log.push_back("on_resize"); }

    // present() takes a BUILDER: widgets make their animation requests while
    // the tree is built, so the device must run build() itself.
    Presented present(const Element&) { ++presents; log.push_back("present"); return next_present; }
    template <class Build>
    Presented present(Build&& build) {
        ++presents; log.push_back("present");
        (void)build();                       // build it: that's where requests land
        return next_present;
    }
    template <class Build>
    Presented present_if(std::uint64_t, Build&& build, bool = false) {
        ++presents; log.push_back("present_if");
        (void)build();
        return next_present;
    }
    void warm(const Element&) { ++warms; log.push_back("warm"); }

    [[nodiscard]] bool backpressured() const noexcept { return backpressured_v; }
    [[nodiscard]] bool pending_output() const noexcept { return pending_v; }
    // True when the residue is gone.
    bool flush() { ++flushes; log.push_back("flush"); return !pending_v; }
    [[nodiscard]] bool ready() noexcept { return ready_v; }
    [[nodiscard]] std::optional<Presented::clock::time_point> ready_deadline() const noexcept {
        return ready_deadline_v;
    }

    void set_title(std::string_view s)          { last_title = s; log.push_back("set_title"); }
    void write_clipboard(std::string_view s)    { last_clip = s;  log.push_back("write_clipboard"); }
    void query_clipboard()                      { log.push_back("query_clipboard"); }
    void emit_host_sequence(std::string_view s) { last_seq = s;   log.push_back("emit_host_sequence"); }
    void commit_scrollback(ScrollbackDebt)      { ++commits;      log.push_back("commit_scrollback"); }
    void commit_overflow()                      { ++overflows;    log.push_back("commit_overflow"); }
    void force_redraw()                         { ++redraws;      log.push_back("force_redraw"); }
    void reset_inline()                         { ++resets;       log.push_back("reset_inline"); }
    void set_mouse(bool on)                     { mouse_on = on;  log.push_back("set_mouse"); }
};

static_assert(Device<FakeDevice>, "the fake must model the same surface as Screen");

// ── a minimal program ──────────────────────────────────────────────────────
struct Tick {};
struct Bye {};

struct Prog {
    struct Model { int n = 0; };
    using Msg = std::variant<Tick, Bye>;
    using Cmd = terminal_cmd<Msg>;
    using Sub = jaal::Sub<Msg, on_key>;

    static Cmd update(Model& m, Tick) { ++m.n; return {}; }
    static Cmd update(Model&, Bye)    { return Cmd::quit(0); }
    static Element view(const Model& m) { return dsl::text(std::to_string(m.n)); }
    static Sub subscribe(const Model&) { return keys<Sub>({{'q', Bye{}}}); }
};

using Host = terminal_host<Prog, FakeDevice>;

// present(k) wants the KERNEL, not the model: it reads k.model() and reports
// a throwing view() through k.view_faulted(). This is the slice of that the
// host touches.
struct FakeKernel {
    Prog::Model m{};
    int faults = 0;
    Prog::Model& model() noexcept { return m; }
    const Prog::Model& model() const noexcept { return m; }
    void view_faulted(std::exception_ptr) { ++faults; }
};

} // namespace

TEST_CASE("host: an idle program owes no frame and no wakeup") {
    // The busy-loop guard. A host fresh off a frame that asked for nothing
    // must not claim a frame is due, and must not hand jaal a wait hint —
    // otherwise the loop spins and burns a core (what .fps=30 did to
    // messenger).
    FakeDevice dev;
    Host h{dev};
    FakeKernel k;

    dev.next_present = Presented{};            // no redraw_at, no redraw_now
    h.present(k);

    CHECK(!h.owes_frame(), "  an idle host must not owe a frame\n");
    CHECK(!h.wait_hint().has_value(),
          "  an idle host must not ask to be woken (that is the busy loop)\n");
    CHECK(dev.presents == 1, "  present() must reach the device once (got %d)\n", dev.presents);
}

TEST_CASE("host: redraw_at becomes a wait hint") {
    // An animation asks to be drawn again at time T. The host must turn that
    // into a bounded wait, so jaal sleeps until T instead of polling.
    FakeDevice dev;
    Host h{dev};
    FakeKernel k;

    Presented f{};
    f.redraw_at = Presented::clock::now() + 50ms;
    dev.next_present = f;
    h.present(k);

    auto hint = h.wait_hint();
    CHECK(hint.has_value(), "  a redraw_at must produce a wait hint\n");
    if (hint) {
        CHECK(*hint <= 50ms, "  the hint must not overshoot the deadline (got %lldms)\n",
              (long long)hint->count());
    }
}

TEST_CASE("host: redraw_now owes a frame immediately") {
    // redraw_now is the frame saying "I was drawn with stale geometry, draw
    // me again". It must be owed at once, with a zero wait.
    FakeDevice dev;
    Host h{dev};
    FakeKernel k;

    Presented f{};
    f.redraw_now = true;
    dev.next_present = f;
    h.present(k);

    CHECK(h.owes_frame(), "  redraw_now must owe a frame\n");
    auto hint = h.wait_hint();
    CHECK(hint.has_value() && *hint == 0ms,
          "  an owed frame must ask for a zero wait, not a sleep\n");
}

TEST_CASE("host: a backed-up tty stops frames until it drains") {
    // Flow control: while the device is backpressured the host must not keep
    // painting into a pipe that refuses bytes.
    FakeDevice dev;
    Host h{dev};
    FakeKernel k;

    dev.next_present = Presented{};
    h.present(k);
    const int after_first = dev.presents;

    dev.backpressured_v = true;
    dev.ready_v         = false;
    h.present(k);
    CHECK(dev.presents == after_first,
          "  a backpressured device must not be presented to (got %d, want %d)\n",
          dev.presents, after_first);

    // Draining lets frames through again.
    dev.backpressured_v = false;
    dev.ready_v         = true;
    h.present(k);
    CHECK(dev.presents == after_first + 1,
          "  a drained device must accept the held frame (got %d)\n", dev.presents);
}

TEST_CASE("host: fps owes frames on a period, event-driven owes none") {
    // .fps = N means "draw N times a second whatever the model does". 0 means
    // draw only when something changed. messenger set 30 on top of a 50ms
    // tick and paid 45% of a core for it.
    FakeDevice dev_ev;
    Host event_driven{dev_ev, 0};
    FakeKernel k;
    dev_ev.next_present = Presented{};
    event_driven.present(k);
    CHECK(!event_driven.owes_frame(),
          "  fps=0 must owe nothing once the frame is out\n");
    CHECK(!event_driven.wait_hint().has_value(),
          "  fps=0 must not schedule a wakeup\n");

    FakeDevice dev_fps;
    Host continuous{dev_fps, 60};
    dev_fps.next_present = Presented{};
    continuous.present(k);
    auto hint = continuous.wait_hint();
    CHECK(hint.has_value(), "  fps>0 must schedule the next frame\n");
    if (hint)
        CHECK(*hint <= 17ms, "  60fps must wake within a frame period (got %lldms)\n",
              (long long)hint->count());
}

TEST_CASE("host: every terminal effect is one device call") {
    // The effects a program returns in its Cmd must arrive at the device
    // unchanged — this is the whole contract of host/effects.hpp.
    FakeDevice dev;
    Host h{dev};

    h.handle(SetTitle{"hello"});
    CHECK(dev.last_title == "hello", "  set_title payload must survive (got '%s')\n",
          dev.last_title.c_str());

    h.handle(WriteClipboard{"clip"});
    CHECK(dev.last_clip == "clip", "  clipboard payload must survive (got '%s')\n",
          dev.last_clip.c_str());

    h.handle(EmitHostSequence{"\x1b]0;x\x1b\\"});
    CHECK(!dev.last_seq.empty(), "  a host sequence must reach the device\n");

    h.handle(QueryClipboard{});
    h.handle(CommitOverflow{});
    h.handle(ForceRedraw{});
    h.handle(ResetInline{});
    h.handle(SetMouse{true});

    CHECK(dev.overflows == 1, "  commit_overflow must be one call (got %d)\n", dev.overflows);
    CHECK(dev.redraws == 1,   "  force_redraw must be one call (got %d)\n", dev.redraws);
    CHECK(dev.resets == 1,    "  reset_inline must be one call (got %d)\n", dev.resets);
    CHECK(dev.mouse_on,       "  set_mouse(true) must reach the device\n");
}

TEST_CASE("host: osc() builds a well-formed sequence") {
    // osc(code, payload) is the one place maya formats a control string for
    // a cooperating host terminal; a malformed one corrupts the screen.
    auto e = osc(0, "title");
    CHECK(e.sequence.starts_with("\x1b]0;"),
          "  osc must start with ESC ] code ;\n");
    CHECK(e.sequence.ends_with("\x1b\\"),
          "  osc must end with ST (ESC backslash)\n");
    CHECK(e.sequence.find("title") != std::string::npos,
          "  osc must carry its payload\n");
}

int main() {
    std::printf("test_host:\n");
    for (const auto& c : cases()) {
        const int before = g_fails;
        c.fn();
        std::printf("  %-4s %s\n", g_fails == before ? "ok" : "FAIL", c.name);
    }
    if (g_fails) std::printf("test_host: %d FAILED\n", g_fails);
    else         std::printf("test_host: ok (%zu cases)\n", cases().size());
    return g_fails ? 1 : 0;
}
