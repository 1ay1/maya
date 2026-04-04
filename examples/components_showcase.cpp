// maya — Showcase of all new components
//
// Demonstrates: Toggle, Sparkline, Gauge, BarChart, CostMeter, FormField,
// RadioGroup, NumberInput, StreamingText, Table, Tree, LogView, FileTree
//
// Usage: ./maya_components_showcase
//        Navigate tabs with Tab/BackTab, interact with focused component.

#include <maya/maya.hpp>
#include <maya/dsl.hpp>
#include <maya/components/components.hpp>

#include <cmath>
#include <string>
#include <vector>

using namespace maya;
using namespace maya::dsl;
using namespace maya::components;

// ── State ───────────────────────────────────────────────────────────────────

struct State {
    int frame = 0;

    // Tab navigation
    Tabs tabs{TabsProps{.labels = {
        "Primitives", "Inputs", "Data", "Content"
    }}};

    // ── Primitives tab state ──────────────────────────────────────
    bool toggle_a = true;
    bool toggle_b = false;
    bool toggle_c = true;

    // ── Inputs tab state ──────────────────────────────────────────
    RadioGroup<std::string> radio{RadioGroupProps<std::string>{
        .items = {"claude-opus-4-6", "claude-sonnet-4-6", "claude-haiku-4-5"},
        .selected = 0,
    }};

    NumberInput number{NumberInputProps{
        .value = 42, .min = 0, .max = 100, .step = 1,
        .label = "Temperature",
    }};

    StreamingText streaming{StreamingTextProps{
        .text = "The session middleware bug was caused by a units mismatch. "
                "Date.now() returns milliseconds, but SESSION_TIMEOUT was in seconds. "
                "Fixed by multiplying the timeout by 1000.",
    }};

    // ── Data tab state ────────────────────────────────────────────
    Table table{TableProps{
        .columns = {
            {.header = "Name",   .width = 20},
            {.header = "Status", .width = 12, .align = ColumnAlign::Center},
            {.header = "CPU",    .width = 8,  .align = ColumnAlign::Right},
            {.header = "Memory", .width = 10, .align = ColumnAlign::Right},
        },
        .rows = {
            {"api-server",   "Running", "23%",  "128MB"},
            {"worker-1",     "Running", "89%",  "512MB"},
            {"worker-2",     "Idle",    "2%",   "64MB"},
            {"scheduler",    "Running", "45%",  "256MB"},
            {"cache-redis",  "Running", "12%",  "1.2GB"},
            {"db-primary",   "Running", "67%",  "4.0GB"},
            {"db-replica",   "Syncing", "34%",  "3.8GB"},
            {"load-balancer", "Running", "8%",  "32MB"},
            {"monitor",      "Running", "5%",   "48MB"},
            {"log-shipper",  "Failed",  "0%",   "16MB"},
        },
        .max_visible = 8,
    }};

    Tree<std::string> tree{TreeProps<std::string>{
        .roots = {
            {.data = "src", .children = {
                {.data = "app", .children = {
                    {.data = "app.cpp"},
                    {.data = "inline.cpp"},
                }},
                {.data = "render", .children = {
                    {.data = "canvas.cpp"},
                    {.data = "renderer.cpp"},
                    {.data = "serialize.cpp"},
                    {.data = "diff.cpp"},
                }},
                {.data = "layout", .children = {
                    {.data = "yoga.cpp"},
                }},
            }, .expanded = true},
            {.data = "include", .children = {
                {.data = "maya", .children = {
                    {.data = "maya.hpp"},
                    {.data = "dsl.hpp"},
                }},
            }},
            {.data = "examples", .children = {
                {.data = "hello.cpp"},
                {.data = "components_showcase.cpp"},
            }},
            {.data = "CMakeLists.txt"},
            {.data = "README.md"},
        },
        .max_visible = 12,
    }};

    // ── Content tab state ─────────────────────────────────────────
    LogView log{LogViewProps{
        .max_visible = 10,
        .tail_follow = true,
        .show_line_nums = true,
    }};
    float log_timer = 0.f;
    int log_line = 0;

    // Sparkline data
    std::vector<float> cpu_history;
    std::vector<float> mem_history;
};

// ── Fake log messages ────────────────────────────────────────────────────────

