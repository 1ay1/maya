// maya — AI agent terminal interface (inline, no alt screen)
//
// Simulates an AI coding agent that thinks, calls tools, and streams
// responses — all rendered inline in your terminal using maya's element
// tree and serialize() pipeline. No fullscreen takeover.
//
// This demonstrates maya as a formatting engine for CLI tools:
//   Canvas + render_tree + serialize → styled ANSI on stdout
//
// Keys: enter = next step   q = quit

#include <maya/maya.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef __unix__
#include <sys/ioctl.h>
#include <unistd.h>
#endif

using namespace maya;

// ── Data model ───────────────────────────────────────────────────────────────

enum class BlockKind { Thinking, ToolCall, ToolResult, Text, Cost };

struct Block {
    BlockKind   kind;
    std::string label;      // tool name, "thinking", etc
    std::string content;    // full content (streamed char by char)
    int         chars_shown; // how many chars revealed so far
    bool        done;
    float       duration;   // seconds (for timing display)
};

struct AgentState {
    std::string query;
    std::vector<Block> blocks;
    int   phase;            // which block we're building
    float phase_time;       // time in current phase
    float total_time;
    bool  finished;
    int   spinner_frame;
    int   tokens_in, tokens_out;
    float cost;
};

// ── Spinner ──────────────────────────────────────────────────────────────────

static const char* kSpinners[] = {
    "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
    "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7",
    "\xe2\xa0\x87", "\xe2\xa0\x8f",
};
static constexpr int kSpinnerCount = 10;

// ── Scenario ─────────────────────────────────────────────────────────────────

static void init_scenario(AgentState& st) {
    st.query = "Fix the failing test in auth_middleware.rs — it panics on expired JWT tokens";
    st.tokens_in = 2847;
    st.tokens_out = 0;
    st.cost = 0.f;

    // Phase 0: Thinking
    st.blocks.push_back({
        .kind = BlockKind::Thinking,
        .label = "thinking",
        .content = "The test `test_expired_jwt_returns_401` is panicking instead of returning "
                   "a 401 response. This is likely because the JWT validation function calls "
                   "`.unwrap()` on the decode result instead of handling the `ExpiredSignature` "
                   "error variant. I need to look at the middleware handler and the test to "
                   "confirm.",
        .chars_shown = 0, .done = false, .duration = 0,
    });

    // Phase 1: Read tool call
    st.blocks.push_back({
        .kind = BlockKind::ToolCall,
        .label = "Read",
        .content = "src/middleware/auth.rs",
        .chars_shown = 0, .done = false, .duration = 0,
    });

    // Phase 2: Tool result
    st.blocks.push_back({
        .kind = BlockKind::ToolResult,
        .label = "src/middleware/auth.rs",
        .content =
            "pub async fn auth_middleware(req: Request) -> Result<Request, Response> {\n"
            "    let token = req.header(\"Authorization\")\n"
            "        .and_then(|h| h.strip_prefix(\"Bearer \"));\n"
            "\n"
            "    let token = match token {\n"
            "        Some(t) => t,\n"
            "        None => return Err(unauthorized(\"missing token\")),\n"
            "    };\n"
            "\n"
            "    let claims = decode_jwt(token).unwrap();  // <-- panics here\n"
            "    req.set_claims(claims);\n"
            "    Ok(req)\n"
            "}",
        .chars_shown = 0, .done = false, .duration = 0,
    });

    // Phase 3: Edit tool call
    st.blocks.push_back({
        .kind = BlockKind::ToolCall,
        .label = "Edit",
        .content = "src/middleware/auth.rs:10\n"
                   "    let claims = decode_jwt(token).unwrap();\n"
                   "\xe2\x86\x92\n"
                   "    let claims = match decode_jwt(token) {\n"
                   "        Ok(c) => c,\n"
                   "        Err(e) if e.kind() == &ErrorKind::ExpiredSignature =>\n"
                   "            return Err(unauthorized(\"token expired\")),\n"
                   "        Err(e) => return Err(unauthorized(&e.to_string())),\n"
                   "    };",
        .chars_shown = 0, .done = false, .duration = 0,
    });

    // Phase 4: Bash tool call
    st.blocks.push_back({
        .kind = BlockKind::ToolCall,
        .label = "Bash",
        .content = "cargo test test_expired_jwt_returns_401",
        .chars_shown = 0, .done = false, .duration = 0,
    });

    // Phase 5: Tool result (test output)
    st.blocks.push_back({
        .kind = BlockKind::ToolResult,
        .label = "test output",
        .content =
            "running 1 test\n"
            "test middleware::auth::test_expired_jwt_returns_401 ... ok\n"
            "\n"
            "test result: ok. 1 passed; 0 failed; 0 ignored",
        .chars_shown = 0, .done = false, .duration = 0,
    });

    // Phase 6: Final response
    st.blocks.push_back({
        .kind = BlockKind::Text,
        .label = "",
        .content = "Fixed. The issue was `.unwrap()` on `decode_jwt()` at line 10 of "
                   "`auth.rs`. Expired tokens caused a panic instead of returning a 401. "
                   "I replaced it with a `match` that handles `ExpiredSignature` explicitly "
                   "and converts other errors to 401 responses. The test passes now.",
        .chars_shown = 0, .done = false, .duration = 0,
    });

    // Phase 7: Cost line
    st.blocks.push_back({
        .kind = BlockKind::Cost,
        .label = "",
        .content = "",
        .chars_shown = 0, .done = false, .duration = 0,
    });

    st.phase = 0;
    st.phase_time = 0;
    st.total_time = 0;
    st.finished = false;
    st.spinner_frame = 0;
}

