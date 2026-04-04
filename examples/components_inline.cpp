// maya — Simulated AI agent session (inline mode with event handling)
//
// Plays back a realistic agent session: user prompt → thinking → tool calls →
// diffs → streaming response. Pauses at permission prompts for real keyboard
// input (a = allow, x = deny, d = options dropdown, q = quit).
//
// Uses run({.alt_screen = false}) for inline rendering WITH event handling.
//
// Usage: ./maya_components_inline

#include <maya/maya.hpp>
#include <maya/dsl.hpp>
#include <maya/components/components.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

using namespace maya;
using namespace maya::dsl;
using namespace maya::components;

// ── Timeline: each step is a phase of agent behavior ────────────────────────

enum class Phase {
    UserMessage,      // user prompt appears
    ThinkingStream,   // thinking text streams in
    ToolSearch,       // grep tool fires (spinner → done)
    ToolSearchResult, // search results expand
    ToolRead,         // read file tool fires
    ToolReadResult,   // file contents shown
    ToolEditPermission, // permission prompt for edit
    ToolEdit,           // edit tool fires (after approval)
    ToolEditResult,     // diff appears
    ToolTestPermission, // permission prompt for test
    ToolTest,           // run tests tool fires (after approval)
    ToolTestResult,     // test output
    ResponseStream,   // final markdown response streams in
    Complete,         // done — show summary
};

struct Step {
    Phase phase;
    float duration;    // seconds this phase takes
};

static const Step kTimeline[] = {
    {Phase::UserMessage,      0.8f},
    {Phase::ThinkingStream,   3.0f},
    {Phase::ToolSearch,       1.2f},
    {Phase::ToolSearchResult, 1.0f},
    {Phase::ToolRead,           1.5f},
    {Phase::ToolReadResult,     1.2f},
    {Phase::ToolEditPermission, 0.0f},  // interactive — waits for user input
    {Phase::ToolEdit,           0.8f},
    {Phase::ToolEditResult,     1.0f},
    {Phase::ToolTestPermission, 0.0f},  // interactive — waits for user input
    {Phase::ToolTest,           2.0f},
    {Phase::ToolTestResult,     1.0f},
    {Phase::ResponseStream,   3.5f},
    {Phase::Complete,         2.0f},
};
static constexpr int kNumSteps = sizeof(kTimeline) / sizeof(kTimeline[0]);

// ── Content ─────────────────────────────────────────────────────────────────

static const char* kUserPrompt =
    "The session middleware is broken — users are getting logged out after "
    "exactly 1 hour even though SESSION_TIMEOUT is set to 24h. Can you fix it?";

static const char* kThinking =
    "The user reports sessions expire after 1 hour despite a 24-hour timeout "
    "setting. This is a classic units mismatch — `Date.now()` returns "
    "milliseconds but the timeout constant is likely in seconds. Let me search "
    "for the session timeout logic to confirm, then check how the comparison "
    "is done. If SESSION_TIMEOUT = 86400 (seconds) but the elapsed time is in "
    "milliseconds, the comparison would effectively timeout after ~86 seconds, "
    "not 1 hour. Wait — 3600 seconds = 1 hour. So SESSION_TIMEOUT might be "
    "3600 (1h) when it should be 86400 (24h), or there's a units bug.";

static const char* kSearchResults =
    "src/middleware/session.ts:8:   const SESSION_TIMEOUT = 86400;\n"
    "src/middleware/session.ts:42:  if (Date.now() - session.created > SESSION_TIMEOUT) {\n"
    "src/middleware/session.ts:43:    return { valid: false, reason: 'expired' };\n"
    "src/config/defaults.ts:12:     sessionTimeout: 86400,  // 24 hours in seconds\n"
    "tests/session.test.ts:87:      expect(isExpired(hourOldSession)).toBe(false);";

static const char* kFileContent =
    "export function validateSession(session: Session): ValidationResult {\n"
    "  const SESSION_TIMEOUT = 86400; // 24 hours in seconds\n"
    "\n"
    "  if (!session || !session.token) {\n"
    "    return { valid: false, reason: 'missing' };\n"
    "  }\n"
    "\n"
    "  // BUG: Date.now() is milliseconds, SESSION_TIMEOUT is seconds\n"
    "  if (Date.now() - session.created > SESSION_TIMEOUT) {\n"
    "    return { valid: false, reason: 'expired' };\n"
    "  }\n"
    "\n"
    "  return { valid: true };\n"
    "}";