static const char* kLogMessages[] = {
    "\033[32m[INFO]\033[0m  Server started on port 8080",
    "\033[32m[INFO]\033[0m  Connected to database",
    "\033[33m[WARN]\033[0m  Cache miss rate above threshold: 15%",
    "\033[32m[INFO]\033[0m  Request: GET /api/users (200, 12ms)",
    "\033[32m[INFO]\033[0m  Request: POST /api/auth (200, 45ms)",
    "\033[31m[ERROR]\033[0m Connection refused: redis://cache:6379",
    "\033[32m[INFO]\033[0m  Retry succeeded: redis reconnected",
    "\033[32m[INFO]\033[0m  Request: GET /api/status (200, 3ms)",
    "\033[33m[WARN]\033[0m  Slow query detected: SELECT * FROM sessions (234ms)",
    "\033[32m[INFO]\033[0m  Scheduled job completed: cleanup_sessions",
    "\033[32m[INFO]\033[0m  Request: PUT /api/users/42 (200, 18ms)",
    "\033[31m[ERROR]\033[0m Unhandled exception in worker-2: SIGTERM",
    "\033[32m[INFO]\033[0m  Worker-2 restarted successfully",
    "\033[34m[DEBUG]\033[0m GC pause: 12ms, heap: 234MB",
    "\033[32m[INFO]\033[0m  Health check: all services nominal",
};
static constexpr int kNumLogs = sizeof(kLogMessages) / sizeof(kLogMessages[0]);

// ── Advance ──────────────────────────────────────────────────────────────────

static void advance(State& st, float dt) {
    st.frame++;

    // Advance streaming text
    if (!st.streaming.fully_revealed() && st.frame % 2 == 0) {
        st.streaming.advance(1);
    }

    // Generate sparkline data
    float t = st.frame * 0.05f;
    float cpu = 0.3f + 0.2f * std::sin(t) + 0.1f * std::sin(t * 3.7f);
    float mem = 0.5f + 0.1f * std::sin(t * 0.3f);
    st.cpu_history.push_back(std::clamp(cpu, 0.f, 1.f));
    st.mem_history.push_back(std::clamp(mem, 0.f, 1.f));
    if (st.cpu_history.size() > 40) st.cpu_history.erase(st.cpu_history.begin());
    if (st.mem_history.size() > 40) st.mem_history.erase(st.mem_history.begin());

    // Append log lines periodically
    st.log_timer += dt;
    if (st.log_timer > 0.8f) {
        st.log_timer = 0.f;
        st.log.append(kLogMessages[st.log_line % kNumLogs]);
        st.log_line++;
    }
}

// ── Build tabs ───────────────────────────────────────────────────────────────

static Element build_primitives(State& st) {
    auto& p = palette();

    return vstack().gap(1)(
        // Toggles
        text("Toggles", Style{}.with_bold().with_fg(p.primary)),
        hstack().gap(3)(
            Toggle({.checked = st.toggle_a, .label = "Dark mode",  .style = ToggleStyle::Checkbox}),
            Toggle({.checked = st.toggle_b, .label = "Autosave",   .style = ToggleStyle::Switch}),
            Toggle({.checked = st.toggle_c, .label = "Telemetry",  .style = ToggleStyle::Dot}),
            Toggle({.checked = false, .label = "Disabled", .style = ToggleStyle::Checkbox, .disabled = true})
        ),

        // Sparklines
        text("System Metrics", Style{}.with_bold().with_fg(p.primary)),
        hstack().gap(2)(
            text("CPU ", Style{}.with_fg(p.muted)),
            Sparkline({.data = st.cpu_history, .color = Color::rgb(100, 200, 255)}),
            text(fmt(" %.0f%%", static_cast<double>(st.cpu_history.empty() ? 0 : st.cpu_history.back() * 100)),
                 Style{}.with_fg(p.text))
        ),
        hstack().gap(2)(
            text("MEM ", Style{}.with_fg(p.muted)),
            Sparkline({.data = st.mem_history, .color = Color::rgb(200, 120, 255)}),
            text(fmt(" %.0f%%", static_cast<double>(st.mem_history.empty() ? 0 : st.mem_history.back() * 100)),
                 Style{}.with_fg(p.text))
        ),

        // Gauges
        text("Gauges", Style{}.with_bold().with_fg(p.primary)),
        Gauge({.value = 0.45f, .label = "Disk   ", .width = 25}),
        Gauge({.value = 0.78f, .label = "Memory ", .width = 25,
               .thresholds = {{0.5f, p.success}, {0.7f, p.warning}, {0.9f, p.error}}}),
        Gauge({.value = 0.95f, .label = "CPU    ", .width = 25}),

        // Bar chart
        text("Request Latency (ms)", Style{}.with_bold().with_fg(p.primary)),
        BarChart({.bars = {
            {.label = "GET /api",     .value = 12,  .color = p.success},
            {.label = "POST /auth",   .value = 45,  .color = p.info},
            {.label = "PUT /users",   .value = 18,  .color = p.success},
            {.label = "DELETE /sess", .value = 234, .color = p.error},
            {.label = "GET /status",  .value = 3,   .color = p.success},
        }, .max_width = 30}),

        // Cost meter
        text("API Cost", Style{}.with_bold().with_fg(p.primary)),
        CostMeter({
            .input_tokens = 12847, .output_tokens = 3421,
            .cache_read = 5000, .cache_write = 1200,
            .cost = 0.42, .budget = 5.00,
            .model = "claude-opus-4-6", .compact = true,
        })
    );
}

