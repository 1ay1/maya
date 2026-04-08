// examples/chat.cpp — AI coding session demo
//
// Runs in inline mode (alt_screen = false) like Claude Code:
//   - Shell history stays visible above
//   - Old content pushed to terminal scrollback (native scroll)
//   - Live region at bottom is erased and redrawn each frame
//   - Resize reflows the live region; scrollback is preserved
//
// Simulates a multi-turn AI coding session with:
//   - Tool calls (read_file, bash, edit)
//   - Streaming markdown with code blocks, tables, lists
//   - Toast notifications
//   - Progressive token delivery

#include <maya/maya.hpp>

#include <string>
#include <string_view>
#include <vector>

using namespace maya;

// ── Simulated AI response ─────────────────────────────────────────────────

struct SimulatedResponse {
    std::vector<std::string> tokens;
    std::vector<std::pair<std::string, std::string>> tool_calls;
};

// A realistic multi-turn coding session -----------------------------------

static const SimulatedResponse responses[] = {

    // ── Turn 1: User asks about the project ──────────────────────────
    {
        {"I", "'ll", " take", " a", " look", " at", " the", " project",
         " structure", " first", "."},
        {{"read_file",
          "Path: CMakeLists.txt\n"
          "Lines: 45\n"
          "Language: CMake"}}
    },

    // ── Turn 2: Analysis with rich markdown ──────────────────────────
    {
        {"Here", "'s", " what", " I", " found", " in", " your",
         " project", ":\n\n",
         "## Project Structure\n\n",
         "This", " is", " a", " **C++26**", " TUI", " framework",
         " called", " `maya`", ".", " Key", " details", ":\n\n",
         "| Component | Description |\n",
         "|-----------|-------------|\n",
         "| `src/render/` | SIMD-accelerated canvas diffing |\n",
         "| `src/layout/` | Flexbox layout engine (Yoga-style) |\n",
         "| `src/widget/` | Chat, Markdown, Table, Input, Scroll |\n",
         "| `src/terminal/` | Type-state terminal RAII |\n",
         "| `src/app/` | Event loop, inline & alt-screen rendering |\n\n",
         "The", " build", " system", " uses", " **CMake**", " with",
         " `g++-15`", " on", " macOS", ".", " Let", " me", " check",
         " the", " test", " suite", "."},
        {}
    },

    // ── Turn 3: Run tests ────────────────────────────────────────────
    {
        {"Running", " the", " test", " suite", " now", "."},
        {{"bash",
          "Command: ctest --output-on-failure\n"
          "Exit: 0\n"
          "Output: 18/18 tests passed"}}
    },

    // ── Turn 4: Test results + code suggestion ───────────────────────
    {
        {"All", " **18", " tests**", " pass", ".", " \xe2\x9c\x93\n\n",
         "Now", " let", " me", " look", " at", " the", " rendering",
         " pipeline", ".", " The", " inline", " mode", " uses", " a",
         " clever", " row-hash", " comparison", " to", " minimize",
         " redraws", ":\n\n",
         "```cpp\n",
         "// Row-hash comparison — only re-render changed rows\n",
         "for (int y = 0; y < ch; ++y) {\n",
         "    uint64_t h = simd::hash_row(cells + y * W, W);\n",
         "    row_hashes_[y] = h;\n",
         "    if (y == stable && y < check && y < prev_sz\n",
         "        && h == prev_row_hashes_[y]) {\n",
         "        stable = y + 1;\n",
         "    }\n",
         "}\n",
         "```\n\n",
         "Stable", " rows", " at", " the", " top", " are", " committed",
         " to", " scrollback", " and", " never", " redrawn", ".",
         " This", " is", " what", " makes", " inline", " mode",
         " efficient", " even", " with", " large", " outputs", "."},
        {}
    },

    // ── Turn 5: Edit suggestion with diff ────────────────────────────
    {
        {"I", " noticed", " a", " potential", " improvement", ".",
         " The", " canvas", " damage", " tracking", " could", " be",
         " tighter", "."},
        {{"edit",
          "File: src/render/canvas.cpp\n"
          "Action: Replace lines 12-15\n"
          "Status: Applied"}}
    },

    // ── Turn 6: Explanation with nested formatting ───────────────────
    {
        {"Here", "'s", " what", " I", " changed", ":\n\n",
         "### Before\n",
         "The", " damage", " rect", " was", " initialized", " to",
         " the", " **full", " canvas**", ",", " causing",
         " unnecessary", " diff", " work", " on", " blank",
         " regions", ".\n\n",
         "### After\n",
         "Now", " the", " damage", " rect", " starts", " empty",
         " and", " grows", " only", " when", " cells", " are",
         " actually", " written", ".", " This", " means", ":\n\n",
         "1. ", "**Partial", " updates**", " only", " diff", " the",
         " region", " that", " changed\n",
         "2. ", "**Empty", " frames**", " skip", " diffing",
         " entirely\n",
         "3. ", "**First", " render**", " still", " diffs",
         " everything", " (full", " canvas", " is", " dirty", ")\n\n",
         "> The", " SIMD", " hash_row", " function", " processes",
         " 64", " bytes", " at", " a", " time\n",
         "> using", " ARM", " NEON", ",", " so", " even", " full",
         "-canvas", " diffs", " are", " fast", ".\n\n",
         "Let", " me", " verify", " the", " tests", " still",
         " pass", " after", " the", " change", "."},
        {}
    },

    // ── Turn 7: Final verification ───────────────────────────────────
    {
        {"Re-running", " tests", "..."},
        {{"bash",
          "Command: ctest --output-on-failure\n"
          "Exit: 0\n"
          "Output: 18/18 tests passed"}}
    },

    // ── Turn 8: Summary ──────────────────────────────────────────────
    {
        {"All", " tests", " pass", ".", " \xe2\x9c\x93\n\n",
         "## Summary\n\n",
         "I", " made", " one", " change", " to",
         " `src/render/canvas.cpp`", " that", " improves",
         " damage", " tracking", ":", "\n\n",
         "- ", "\xe2\x9c\x85", " Tests", ":", " **18/18**", " passing\n",
         "- ", "\xf0\x9f\x9a\x80", " Performance", ":",
         " tighter", " damage", " rects", " reduce", " diff", " work\n",
         "- ", "\xf0\x9f\x94\x92", " Safety", ":",
         " no", " behavioral", " change", " for", " full", " repaints\n\n",
         "The", " inline", " rendering", " pipeline", " is", " solid",
         " \xe2\x80\x94", " the", " row-hash", " comparison",
         " combined", " with", " SIMD", " diffing", " gives",
         " you", " fast", " updates", " without", " flicker", "."},
        {}
    },
};