static const char* kDiff =
    "@@ -8,3 +8,3 @@ export function validateSession(session: Session): ValidationResult {\n"
    "-  if (Date.now() - session.created > SESSION_TIMEOUT) {\n"
    "+  if (Date.now() - session.created > SESSION_TIMEOUT * 1000) {\n"
    "     return { valid: false, reason: 'expired' };";

static const char* kTestOutput =
    "PASS  tests/session.test.ts\n"
    "  Session Validation\n"
    "    ✓ rejects missing session (2ms)\n"
    "    ✓ rejects expired session (1ms)\n"
    "    ✓ accepts valid session (1ms)\n"
    "    ✓ accepts session under 24h old (1ms)\n"
    "    ✓ rejects session over 24h old (1ms)\n"
    "\n"
    "Tests: 5 passed, 5 total\n"
    "Time:  0.847s";

static const char* kResponse =
    "**Fixed the session timeout bug** in `src/middleware/session.ts`.\n"
    "\n"
    "## Root cause\n"
    "\n"
    "`Date.now()` returns **milliseconds** since epoch, but `SESSION_TIMEOUT` "
    "was set to `86400` (seconds). The comparison:\n"
    "\n"
    "```ts\n"
    "if (Date.now() - session.created > SESSION_TIMEOUT)\n"
    "```\n"
    "\n"
    "Was comparing milliseconds against seconds — so sessions expired after "
    "~86 seconds (86400ms ≈ 1.4 minutes), not 24 hours. The 1-hour expiry "
    "users saw was likely due to session refresh extending the window.\n"
    "\n"
    "## Fix\n"
    "\n"
    "Multiply `SESSION_TIMEOUT` by 1000 to convert to milliseconds:\n"
    "\n"
    "```ts\n"
    "if (Date.now() - session.created > SESSION_TIMEOUT * 1000)\n"
    "```\n"
    "\n"
    "All 5 session tests pass.";

// ── State ───────────────────────────────────────────────────────────────────

struct State {
    int   step      = 0;
    float step_t    = 0;      // time within current step
    float total_t   = 0;
    int   frame     = 0;
    int   tokens_in = 1847;
    int   tokens_out = 0;

    // Persistent permission prompts (survive across frames)
    PermissionPrompt edit_perm{PermissionPromptProps{
        .title = "Edit src/middleware/session.ts:9",
        .style = PermissionStyle::Dropdown,
        .options = {
            {.label = "Only this time", .decision = PermissionDecision::AllowOnce, .is_default = true},
            {.label = "Always for src/middleware/", .decision = PermissionDecision::AllowAlways},
            {.label = "Always for edit_file", .decision = PermissionDecision::AllowAlways},
        },
    }};

    PermissionPrompt test_perm{PermissionPromptProps{
        .title = "Run `npm test -- --testPathPattern=session`",
        .warning = "This will execute a shell command",
        .style = PermissionStyle::Dangerous,
    }};

    // Timing
    using Clock = std::chrono::steady_clock;
    Clock::time_point last_tick = Clock::now();
};

static float progress(const State& st) {
    if (st.step >= kNumSteps) return 1.0f;
    return std::clamp(st.step_t / kTimeline[st.step].duration, 0.f, 1.f);
}

static Phase current_phase(const State& st) {
    if (st.step >= kNumSteps) return Phase::Complete;
    return kTimeline[st.step].phase;
}

static bool phase_reached(const State& st, Phase p) {
    if (st.step >= kNumSteps) return true;
    return kTimeline[st.step].phase > p ||
           (kTimeline[st.step].phase == p);
}

static bool is_permission_phase(Phase p) {
    return p == Phase::ToolEditPermission || p == Phase::ToolTestPermission;
}