static Element build_inputs(State& st) {
    auto& p = palette();

    return vstack().gap(1)(
        // Radio group
        FormField({
            .label = "Model Selection",
            .description = "Choose the AI model for this session",
            .children = {st.radio.render()},
        }),

        text(""),

        // Number input
        FormField({
            .label = "Max Tokens",
            .description = "Maximum number of output tokens",
            .children = {st.number.render()},
        }),

        text(""),

        // Streaming text
        text("Streaming Response", Style{}.with_bold().with_fg(p.primary)),
        st.streaming.render(),

        text(""),

        // Key bindings
        KeyBindings({
            {.keys = "j/k", .label = "navigate"},
            {.keys = "Enter", .label = "select"},
            {.keys = "Tab", .label = "next tab"},
            {.keys = "q", .label = "quit"},
        })
    );
}

static Element build_data(State& st) {
    auto& p = palette();

    return vstack().gap(1)(
        text("Process Table", Style{}.with_bold().with_fg(p.primary)),
        st.table.render(),

        text(""),

        text("Project Tree", Style{}.with_bold().with_fg(p.primary)),
        st.tree.render([&p](const std::string& data, int, bool, bool selected, bool) {
            Style s = selected
                ? Style{}.with_bold().with_fg(p.primary)
                : Style{}.with_fg(p.text);
            return text(data, s);
        })
    );
}

static Element build_content(State& st) {
    auto& p = palette();

    return vstack().gap(1)(
        text("Live Logs", Style{}.with_bold().with_fg(p.primary)),
        st.log.render(),

        text(""),

        text("Cost Breakdown", Style{}.with_bold().with_fg(p.primary)),
        CostMeter({
            .input_tokens = 12847, .output_tokens = 3421,
            .cache_read = 5000, .cache_write = 1200,
            .cost = 0.42, .budget = 5.00,
            .model = "claude-opus-4-6",
        })
    );
}

// ── Build UI ─────────────────────────────────────────────────────────────────

static Element build_ui(State& st) {
    auto& p = palette();

    Element panel;
    switch (st.tabs.active()) {
        case 0: panel = build_primitives(st); break;
        case 1: panel = build_inputs(st);     break;
        case 2: panel = build_data(st);       break;
        case 3: panel = build_content(st);    break;
        default: panel = text("?");           break;
    }

    return vstack()(
        StatusBar(StatusBarProps{.sections = {
            {.content = "maya components", .color = p.primary, .bold = true},
            {.content = "showcase", .color = p.accent},
        }}),
        text(""),
        st.tabs.render(),
        text(""),
        std::move(panel)
    );
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main() {
    State st;
    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();

    run(
        {.fps = 30, .alt_screen = true},

        [&](const Event& ev) {
            if (key(ev, 'q')) return false;

            // Tab navigation
            if (st.tabs.update(ev)) return true;

            // Forward to active tab's components
            switch (st.tabs.active()) {
                case 0:
                    if (key(ev, '1')) { st.toggle_a = !st.toggle_a; return true; }
                    if (key(ev, '2')) { st.toggle_b = !st.toggle_b; return true; }
                    if (key(ev, '3')) { st.toggle_c = !st.toggle_c; return true; }
                    break;
                case 1:
                    if (st.radio.update(ev)) return true;
                    if (st.number.update(ev)) return true;
                    break;
                case 2:
                    if (st.table.update(ev)) return true;
                    if (st.tree.update(ev)) return true;
                    break;
                case 3:
                    if (st.log.update(ev)) return true;
                    break;
            }

            return true;
        },

        [&] {
            auto now = Clock::now();
            float dt = std::chrono::duration<float>(now - last).count();
            last = now;
            advance(st, dt);
            return build_ui(st);
        }
    );
}
