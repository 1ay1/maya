// chat.cpp — Claude Code-style chat UI demo
//
// Demonstrates maya's Ink-like features working together:
//   - Input widget with focus, cursor, and history
//   - Markdown rendering for assistant responses
//   - Spinner for streaming animation
//   - Inline mode (non-fullscreen, preserves scrollback)
//
// Controls:
//   Type + Enter   send a message
//   Ctrl+C / Esc   quit
//
// Usage:  ./maya_chat

#include <maya/maya.hpp>
#include <maya/dsl.hpp>
#include <maya/app/run.hpp>
#include <maya/app/events.hpp>

#include <chrono>
#include <string>
#include <vector>

using namespace maya::dsl;

// ── Message model ──────────────────────────────────────────────────────────

enum class Role { User, Assistant };

struct Message {
    Role role;
    std::string content;
    bool streaming = false;
    // Cached rendered element — avoids re-parsing markdown every frame
    std::optional<maya::Element> cached;
};

// ── Application state ──────────────────────────────────────────────────────

static std::vector<Message> messages;
static maya::Input<> input_widget;
static maya::Spinner<maya::SpinnerStyle::Dots> spinner;
static bool streaming = false;
static float stream_timer = 0.0f;
static int stream_char_idx = 0;

static const char* fake_responses[] = {
    "I can help you with that! Here's a **quick overview**:\n\n"
    "1. First, set up the project\n"
    "2. Then configure the build system\n"
    "3. Finally, run the tests\n\n"
    "Let me know if you'd like me to proceed.",

    "Here's a code example:\n\n"
    "```cpp\n"
    "#include <iostream>\n\n"
    "int main() {\n"
    "    std::cout << \"Hello!\" << std::endl;\n"
    "    return 0;\n"
    "}\n"
    "```\n\n"
    "This prints `Hello!` to the console.",

    "Key differences:\n\n"
    "- **Performance**: C++ is compiled\n"
    "- **Memory**: C++ has manual management\n"
    "- **Syntax**: Python is more concise\n\n"
    "> Both languages have their strengths.\n\n"
    "Want to explore further?",
};
static int response_idx = 0;

// ── Render a single message ────────────────────────────────────────────────

static maya::Element render_user_msg(const std::string& content) {
    using namespace maya;
    return detail::hstack()(
        Element{TextElement{
            .content = "❯ ",
            .style = Style{}.with_bold().with_fg(Color::rgb(100, 200, 255)),
        }},
        Element{TextElement{
            .content = content,
            .style = Style{}.with_fg(Color::rgb(220, 220, 240)),
        }}
    );
}

static maya::Element render_assistant_msg(const Message& msg) {
    using namespace maya;

    std::vector<Element> parts;
    parts.push_back(Element{TextElement{
        .content = "◆ Assistant",
        .style = Style{}.with_bold().with_fg(Color::rgb(180, 140, 255)),
    }});

    if (!msg.content.empty()) {
        parts.push_back(markdown(msg.content));
    }

    if (msg.streaming) {
        parts.push_back(detail::hstack()(
            spinner,
            Element{TextElement{
                .content = " Generating...",
                .style = Style{}.with_dim(),
            }}
        ));
    }

    return detail::vstack()(std::move(parts));
}

static maya::Element render_message(Message& msg) {
    // Use cached element for completed messages (avoids re-parsing markdown)
    if (msg.cached) return *msg.cached;

    maya::Element elem;
    if (msg.role == Role::User) {
        elem = render_user_msg(msg.content);
    } else {
        elem = render_assistant_msg(msg);
    }

    // Cache once streaming is done
    if (!msg.streaming) {
        msg.cached = elem;
    }

    return elem;
}

// ── Streaming ──────────────────────────────────────────────────────────────

static void start_stream() {
    streaming = true;
    stream_timer = 0.0f;
    stream_char_idx = 0;
    messages.push_back({Role::Assistant, "", true, std::nullopt});
}

static void advance_stream(float dt) {
    if (!streaming) return;

    spinner.advance(dt);
    stream_timer += dt;

    const std::string& full = fake_responses[response_idx % 3];

    int target = static_cast<int>(stream_timer * 80.0f);
    if (target > static_cast<int>(full.size()))
        target = static_cast<int>(full.size());

    if (stream_char_idx < target) {
        stream_char_idx = target;
        messages.back().content = full.substr(0, static_cast<size_t>(stream_char_idx));
    }

    if (stream_char_idx >= static_cast<int>(full.size())) {
        messages.back().streaming = false;
        streaming = false;
        ++response_idx;
    }
}

// ── Submit handler ─────────────────────────────────────────────────────────

static void on_submit(std::string_view text) {
    if (text.empty()) return;
    if (streaming) return;  // don't allow input while streaming
    messages.push_back({Role::User, std::string{text}, false, std::nullopt});
    input_widget.clear();
    start_stream();
}

// ── Main render ────────────────────────────────────────────────────────────

static maya::Element render() {
    using namespace maya;

    // Messages
    Element msg_area;
    if (messages.empty()) {
        msg_area = Element{TextElement{
            .content = "  Type a message to start chatting...",
            .style = Style{}.with_dim().with_italic(),
        }};
    } else {
        std::vector<Element> rendered;
        rendered.reserve(messages.size() * 2);
        for (auto& msg : messages) {
            rendered.push_back(render_message(msg));
            // Spacer between messages
            rendered.push_back(Element{TextElement{.content = ""}});
        }
        msg_area = detail::vstack()(std::move(rendered));
    }

    // Separator
    std::string sep_line;
    for (int i = 0; i < 60; ++i) sep_line += "─";

    // Input prompt
    auto prompt = detail::hstack()(
        Element{TextElement{
            .content = "❯ ",
            .style = Style{}.with_bold().with_fg(Color::rgb(100, 200, 255)),
        }},
        input_widget
    );

    // Status
    std::string status_text = streaming ? "streaming" : "ready";
    status_text += " │ " + std::to_string(messages.size()) + " msgs │ Esc: quit";

    return detail::vstack()(
        std::move(msg_area),
        Element{TextElement{
            .content = std::move(sep_line),
            .style = Style{}.with_fg(Color::rgb(60, 60, 80)),
        }},
        std::move(prompt),
        Element{TextElement{
            .content = std::move(status_text),
            .style = Style{}.with_dim(),
        }}
    );
}

// ── Main ───────────────────────────────────────────────────────────────────

int main() {
    input_widget.set_placeholder("Type a message...");
    input_widget.on_submit(on_submit);

    maya::FocusScope focus;
    input_widget.focus_node().bind_scope(focus);
    focus.focus_next();

    maya::run(
        {.title = "maya chat", .fps = 10, .alt_screen = false},
        [&](const maya::Event& ev) {
            maya::on(ev, maya::SpecialKey::Escape, [] { maya::quit(); });
            if (maya::ctrl(ev, 'c')) maya::quit();

            if (auto* ke = maya::as_key(ev)) {
                (void)input_widget.handle(*ke);
            }
        },
        [&]() -> maya::Element {
            using Clock = std::chrono::steady_clock;
            static auto last = Clock::now();
            auto now = Clock::now();
            float dt = std::chrono::duration<float>(now - last).count();
            last = now;

            advance_stream(dt);
            return render();
        }
    );

    return 0;
}