static void advance(State& st) {
    auto now = State::Clock::now();
    float dt = std::chrono::duration<float>(now - st.last_tick).count();
    st.last_tick = now;

    st.total_t += dt;
    ++st.frame;

    if (st.step >= kNumSteps) { quit(); return; }

    // Pause during permission phases — wait for user input
    Phase cur = kTimeline[st.step].phase;
    if (is_permission_phase(cur)) return;

    st.step_t += dt;

    if (st.step_t >= kTimeline[st.step].duration) {
        st.step_t -= kTimeline[st.step].duration;
        st.step++;
        if (st.step >= kNumSteps) { quit(); return; }
    }

    // Estimate output tokens
    st.tokens_out = 0;
    for (int i = 0; i <= st.step && i < kNumSteps; ++i) {
        float t = (i < st.step) ? 1.0f : progress(st);
        switch (kTimeline[i].phase) {
            case Phase::ThinkingStream:
                st.tokens_out += static_cast<int>(t * 120); break;
            case Phase::ResponseStream:
                st.tokens_out += static_cast<int>(t * 180); break;
            default: break;
        }
    }
}

// Advance past a permission phase after user decides
static void skip_permission(State& st) {
    if (st.step < kNumSteps && is_permission_phase(kTimeline[st.step].phase)) {
        st.step_t = 0;
        st.step++;
        if (st.step >= kNumSteps) quit();
    }
}

// ── Streaming text helper ───────────────────────────────────────────────────

static std::string stream_text(const char* full, float t) {
    int len = static_cast<int>(std::strlen(full));
    int show = static_cast<int>(t * static_cast<float>(len));
    show = std::clamp(show, 0, len);
    // Don't break UTF-8 sequences
    while (show < len && (full[show] & 0xC0) == 0x80) ++show;
    return std::string(full, static_cast<size_t>(show));
}

// ── Build UI ────────────────────────────────────────────────────────────────

