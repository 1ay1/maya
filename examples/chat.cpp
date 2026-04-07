// examples/chat.cpp — AI chat interface using ChatView widget
//
// Demonstrates:
//   - ChatView with streaming markdown, tool calls, toasts
//   - Inline mode (alt_screen = false) that promotes on resize
//   - Simulated AI responses with progressive token streaming

#include <maya/maya.hpp>

#include <string>
#include <string_view>
#include <vector>

using namespace maya;

// ── Simulated AI response ──────────────────────────────────────────────────

struct SimulatedResponse {
    std::vector<std::string> tokens;
    std::vector<std::pair<std::string, std::string>> tool_calls;  // {name, content}
};

static const SimulatedResponse responses[] = {
    {
        {"I", "'ll", " help", " you", " with", " that", "!\n\n",
         "Here", "'s", " a", " quick", " overview", ":\n\n",
         "- ", "**Maya**", " is", " a", " C++26", " TUI", " framework", "\n",
         "- ", "It", " uses", " **compile-time**", " DSL", " for", " type-safe", " UI", "\n",
         "- ", "SIMD", "-accelerated", " terminal", " diffing", "\n",
         "- ", "Flexbox", " layout", " engine", "\n\n",
         "The", " framework", " is", " designed", " for", " **high-performance**",
         " terminal", " applications", "."},
        {}
    },
    {
        {"Let", " me", " check", " the", " project", " structure", ".\n\n",
         "I", " found", " the", " following", ":\n\n",
         "```cpp\n",
         "maya::run(\n",
         "    {.alt_screen = false},\n",
         "    event_handler,\n",
         "    render_fn\n",
         ");\n",
         "```\n\n",
         "This", " starts", " in", " **inline mode**", " and",
         " automatically", " promotes", " to", " alt", " screen",
         " on", " resize", "."},
        {{"read_file", "Path: CMakeLists.txt\nLanguage: C++\nStandard: C++26\nCompiler: g++-15"}}
    },
    {
        {"Sure", "!", " Here", " are", " the", " available", " widgets", ":\n\n",
         "| Widget | Purpose |\n",
         "|--------|--------|\n",
         "| `Input` | Text input with cursor |\n",
         "| `Scroll` | Scrollable viewport |\n",
         "| `Markdown` | Rich text rendering |\n",
         "| `ChatView` | This chat interface |\n",
         "| `Table` | Tabular data |\n",
         "| `Spinner` | Loading animation |\n\n",
         "All", " widgets", " are", " **automatically responsive**",
         " \xe2\x80\x94 ", "they", " reflow", " on", " resize", "."},
        {}
    },
};

// ── Main ───────────────────────────────────────────────────────────────────

int main() {
    // FocusScope must be created before ChatView so the Input's FocusNode
    // registers with this scope.
    FocusScope scope;
    ChatView chat;

    // Focus the input immediately
    scope.focus_index(0);

    // Streaming state
    int response_idx = 0;
    int token_idx = 0;
    int tool_idx = 0;
    float token_timer = 0.0f;
    bool feeding_tools = false;
    constexpr float token_interval = 0.03f;

    chat.on_submit([&](std::string_view /*text*/) {
        if (response_idx >= static_cast<int>(std::size(responses))) {
            chat.toast("No more canned responses", ToastLevel::Warning);
            return;
        }

        auto& resp = responses[response_idx];

        if (!resp.tool_calls.empty()) {
            feeding_tools = true;
            tool_idx = 0;
        } else {
            chat.begin_stream();
            token_idx = 0;
            token_timer = 0.0f;
        }
    });

    maya::run(
        {.fps = 30, .mouse = true, .alt_screen = false},

        [&](const Event& ev) {
            if (ctrl(ev, 'c') || ctrl(ev, 'd')) return false;

            if (auto* ke = as_key(ev); ke)
                (void)chat.handle(*ke);
            if (auto* me = as_mouse(ev); me)
                (void)chat.handle(*me);

            return true;
        },

        [&](const Ctx& /*ctx*/) -> Element {
            constexpr float dt = 1.0f / 30.0f;
            chat.advance(dt);

            // Feed tool calls one per frame
            if (feeding_tools && response_idx < static_cast<int>(std::size(responses))) {
                auto& resp = responses[response_idx];
                if (tool_idx < static_cast<int>(resp.tool_calls.size())) {
                    auto& [name, content] = resp.tool_calls[static_cast<size_t>(tool_idx)];
                    chat.add_tool(name, content);
                    ++tool_idx;
                }
                if (tool_idx >= static_cast<int>(resp.tool_calls.size())) {
                    feeding_tools = false;
                    chat.begin_stream();
                    token_idx = 0;
                    token_timer = 0.0f;
                }
            }

            // Stream tokens progressively
            if (chat.is_streaming() && response_idx < static_cast<int>(std::size(responses))) {
                auto& resp = responses[response_idx];
                token_timer += dt;
                while (token_timer >= token_interval &&
                       token_idx < static_cast<int>(resp.tokens.size())) {
                    chat.stream_append(resp.tokens[static_cast<size_t>(token_idx)]);
                    ++token_idx;
                    token_timer -= token_interval;
                }
                if (token_idx >= static_cast<int>(resp.tokens.size())) {
                    chat.end_stream();
                    chat.toast("Response complete", ToastLevel::Success);
                    ++response_idx;
                }
            }

            return chat.build();
        }
    );
}