// ── Simulation tick ──────────────────────────────────────────────────────────

static void tick(AgentState& st, float dt) {
    st.total_time += dt;
    st.spinner_frame = static_cast<int>(st.total_time * 10.f) % kSpinnerCount;

    if (st.finished || st.phase >= static_cast<int>(st.blocks.size())) {
        st.finished = true;
        return;
    }

    auto& b = st.blocks[static_cast<std::size_t>(st.phase)];
    st.phase_time += dt;
    b.duration = st.phase_time;

    // Stream characters
    int target = static_cast<int>(b.content.size());
    if (b.kind == BlockKind::Cost) {
        // Cost block is instant
        b.done = true;
        st.phase++;
        st.phase_time = 0;
        st.finished = true;
        return;
    }

    // Streaming speed varies by block type
    float chars_per_sec;
    switch (b.kind) {
        case BlockKind::Thinking:  chars_per_sec = 120.f; break;
        case BlockKind::ToolCall:  chars_per_sec = 200.f; break;
        case BlockKind::ToolResult: chars_per_sec = 500.f; break;
        case BlockKind::Text:      chars_per_sec = 100.f; break;
        default:                   chars_per_sec = 100.f; break;
    }

    int new_shown = static_cast<int>(st.phase_time * chars_per_sec);
    // Don't split mid-UTF8
    while (new_shown < target && (b.content[static_cast<std::size_t>(new_shown)] & 0xC0) == 0x80)
        new_shown++;
    b.chars_shown = std::min(new_shown, target);

    // Track tokens
    st.tokens_out = 0;
    for (auto& blk : st.blocks)
        st.tokens_out += blk.chars_shown / 4; // rough approx
    st.cost = (st.tokens_in * 3.f + st.tokens_out * 15.f) / 1000000.f;

    if (b.chars_shown >= target) {
        b.done = true;
        // Small pause before next phase
        if (st.phase_time > b.duration + 0.3f) {
            st.phase++;
            st.phase_time = 0;
        }
    }
}

// ── Build element tree ───────────────────────────────────────────────────────

