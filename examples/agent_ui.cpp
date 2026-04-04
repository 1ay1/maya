// maya — Zed-like AI agent terminal UI
//
// Demonstrates the full component library: message list, tool cards,
// markdown rendering, thinking blocks, activity bar, text input,
// model selector, tabs, status bar — everything needed for an agent TUI.
//
// Keys: Enter = send  Tab = cycle tabs  t = toggle thinking
//       Ctrl+J = scroll down  Ctrl+K = scroll up  q = quit

#include <maya/maya.hpp>
#include <maya/dsl.hpp>
#include <maya/components/components.hpp>

#include <string>
#include <vector>

using namespace maya;
using namespace maya::dsl;
using namespace maya::components;

// ── Simulated conversation data ──────────────────────────────────────────────

struct Message {
    Role role;
    std::string content;
    bool has_thinking = false;
    std::string thinking = "";
    bool has_tool = false;
    std::string tool_name = "";
    std::string tool_title = "";
    TaskStatus tool_status = TaskStatus::Completed;
    std::string tool_content = "";
};

static std::vector<Message> g_messages = {
    Message{.role = Role::User,
            .content = "Can you analyze the authentication module and fix the session timeout bug?"},

    Message{.role = Role::Assistant,
            .has_thinking = true,
            .thinking = "Let me look at the authentication module. The user mentioned a session "
                        "timeout bug, so I should check the session handling code, token expiration "
                        "logic, and any middleware that validates sessions.",
            .has_tool = true,
            .tool_name = "grep",
            .tool_title = "Search: \"session.*timeout\"",
            .tool_content = "src/auth/session.ts:42:  const SESSION_TIMEOUT = 3600;\n"
                            "src/auth/middleware.ts:18:  if (isSessionTimedOut(token)) {\n"
                            "src/auth/session.ts:87:  // BUG: comparing milliseconds to seconds\n"
                            "tests/auth.test.ts:156:  it('should timeout after 1 hour',"},

    Message{.role = Role::Assistant,
            .has_tool = true,
            .tool_name = "read_file",
            .tool_title = "Read: src/auth/session.ts",
            .tool_content = "Found the bug on line 87: comparing `Date.now()` (milliseconds) "
                            "against `SESSION_TIMEOUT` (seconds). The comparison never triggers "
                            "because milliseconds >> seconds."},

    Message{.role = Role::Assistant,
            .has_tool = true,
            .tool_name = "edit_file",
            .tool_title = "Edit: src/auth/session.ts",
            .tool_content = "@@ -85,3 +85,3 @@\n"
                            "-  if (Date.now() - session.created > SESSION_TIMEOUT) {\n"
                            "+  if (Date.now() - session.created > SESSION_TIMEOUT * 1000) {\n"
                            "     return { valid: false, reason: 'expired' };"},

    Message{.role = Role::Assistant,
            .content = "Fixed the session timeout bug in `src/auth/session.ts`:\n\n"
                       "**Root cause**: `Date.now()` returns milliseconds, but `SESSION_TIMEOUT` "
                       "was in seconds (3600). The comparison `Date.now() - created > 3600` would "
                       "never trigger because the difference is always in the millions.\n\n"
                       "**Fix**: Multiply `SESSION_TIMEOUT` by 1000 to convert to milliseconds.\n\n"
                       "The existing test at `tests/auth.test.ts:156` should now pass."},
};

// ── App state ────────────────────────────────────────────────────────────────

static int       g_frame = 0;
static int       g_scroll = 0;
static bool      g_thinking_expanded = false;

static TextInput g_input(TextInputProps{
    .placeholder = "Message — Enter to send, @ for context",
    .max_height  = 5,
    .min_height  = 1,
});

static Tabs g_tabs(TabsProps{
    .labels = {"Agent", "History", "Config"},
    .active = 0,
});

static Select<std::string> g_model(SelectProps<std::string>{
    .items = {"claude-opus-4-6", "claude-sonnet-4-6", "claude-haiku-4-5"},
    .selected = 0,
    .label = "Model",
});

static ActivityBar g_activity(ActivityBarProps{
    .plan_items = {
        {.text = "Analyze auth module", .status = TaskStatus::Completed},
        {.text = "Find session timeout bug", .status = TaskStatus::Completed},
        {.text = "Apply fix", .status = TaskStatus::Completed},
        {.text = "Run tests", .status = TaskStatus::Pending},
    },
    .edits = {
        {.path = "src/auth/session.ts", .added = 1, .removed = 1},
    },
});

// ── Render helpers ───────────────────────────────────────────────────────────