static Element build_ui(State& st) {
    std::vector<Element> rows;
    auto& p = palette();
    Phase cur = current_phase(st);
    float t = progress(st);

    // ── Header ──────────────────────────────────────────────────────────
    rows.push_back(StatusBar(StatusBarProps{.sections = {
        {.content = "maya agent", .color = p.primary, .bold = true},
        {.content = "claude-opus-4-6", .color = p.accent},
        {.content = fmt("%d in / %d out tokens", st.tokens_in, st.tokens_out),
         .color = p.muted},
    }}));
    rows.push_back(text(""));

    // ── User message ────────────────────────────────────────────────────
    if (phase_reached(st, Phase::UserMessage)) {
        std::string msg = (cur == Phase::UserMessage)
            ? stream_text(kUserPrompt, t) : kUserPrompt;

        rows.push_back(MessageBubble(MessageBubbleProps{
            .role = Role::User,
            .content = msg,
        }));
        rows.push_back(text(""));
    }

    // ── Thinking ────────────────────────────────────────────────────────
    if (phase_reached(st, Phase::ThinkingStream)) {
        bool is_thinking = (cur == Phase::ThinkingStream);
        bool thinking_done = !is_thinking && phase_reached(st, Phase::ToolSearch);

        std::string content = is_thinking
            ? stream_text(kThinking, t) : kThinking;

        auto tb = ThinkingBlock(ThinkingBlockProps{
            .content = content,
            .expanded = is_thinking || cur == Phase::ToolSearch,
            .is_streaming = is_thinking,
            .max_lines = 8,
        });
        rows.push_back(tb.render(st.frame));

        if (thinking_done && cur != Phase::ThinkingStream) {
            // Collapse after moving on
            auto collapsed_tb = ThinkingBlock(ThinkingBlockProps{
                .content = content,
                .expanded = false,
            });
            // Only show collapsed once we're past the search
            if (static_cast<int>(cur) >= static_cast<int>(Phase::ToolSearchResult)) {
                rows.pop_back(); // remove expanded
                rows.push_back(collapsed_tb.render(st.frame));
            }
        }
        rows.push_back(text(""));
    }

    // ── Tool: grep search ───────────────────────────────────────────────
    if (phase_reached(st, Phase::ToolSearch)) {
        bool active = (cur == Phase::ToolSearch);
        bool has_result = phase_reached(st, Phase::ToolSearchResult);

        TaskStatus status = active ? TaskStatus::InProgress
                          : has_result ? TaskStatus::Completed
                          : TaskStatus::Pending;

        Children children;
        if (has_result) {
            std::string result = (cur == Phase::ToolSearchResult)
                ? stream_text(kSearchResults, t) : kSearchResults;
            children.push_back(CodeBlock(CodeBlockProps{
                .code = result,
                .show_line_nums = false,
            }));
        }

        // Once we have later tools, collapse this one
        bool collapsed = static_cast<int>(cur) >= static_cast<int>(Phase::ToolRead);

        rows.push_back(ToolCard(ToolCardProps{
            .title = "Search: \"session.*timeout\"",
            .status = status,
            .frame = st.frame,
            .collapsed = collapsed,
            .summary = "5 results in 4 files",
            .children = std::move(children),
            .tool_name = "grep",
        }));
        rows.push_back(text(""));
    }

    // ── Tool: read file ─────────────────────────────────────────────────
    if (phase_reached(st, Phase::ToolRead)) {
        bool active = (cur == Phase::ToolRead);
        bool has_result = phase_reached(st, Phase::ToolReadResult);

        TaskStatus status = active ? TaskStatus::InProgress
                          : has_result ? TaskStatus::Completed
                          : TaskStatus::Pending;

        Children children;
        if (has_result) {
            std::string result = (cur == Phase::ToolReadResult)
                ? stream_text(kFileContent, t) : kFileContent;
            children.push_back(CodeBlock(CodeBlockProps{
                .code = result,
                .language = "typescript",
                .start_line = 1,
                .highlight_lines = {9},
            }));
        }

        bool collapsed = static_cast<int>(cur) >= static_cast<int>(Phase::ToolEditPermission);

        rows.push_back(ToolCard(ToolCardProps{
            .title = "Read: src/middleware/session.ts",
            .status = status,
            .frame = st.frame,
            .collapsed = collapsed,
            .summary = "14 lines",
            .children = std::move(children),
            .tool_name = "read_file",
        }));
        rows.push_back(text(""));
    }

    // ── Tool: edit file (with permission) ────────────────────────────────
    if (phase_reached(st, Phase::ToolEditPermission)) {
        bool waiting  = (cur == Phase::ToolEditPermission);
        bool active   = (cur == Phase::ToolEdit);
        bool has_result = phase_reached(st, Phase::ToolEditResult);

        TaskStatus status = waiting    ? TaskStatus::WaitingForConfirmation
                          : active     ? TaskStatus::InProgress
                          : has_result ? TaskStatus::Completed
                          : TaskStatus::Pending;

        Children children;
        if (has_result) {
            children.push_back(DiffView(DiffViewProps{
                .diff = kDiff,
                .file_path = "src/middleware/session.ts",
            }));
        }

        // Render persistent permission prompt
        Element perm_el;
        if (waiting) {
            perm_el = st.edit_perm.render(st.frame);
        }

        bool collapsed = static_cast<int>(cur) >= static_cast<int>(Phase::ToolTestPermission);

        rows.push_back(ToolCard(ToolCardProps{
            .title = "Edit: src/middleware/session.ts:9",
            .status = status,
            .frame = st.frame,
            .collapsed = collapsed,
            .summary = "+1 -1",
            .children = std::move(children),
            .tool_name = "edit_file",
            .permission = std::move(perm_el),
        }));
        rows.push_back(text(""));
    }

    // ── Tool: run tests (with permission) ────────────────────────────────
    if (phase_reached(st, Phase::ToolTestPermission)) {
        bool waiting  = (cur == Phase::ToolTestPermission);
        bool active   = (cur == Phase::ToolTest);
        bool has_result = phase_reached(st, Phase::ToolTestResult);

        TaskStatus status = waiting    ? TaskStatus::WaitingForConfirmation
                          : active     ? TaskStatus::InProgress
                          : has_result ? TaskStatus::Completed
                          : TaskStatus::Pending;

        Children children;
        if (has_result) {
            std::string result = (cur == Phase::ToolTestResult)
                ? stream_text(kTestOutput, t) : kTestOutput;
            children.push_back(CodeBlock(CodeBlockProps{
                .code = result,
                .show_line_nums = false,
            }));
        }

        // Render persistent permission prompt
        Element perm_el;
        if (waiting) {
            perm_el = st.test_perm.render(st.frame);
        }

        rows.push_back(ToolCard(ToolCardProps{
            .title = "Run: npm test -- --testPathPattern=session",
            .status = status,
            .frame = st.frame,
            .children = std::move(children),
            .tool_name = "terminal",
            .permission = std::move(perm_el),
        }));
        rows.push_back(text(""));
    }

    // ── Response ────────────────────────────────────────────────────────
    if (phase_reached(st, Phase::ResponseStream)) {
        bool streaming = (cur == Phase::ResponseStream);

        std::string content = streaming
            ? stream_text(kResponse, t) : kResponse;

        Children md_children;
        md_children.push_back(Markdown(MarkdownProps{.source = content}));

        rows.push_back(MessageBubble(MessageBubbleProps{
            .role = Role::Assistant,
            .children = std::move(md_children),
            .is_streaming = streaming,
            .frame = st.frame,
        }));
        rows.push_back(text(""));
    }

    // ── Activity bar ────────────────────────────────────────────────────
    {
        auto plan_status = [&](Phase p, Phase perm = Phase::Complete) -> TaskStatus {
            if (cur == perm) return TaskStatus::WaitingForConfirmation;
            if (static_cast<int>(cur) > static_cast<int>(p)) return TaskStatus::Completed;
            if (cur == p) return TaskStatus::InProgress;
            return TaskStatus::Pending;
        };

        std::vector<FileEdit> edits;
        if (phase_reached(st, Phase::ToolEditResult)) {
            edits.push_back({.path = "src/middleware/session.ts", .added = 1, .removed = 1});
        }

        auto bar = ActivityBar(ActivityBarProps{
            .plan_items = {
                {.text = "Search for timeout logic",
                 .status = plan_status(Phase::ToolSearch)},
                {.text = "Read session.ts",
                 .status = plan_status(Phase::ToolRead)},
                {.text = "Fix units mismatch",
                 .status = plan_status(Phase::ToolEdit, Phase::ToolEditPermission)},
                {.text = "Run tests",
                 .status = plan_status(Phase::ToolTest, Phase::ToolTestPermission)},
            },
            .edits = std::move(edits),
        });
        rows.push_back(bar.render(st.frame));
        rows.push_back(text(""));
    }

    // ── Summary footer ──────────────────────────────────────────────────
    if (phase_reached(st, Phase::Complete)) {
        float cost = (static_cast<float>(st.tokens_in) * 15.f +
                      static_cast<float>(st.tokens_out) * 75.f) / 1000000.f;
        rows.push_back(Divider(DividerProps{.label = "Done"}));
        rows.push_back(hstack().gap(3)(
            DiffStat(DiffStatProps{.added = 1, .removed = 1}),
            Chip(ChipProps{.label = "5/5 tests pass", .severity = Severity::Success}),
            text(fmt("$%.4f", static_cast<double>(cost)),
                 Style{}.with_fg(p.dim))
        ));
        rows.push_back(text(""));
        rows.push_back(KeyBindings({
            {.keys = "Enter", .label = "accept"},
            {.keys = "u", .label = "undo"},
            {.keys = "r", .label = "retry"},
            {.keys = "q", .label = "quit"},
        }));
    }

    return vstack()(std::move(rows));
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main() {
    State st;

    run(
        {.fps = 30, .alt_screen = false},

        // Event handler — forward to permission prompts, q to quit
        [&](const Event& ev) {
            if (key(ev, 'q')) return false;

            Phase cur = current_phase(st);

            if (cur == Phase::ToolEditPermission) {
                st.edit_perm.update(ev);
                if (st.edit_perm.decided()) skip_permission(st);
            } else if (cur == Phase::ToolTestPermission) {
                st.test_perm.update(ev);
                if (st.test_perm.decided()) skip_permission(st);
            }

            return true;
        },

        // Render function
        [&] {
            advance(st);
            return build_ui(st);
        }
    );
}