static Element build_ui(const AgentState& st, int width) {
    std::vector<Element> rows;

    // ── User query ───────────────────────────────────────────────────────
    rows.push_back(
        hstack()(
            text("\xe2\x9d\xaf ", Style{}.with_bold().with_fg(Color::rgb(100, 200, 255))),
            text(st.query, Style{}.with_bold().with_fg(Color::rgb(220, 220, 240)))
        )
    );
    rows.push_back(text(""));

    // ── Blocks ───────────────────────────────────────────────────────────
    for (int i = 0; i <= st.phase && i < static_cast<int>(st.blocks.size()); ++i) {
        const auto& b = st.blocks[static_cast<std::size_t>(i)];
        std::string_view visible{b.content.data(),
            static_cast<std::size_t>(std::min(b.chars_shown, static_cast<int>(b.content.size())))};

        switch (b.kind) {
        case BlockKind::Thinking: {
            // Dimmed italic thinking block
            std::string header = b.done
                ? std::string("\xe2\x9c\x93 thinking (") + std::to_string(static_cast<int>(b.duration * 10) / 10) + "s)"
                : std::string(kSpinners[st.spinner_frame]) + " thinking...";

            rows.push_back(
                box().direction(FlexDirection::Column).padding(0, 0, 0, 2)(
                    text(header, b.done
                        ? Style{}.with_fg(Color::rgb(80, 80, 100))
                        : Style{}.with_fg(Color::rgb(180, 140, 255))),
                    text(std::string(visible), Style{}.with_italic().with_fg(Color::rgb(120, 120, 150)))
                )
            );
            rows.push_back(text(""));
            break;
        }

        case BlockKind::ToolCall: {
            Style tool_style = Style{}.with_bold().with_fg(Color::rgb(80, 200, 255));
            Style content_style = Style{}.with_fg(Color::rgb(200, 200, 220));

            std::string icon = b.done ? "\xe2\x9c\x93" : kSpinners[st.spinner_frame];
            Style icon_style = b.done
                ? Style{}.with_fg(Color::rgb(80, 220, 120))
                : Style{}.with_fg(Color::rgb(180, 140, 255));

            if (b.label == "Edit" || b.label == "Read" || b.label == "Bash") {
                // Tool call with content box
                rows.push_back(
                    hstack()(
                        text(icon + " ", icon_style),
                        text(b.label, tool_style)
                    )
                );

                if (b.chars_shown > 0) {
                    // Code-like content in a bordered box
                    rows.push_back(
                        box().direction(FlexDirection::Column)
                            .border(BorderStyle::Round)
                            .border_color(Color::rgb(50, 50, 70))
                            .padding(0, 1, 0, 1)
                            .width(std::min(width - 4, static_cast<int>(b.content.size()) + 4))(
                            text(std::string(visible), content_style)
                        )
                    );
                }
            } else {
                rows.push_back(
                    hstack()(
                        text(icon + " ", icon_style),
                        text(b.label + ": ", tool_style),
                        text(std::string(visible), content_style)
                    )
                );
            }
            rows.push_back(text(""));
            break;
        }

        case BlockKind::ToolResult: {
            if (b.chars_shown > 0) {
                Style label_style = Style{}.with_fg(Color::rgb(80, 80, 100));

                rows.push_back(
                    box().direction(FlexDirection::Column)
                        .border(BorderStyle::Round)
                        .border_color(Color::rgb(40, 50, 40))
                        .padding(0, 1, 0, 1)
                        .max_width(width - 2)(
                        text(std::string(visible), Style{}.with_fg(Color::rgb(160, 170, 160)))
                    )
                );
                rows.push_back(text(""));
            }
            break;
        }

        case BlockKind::Text: {
            rows.push_back(
                box().direction(FlexDirection::Column).padding(0)(
                    text(std::string(visible), Style{}.with_fg(Color::rgb(210, 210, 230)))
                )
            );
            rows.push_back(text(""));
            break;
        }

        case BlockKind::Cost: {
            if (st.finished) {
                char buf[80];
                std::snprintf(buf, sizeof buf,
                    "\xe2\x94\x80 %d in + %d out = $%.4f",
                    st.tokens_in, st.tokens_out, static_cast<double>(st.cost));
                rows.push_back(
                    text(buf, Style{}.with_fg(Color::rgb(70, 70, 90)))
                );
            }
            break;
        }
        }
    }

    // Active streaming indicator
    if (!st.finished) {
        // nothing — the spinner in the current block header is enough
    }

    return vstack().width(width)(std::move(rows));
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main() {
    AgentState state{};
    init_scenario(state);

    // Get terminal width (fallback 80)
    int term_w = 80;
    if (auto* cols = std::getenv("COLUMNS")) {
        term_w = std::atoi(cols);
        if (term_w < 40) term_w = 80;
    }
    // Try ioctl
    #ifdef __unix__
    {
        struct winsize ws{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
            term_w = ws.ws_col;
    }
    #endif

    int width = std::min(term_w, 100);

    StylePool pool;
    int prev_height = 0;
    std::string out;

    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();

    // Hide cursor
    std::fputs("\x1b[?25l", stdout);

    while (!state.finished) {
        auto now = Clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;

        tick(state, dt);

        // Build element tree
        Element root = build_ui(state, width);

        // Measure: how tall will this render?
        // We use a generous height and let layout compute actual.
        int render_h = 60; // max height
        Canvas canvas{width, render_h, &pool};
        render_tree(root, canvas, pool, theme::dark);

        // Find actual content height (last non-empty row)
        int actual_h = 0;
        for (int y = render_h - 1; y >= 0; --y) {
            bool empty = true;
            for (int x = 0; x < width; ++x) {
                auto cell = Cell::unpack(canvas.cells()[y * width + x]);
                if (cell.character != U' ' && cell.character != 0) { empty = false; break; }
            }
            if (!empty) { actual_h = y + 1; break; }
        }
        if (actual_h == 0) actual_h = 1;

        // Resize canvas to actual height for clean output
        Canvas trimmed{width, actual_h, &pool};
        for (int y = 0; y < actual_h; ++y)
            for (int x = 0; x < width; ++x) {
                auto cell = Cell::unpack(canvas.cells()[y * width + x]);
                trimmed.set(x, y, cell.character, cell.style_id);
            }

        // Erase previous output, write new
        out.clear();
        if (prev_height > 0) {
            ansi::erase_lines(prev_height, out);
        }
        serialize(trimmed, pool, out);

        std::fwrite(out.data(), 1, out.size(), stdout);
        std::fflush(stdout);
        prev_height = actual_h;

        std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30fps
    }

    // Final render (ensure everything visible)
    tick(state, 0.5f);
    {
        Element root = build_ui(state, width);
        int render_h = 60;
        Canvas canvas{width, render_h, &pool};
        render_tree(root, canvas, pool, theme::dark);

        int actual_h = 0;
        for (int y = render_h - 1; y >= 0; --y) {
            bool empty = true;
            for (int x = 0; x < width; ++x) {
                auto cell = Cell::unpack(canvas.cells()[y * width + x]);
                if (cell.character != U' ' && cell.character != 0) { empty = false; break; }
            }
            if (!empty) { actual_h = y + 1; break; }
        }
        if (actual_h == 0) actual_h = 1;

        Canvas trimmed{width, actual_h, &pool};
        for (int y = 0; y < actual_h; ++y)
            for (int x = 0; x < width; ++x) {
                auto cell = Cell::unpack(canvas.cells()[y * width + x]);
                trimmed.set(x, y, cell.character, cell.style_id);
            }

        out.clear();
        if (prev_height > 0) ansi::erase_lines(prev_height, out);
        serialize(trimmed, pool, out);
        std::fwrite(out.data(), 1, out.size(), stdout);
    }

    // Show cursor, newline
    std::fputs("\x1b[?25h\n", stdout);
    std::fflush(stdout);
}
