// examples/chat.cpp — AI coding assistant demo (DSL version)
//
// Composes individual widgets into a Claude Code-style chat interface
// using maya's compile-time DSL with dyn() escape hatches for runtime state.
//
// Runs in inline mode — shell history above, native terminal scroll.

#include <maya/maya.hpp>

#include <string>
#include <string_view>
#include <vector>

using namespace maya;
using namespace maya::dsl;

// ── Style presets ────────────────────────────────────────────────────────

constexpr auto user_label  = Bold | Fg<100, 200, 255>;
constexpr auto asst_label  = Bold | Fg<180, 140, 255>;
constexpr auto input_fg    = Fg<220, 220, 240>;
constexpr auto prompt_fg   = Fg<100, 200, 255>;
constexpr auto status_ok   = Fg<80, 220, 120>;
constexpr auto status_busy = Fg<255, 200, 60>;

// ── Message model ────────────────────────────────────────────────────────

enum class Role : uint8_t { User, Assistant, Tool };

struct Message {
    Role role;
    std::string content;
    std::string tool_name;
    StreamingMarkdown md;
    bool streaming = false;
};

// ── Simulated responses ──────────────────────────────────────────────────

struct Turn {
    std::vector<std::string> tokens;
    std::vector<std::pair<std::string, std::string>> tool_calls;
    float think_time      = 0.0f;
    bool needs_permission = false;
    bool show_diff        = false;
    bool show_tree        = false;
};

static const Turn turns[] = {
    {.tokens = {"I", "'ll", " look", " at", " the", " project", " first", "."},
     .tool_calls = {{"read_file", "Path: CMakeLists.txt"}},
     .think_time = 1.5f,
     .needs_permission = true},

    {.tokens = {"## Project Structure\n\n",
         "| Component | Description |\n",
         "|-----------|-------------|\n",
         "| `src/render/` | SIMD-accelerated canvas diffing |\n",
         "| `src/layout/` | Flexbox layout engine |\n",
         "| `src/widget/` | Markdown, Input, Spinner, Table |\n",
         "| `src/app/` | Event loop, inline rendering |\n\n",
         "Built", " with", " **CMake**", " +", " `g++-15`", "."},
     .think_time = 1.5f,
     .show_tree = true},

    {.tokens = {"Running", " tests", "..."},
     .tool_calls = {{"bash", "ctest --output-on-failure"}},
     .think_time = 0.8f,
     .needs_permission = true},

    {.tokens = {"All", " **18 tests**", " pass", ". \xe2\x9c\x93\n\n",
         "The", " row-hash", " diffing", ":\n\n",
         "```cpp\n",
         "for (int y = 0; y < ch; ++y) {\n",
         "    uint64_t h = simd::hash_row(cells + y * W, W);\n",
         "    if (y == stable && h == prev_hashes[y])\n",
         "        stable = y + 1;\n",
         "}\n",
         "```\n\n",
         "Stable", " rows", " skip", " re-rendering", "."},
     .think_time = 1.0f},

    {.tokens = {"I", " improved", " the", " damage", " tracking", ":"},
     .tool_calls = {{"edit", "src/render/canvas.cpp:12-15"}},
     .think_time = 1.0f,
     .needs_permission = true,
     .show_diff = true},

    {.tokens = {"## Summary\n\n",
         "- \xe2\x9c\x85 Tests: **18/18** passing\n",
         "- \xf0\x9f\x9a\x80 Tighter damage rects reduce diff work\n",
         "- \xf0\x9f\x94\x92 No behavioral change for full repaints\n"},
     .think_time = 0.6f},
};

// ── Phases ───────────────────────────────────────────────────────────────

enum class Phase { Input, Thinking, Permission, Tools, Streaming, Done };

// ── Main ─────────────────────────────────────────────────────────────────