static Element render_message(const Message& msg) {
    std::vector<Element> parts;

    // Thinking block
    if (msg.has_thinking && !msg.thinking.empty()) {
        auto tb = ThinkingBlock(ThinkingBlockProps{
            .content = msg.thinking,
            .expanded = g_thinking_expanded,
            .max_lines = 6,
        });
        parts.push_back(tb.render(g_frame));
    }

    // Tool card
    if (msg.has_tool) {
        Children tool_children;
        if (msg.tool_name == "edit_file" || msg.tool_name == "grep") {
            // Show as code block
            tool_children.push_back(CodeBlock(CodeBlockProps{
                .code = msg.tool_content,
                .show_line_nums = false,
            }));
        } else {
            tool_children.push_back(
                text(msg.tool_content, Style{}.with_fg(palette().text)));
        }

        parts.push_back(ToolCard(ToolCardProps{
            .title = msg.tool_title,
            .status = msg.tool_status,
            .frame = g_frame,
            .children = std::move(tool_children),
            .tool_name = msg.tool_name,
        }));
    }

    // Message content (markdown)
    if (!msg.content.empty()) {
        parts.push_back(MessageBubble(MessageBubbleProps{
            .role = msg.role,
            .children = { Markdown(MarkdownProps{.source = msg.content}) },
        }));
    } else if (parts.empty()) {
        // Empty message — just show the bubble
        parts.push_back(MessageBubble(MessageBubbleProps{.role = msg.role}));
    }

    if (parts.size() == 1) return std::move(parts[0]);
    return vstack().gap(0)(std::move(parts));
}

static Element render_agent_tab() {
    std::vector<Element> rows;

    // Messages
    for (auto& msg : g_messages) {
        rows.push_back(render_message(msg));
        rows.push_back(text(""));  // spacing
    }

    // Input area
    rows.push_back(g_input.render());

    // Keyhints
    rows.push_back(KeyBindings({
        {.keys = "Enter", .label = "send"},
        {.keys = "t", .label = "thinking"},
        {.keys = "Tab", .label = "tabs"},
        {.keys = "q", .label = "quit"},
    }));

    return vstack().gap(0).padding(1, 1, 0, 1)(std::move(rows));
}

static Element render_history_tab() {
    return vstack().padding(1)(
        text("Thread History", Style{}.with_bold().with_fg(palette().primary)),
        text(""),
        text("  Today", Style{}.with_bold().with_fg(palette().muted)),
        text("  ▸ Fix session timeout bug           2m ago",
             Style{}.with_fg(palette().text)),
        text("  ▸ Refactor database queries          1h ago",
             Style{}.with_fg(palette().muted)),
        text(""),
        text("  Yesterday", Style{}.with_bold().with_fg(palette().muted)),
        text("  ▸ Add rate limiting middleware        23h ago",
             Style{}.with_fg(palette().muted)),
        text("  ▸ Update deployment config            1d ago",
             Style{}.with_fg(palette().muted))
    );
}

static Element render_config_tab() {
    return vstack().padding(1).gap(1)(
        text("Configuration", Style{}.with_bold().with_fg(palette().primary)),
        text(""),
        g_model.render(),
        text(""),
        Callout(CalloutProps{
            .severity = Severity::Info,
            .title = "API Key configured",
            .body = "Using ANTHROPIC_API_KEY from environment",
        })
    );
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main() {
    g_input.focus();

    run(
        {.title = "maya agent", .fps = 15, .mouse = true},

        [&](const Event& ev) -> bool {
            if (key(ev, 'q') && !g_input.focused()) return false;
            if (key(ev, SpecialKey::Escape)) return false;

            // Tab switching
            if (key(ev, SpecialKey::Tab)) {
                g_tabs.next();
                return true;
            }

            // Toggle thinking blocks
            on(ev, 't', [&] {
                if (!g_input.focused())
                    g_thinking_expanded = !g_thinking_expanded;
            });

            // Model selector (when config tab)
            if (g_tabs.active() == 2) {
                g_model.update(ev);
            }

            // Text input
            g_input.update(ev);

            return true;
        },

        [&](const Ctx& ctx) -> Element {
            ++g_frame;

            // Main layout: tab bar + content + status bar
            std::vector<Element> content;

            // Tab bar
            content.push_back(g_tabs.render());
            content.push_back(Divider());

            // Active tab content
            switch (g_tabs.active()) {
                case 0: content.push_back(render_agent_tab()); break;
                case 1: content.push_back(render_history_tab()); break;
                case 2: content.push_back(render_config_tab()); break;
            }

            // Spacer to push status bar down
            content.push_back(Element(space));

            // Status bar
            content.push_back(StatusBar(StatusBarProps{
                .left = "maya agent",
                .center = "claude-opus-4-6 · 1.2k tokens",
                .right = "[q]uit  [Tab]switch",
            }));

            return vstack()(std::move(content));
        }
    );
}