// ── Main ──────────────────────────────────────────────────────────────────

int main() {
    FocusScope scope;
    ChatView chat;
    scope.focus_index(0);

    int response_idx = 0;
    int token_idx    = 0;
    int tool_idx     = 0;
    float token_timer = 0.0f;
    bool feeding_tools = false;
    constexpr float token_interval = 0.025f;

    chat.on_submit([&](std::string_view /*text*/) {
        if (response_idx >= static_cast<int>(std::size(responses))) {
            chat.toast("Session complete — no more responses", ToastLevel::Warning);
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
            if (feeding_tools &&
                response_idx < static_cast<int>(std::size(responses))) {
                auto& resp = responses[response_idx];
                if (tool_idx < static_cast<int>(resp.tool_calls.size())) {
                    auto& [name, content] =
                        resp.tool_calls[static_cast<size_t>(tool_idx)];
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
            if (chat.is_streaming() &&
                response_idx < static_cast<int>(std::size(responses))) {
                auto& resp = responses[response_idx];
                token_timer += dt;
                while (token_timer >= token_interval &&
                       token_idx < static_cast<int>(resp.tokens.size())) {
                    chat.stream_append(
                        resp.tokens[static_cast<size_t>(token_idx)]);
                    ++token_idx;
                    token_timer -= token_interval;
                }
                if (token_idx >= static_cast<int>(resp.tokens.size())) {
                    chat.end_stream();
                    ++response_idx;

                    // Auto-feed the next turn's tool calls if any
                    if (response_idx < static_cast<int>(std::size(responses))) {
                        auto& next = responses[response_idx];
                        if (!next.tool_calls.empty()) {
                            // Don't auto-start — wait for user message
                        }
                    }

                    chat.toast("Response complete", ToastLevel::Success);
                }
            }

            return chat.build();
        }
    );
}