int main() {
    FocusScope scope;
    Input<> input;
    scope.focus_index(0);
    input.set_placeholder("Type a message...");

    // State
    std::vector<Message> messages;
    Phase phase       = Phase::Input;
    int turn_idx      = 0;
    int token_idx     = 0;
    int tool_idx      = 0;
    float token_timer = 0.0f;
    float think_timer = 0.0f;
    constexpr float kTokenRate = 0.025f;

    // Widgets
    ThinkingBlock thinking({.show_content = true, .show_border = true});
    Spinner<SpinnerStyle::Dots> spinner;
    ToastManager toasts;

    // Extra widget elements (built once after streaming)
    Element diff_elem{TextElement{}};
    Element tree_elem{TextElement{}};
    bool show_diff = false;
    bool show_tree = false;

    // Submit handler
    input.on_submit([&](std::string_view sv) {
        if (sv.empty()) return;
        if (turn_idx >= static_cast<int>(std::size(turns))) {
            toasts.push("No more responses", ToastLevel::Warning);
            return;
        }

        messages.push_back({Role::User, std::string{sv}, {}, {}, false});
        input.clear();

        show_diff = false;
        show_tree = false;

        auto& turn = turns[turn_idx];
        if (turn.think_time > 0.0f) {
            phase = Phase::Thinking;
            think_timer = 0.0f;
            thinking.set_active(true);
            thinking.set_content("");
            thinking.append("Analyzing the request...\n");
        } else {
            phase = Phase::Tools;
            tool_idx = 0;
        }
    });

    maya::run(
        {.fps = 30, .mode = maya::Mode::Inline},

        // ── Events ───────────────────────────────────────────────
        [&](const Event& ev) {
            if (ctrl(ev, 'c') || ctrl(ev, 'd')) return false;

            if (auto* ke = as_key(ev)) {
                if (phase == Phase::Permission) {
                    if (key(ev, 'y') || key(ev, 'Y')) {
                        phase = Phase::Tools;
                        tool_idx = 0;
                    } else if (key(ev, 'n') || key(ev, 'N')) {
                        toasts.push("Denied", ToastLevel::Error);
                        phase = Phase::Input;
                    }
                    return true;
                }
                if (phase == Phase::Input)
                    (void)input.handle(*ke);
            }
            return true;
        },

        // ── Render ───────────────────────────────────────────────
        [&]() -> Element {
            constexpr float dt = 1.0f / 30.0f;
            spinner.advance(dt);
            thinking.advance(dt);
            toasts.advance(dt);

            if (turn_idx < static_cast<int>(std::size(turns))) {
                auto& turn = turns[turn_idx];

                // ── Thinking ─────────────────────────────────────
                if (phase == Phase::Thinking) {
                    think_timer += dt;
                    if (think_timer > 0.5f && think_timer - dt <= 0.5f)
                        thinking.append("Reading relevant files...\n");
                    if (think_timer > 1.0f && think_timer - dt <= 1.0f)
                        thinking.append("Forming response...\n");

                    if (think_timer >= turn.think_time) {
                        thinking.set_active(false);
                        thinking.set_expanded(false);

                        if (turn.needs_permission && !turn.tool_calls.empty()) {
                            phase = Phase::Permission;
                        } else if (!turn.tool_calls.empty()) {
                            phase = Phase::Tools;
                            tool_idx = 0;
                        } else {
                            messages.push_back({Role::Assistant, {}, {}, {}, true});
                            token_idx = 0;
                            token_timer = 0.0f;
                            phase = Phase::Streaming;
                        }
                    }
                }

                // ── Tools ────────────────────────────────────────
                if (phase == Phase::Tools) {
                    if (tool_idx < static_cast<int>(turn.tool_calls.size())) {
                        auto& [name, content] =
                            turn.tool_calls[static_cast<size_t>(tool_idx)];
                        messages.push_back({Role::Tool, std::string{content},
                                           std::string{name}, {}, false});
                        ++tool_idx;
                    }
                    if (tool_idx >= static_cast<int>(turn.tool_calls.size())) {
                        messages.push_back({Role::Assistant, {}, {}, {}, true});
                        token_idx = 0;
                        token_timer = 0.0f;
                        phase = Phase::Streaming;
                    }
                }

                // ── Streaming ────────────────────────────────────
                if (phase == Phase::Streaming) {
                    token_timer += dt;
                    while (token_timer >= kTokenRate &&
                           token_idx < static_cast<int>(turn.tokens.size())) {
                        auto& msg = messages.back();
                        auto& tok = turn.tokens[static_cast<size_t>(token_idx)];
                        msg.content += tok;
                        msg.md.append(tok);
                        ++token_idx;
                        token_timer -= kTokenRate;
                    }
                    if (token_idx >= static_cast<int>(turn.tokens.size())) {
                        messages.back().md.finish();
                        messages.back().streaming = false;
                        toasts.push("Response complete", ToastLevel::Success);

                        if (turn.show_diff) {
                            show_diff = true;
                            diff_elem = DiffView("src/render/canvas.cpp",
                                "@@ -12,4 +12,6 @@\n"
                                " void Canvas::begin_frame() {\n"
                                "-    damage_ = {0, 0, width_, height_};\n"
                                "-    // Always diff the entire canvas\n"
                                "+    damage_ = {0, 0, 0, 0};\n"
                                "+    // Start with empty damage rect\n"
                                "+    // Grows as cells are written\n"
                                " }\n").build();
                        }
                        if (turn.show_tree) {
                            show_tree = true;
                            TreeView tv({.show_icons = true});
                            tv.add("maya/", {
                                TreeView::dir("src/", {
                                    TreeView::dir("app/", {
                                        TreeView::leaf("app.cpp"),
                                        TreeView::leaf("inline.cpp")}),
                                    TreeView::dir("render/", {
                                        TreeView::leaf("canvas.cpp"),
                                        TreeView::leaf("diff.cpp")}),
                                    TreeView::dir("widget/", {
                                        TreeView::leaf("markdown.cpp"),
                                        TreeView::leaf("input.hpp")})}),
                                TreeView::dir("include/maya/", {
                                    TreeView::leaf("maya.hpp"),
                                    TreeView::leaf("dsl.hpp")}),
                                TreeView::leaf("CMakeLists.txt")});
                            tree_elem = tv.build();
                        }

                        ++turn_idx;
                        phase = Phase::Input;
                    }
                }
            }

            // ── Build UI with DSL ────────────────────────────────

            // Messages → vector<Element>
            // Capture by index (not range-for ref) so dyn() lambdas
            // don't hold dangling references to the loop variable.
            std::vector<Element> msg_elems;
            for (size_t i = 0; i < messages.size(); ++i) {
                msg_elems.push_back(blank().build());
                switch (messages[i].role) {
                case Role::User:
                    msg_elems.push_back(
                        (text("  You") | user_label).build());
                    msg_elems.push_back(
                        markdown(messages[i].content));
                    break;

                case Role::Assistant:
                    msg_elems.push_back(
                        h(text("\xe2\x97\x86 ") | asst_label,   // ◆
                          text("Assistant") | asst_label).build());
                    if (messages[i].streaming) {
                        msg_elems.push_back(messages[i].md.build());
                    } else {
                        msg_elems.push_back(
                            markdown(messages[i].content));
                    }
                    break;

                case Role::Tool:
                    msg_elems.push_back(
                        vstack()
                            .border(BorderStyle::Round)
                            .border_color(Color::rgb(50, 50, 70))
                            .border_text(messages[i].tool_name, BorderTextPos::Top)
                            .padding(0, 1, 0, 1)(
                                (text(messages[i].content) | Dim).build()));
                    break;
                }
            }

            // Input line: "❯ " + text + cursor
            auto val = input.value()();
            int cur  = input.cursor()();
            std::string before = val.substr(0, static_cast<size_t>(cur));
            std::string after  = (cur < static_cast<int>(val.size()))
                ? val.substr(static_cast<size_t>(cur)) : "";

            auto input_row = [&]() -> Element {
                std::vector<Element> parts;
                parts.push_back((text("\xe2\x9d\xaf ") | prompt_fg).build());
                if (!before.empty())
                    parts.push_back((text(std::move(before)) | input_fg).build());
                if (!after.empty()) {
                    parts.push_back(
                        (text(after.substr(0, 1)) | Inverse).build());
                    if (after.size() > 1)
                        parts.push_back(
                            (text(after.substr(1)) | input_fg).build());
                } else {
                    parts.push_back(
                        (text("\xe2\x96\x88") | prompt_fg).build());  // █
                }
                return hstack()(std::move(parts));
            };

            // Compose the full UI — flat vector avoids blank rows
            // from when(false, ...) producing height-1 BlankNodes.
            std::vector<Element> ui;
            ui.reserve(msg_elems.size() + 8);
            for (auto& e : msg_elems) ui.push_back(std::move(e));

            if (phase == Phase::Streaming) {
                ui.push_back(
                    h(t<"  ">,
                      dyn([&] { return spinner.build(); }),
                      t<" Generating..."> | Dim | Italic).build());
            }

            if (phase == Phase::Thinking) {
                ui.push_back(thinking.build());
            }

            if (phase == Phase::Permission &&
                turn_idx < static_cast<int>(std::size(turns))) {
                auto& tc = turns[turn_idx];
                if (!tc.tool_calls.empty()) {
                    auto& [name, content] = tc.tool_calls[0];
                    ui.push_back(Permission(name, content).build());
                }
            }

            if (!toasts.empty()) {
                ui.push_back(toasts.build());
            }

            if (show_diff) {
                ui.push_back(
                    v(h(t<"  "> | Dim,
                        dyn([&] { return FileRef("src/render/canvas.cpp", 12).build(); })),
                      dyn([&] { return diff_elem; })
                    ).build());
            }
            if (show_tree) {
                ui.push_back(tree_elem);
            }

            // ─── divider + input + status ───
            ui.push_back((t<"\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"> | Dim).build());
            ui.push_back(input_row());
            ui.push_back(
                phase == Phase::Streaming
                    ? (text("\xe2\x97\x8f streaming") | status_busy).build()
                    : (text("\xe2\x97\x8f ready") | status_ok).build());

            return vstack()(std::move(ui));
        }
    );
}
