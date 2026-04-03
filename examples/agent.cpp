// maya — AI coding agent (inline, no alt screen)
//
// Automatic playthrough of an AI agent fixing a bug: thinking, reading files,
// editing code, running tests — all rendered inline with streaming text,
// spinners, diff-colored edits, and bordered tool output.
//
// Demonstrates maya as a formatting engine for CLI tools:
//   Canvas + render_tree() + serialize() → styled ANSI on stdout

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

// ── Types ────────────────────────────────────────────────────────────────────

enum class Kind { Thinking, Tool, Result, Text, Divider };

struct Block {
    Kind        kind;
    std::string tool;       // tool name (Read, Edit, Bash)
    std::string content;    // full text
    float       speed;      // chars per second
    float       pause;      // seconds to wait after finishing
};

// ── Scenario ─────────────────────────────────────────────────────────────────

static const char* kQuery =
    "Fix the failing test in auth_middleware.rs \xe2\x80\x94 "
    "it panics on expired JWT tokens";

static std::vector<Block> make_scenario() {
    return {
        // Thinking
        {Kind::Thinking, "",
         "The test `test_expired_jwt_returns_401` panics instead of returning 401. "
         "The JWT validation likely calls `.unwrap()` on the decode result rather "
         "than handling `ExpiredSignature`. Let me check the middleware code.",
         160, 0.4f},

        // Read file
        {Kind::Tool, "Read", "src/middleware/auth.rs", 400, 0.2f},

        {Kind::Result, "",
         "pub async fn auth_middleware(req: Request) -> Result<Request, Response> {\n"
         "    let token = req.header(\"Authorization\")\n"
         "        .and_then(|h| h.strip_prefix(\"Bearer \"));\n"
         "\n"
         "    let token = match token {\n"
         "        Some(t) => t,\n"
         "        None => return Err(unauthorized(\"missing token\")),\n"
         "    };\n"
         "\n"
         "    let claims = decode_jwt(token).unwrap();  // BUG\n"
         "    req.set_claims(claims);\n"
         "    Ok(req)\n"
         "}",
         800, 0.5f},

        // Edit
        {Kind::Tool, "Edit", "src/middleware/auth.rs:10", 400, 0.2f},

        {Kind::Result, "",
         // red = removed, green = added
         "\xe2\x94\x80    let claims = decode_jwt(token).unwrap();\n"
         "\xe2\x94\x80\n"
         "+    let claims = match decode_jwt(token) {\n"
         "+        Ok(c) => c,\n"
         "+        Err(e) if e.kind() == &ErrorKind::ExpiredSignature =>\n"
         "+            return Err(unauthorized(\"token expired\")),\n"
         "+        Err(e) => return Err(unauthorized(&e.to_string())),\n"
         "+    };",
         500, 0.5f},

        // Run test
        {Kind::Tool, "Bash", "cargo test test_expired_jwt_returns_401", 300, 0.2f},

        {Kind::Result, "",
         "running 1 test\n"
         "test middleware::auth::test_expired_jwt_returns_401 ... ok\n"
         "\n"
         "test result: ok. 1 passed; 0 failed; 0 ignored",
         600, 0.4f},

        // Final response
        {Kind::Text, "",
         "Fixed the panic. The issue was `.unwrap()` on `decode_jwt()` at line 10 "
         "\xe2\x80\x94 expired tokens triggered a panic instead of a 401 response. "
         "Replaced it with a `match` that handles `ExpiredSignature` explicitly. "
         "The test passes now.",
         90, 0.3f},

        // Cost divider
        {Kind::Divider, "", "", 0, 0},
    };
}

// ── State ────────────────────────────────────────────────────────────────────

struct State {
    std::vector<Block> blocks;
    int     phase = 0;
    float   phase_t = 0;
    float   total_t = 0;
    bool    done = false;
    int     tokens_in = 2847, tokens_out = 0;
};

static void advance(State& st, float dt) {
    if (st.done) return;
    st.total_t += dt;

    if (st.phase >= static_cast<int>(st.blocks.size())) {
        st.done = true;
        return;
    }

    auto& b = st.blocks[static_cast<std::size_t>(st.phase)];
    st.phase_t += dt;

    if (b.kind == Kind::Divider) {
        st.done = true;
        return;
    }

    int target = static_cast<int>(b.content.size());
    int shown  = std::min(static_cast<int>(st.phase_t * b.speed), target);
    // Don't split UTF-8
    while (shown < target && (b.content[static_cast<std::size_t>(shown)] & 0xC0) == 0x80)
        shown++;

    // Track output tokens
    st.tokens_out = 0;
    for (int i = 0; i <= st.phase; ++i) {
        auto& bl = st.blocks[static_cast<std::size_t>(i)];
        int s = (i < st.phase) ? static_cast<int>(bl.content.size()) : shown;
        st.tokens_out += s / 4;
    }

    bool text_done = shown >= target;
    float elapsed_after = st.phase_t - (static_cast<float>(target) / b.speed);

    if (text_done && elapsed_after >= b.pause) {
        st.phase++;
        st.phase_t = 0;
    }
}

// ── Spinner ──────────────────────────────────────────────────────────────────

static const char* spinner(float t) {
    static const char* frames[] = {
        "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
        "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7",
        "\xe2\xa0\x87", "\xe2\xa0\x8f",
    };
    return frames[static_cast<int>(t * 10.f) % 10];
}

// ── Styles ───────────────────────────────────────────────────────────────────

struct Sty {
    Style prompt, query, thinking_hdr, thinking_active, thinking_body;
    Style tool_icon, tool_active, tool_name, tool_arg;
    Style result_border, result_text;
    Style diff_del, diff_add, diff_ctx;
    Style response, done_icon, cost;
    Style check;
};

static Sty make_styles() {
    return {
        .prompt         = Style{}.with_bold().with_fg(Color::rgb(100, 180, 255)),
        .query          = Style{}.with_bold().with_fg(Color::rgb(225, 225, 240)),
        .thinking_hdr   = Style{}.with_fg(Color::rgb(100, 100, 120)),
        .thinking_active= Style{}.with_fg(Color::rgb(180, 130, 255)),
        .thinking_body  = Style{}.with_italic().with_fg(Color::rgb(130, 130, 155)),
        .tool_icon      = Style{}.with_fg(Color::rgb(180, 130, 255)),
        .tool_active    = Style{}.with_fg(Color::rgb(180, 130, 255)),
        .tool_name      = Style{}.with_bold().with_fg(Color::rgb(80, 190, 255)),
        .tool_arg       = Style{}.with_fg(Color::rgb(180, 180, 200)),
        .result_border  = Style{}.with_fg(Color::rgb(50, 55, 70)),
        .result_text    = Style{}.with_fg(Color::rgb(170, 175, 185)),
        .diff_del       = Style{}.with_fg(Color::rgb(255, 100, 100)),
        .diff_add       = Style{}.with_fg(Color::rgb(80, 220, 120)),
        .diff_ctx       = Style{}.with_fg(Color::rgb(150, 150, 170)),
        .response       = Style{}.with_fg(Color::rgb(215, 215, 235)),
        .done_icon      = Style{}.with_fg(Color::rgb(80, 220, 120)),
        .cost           = Style{}.with_fg(Color::rgb(75, 75, 95)),
        .check          = Style{}.with_bold().with_fg(Color::rgb(80, 220, 120)),
    };
}

// ── Build element tree ───────────────────────────────────────────────────────

static Element build_ui(const State& st, int w, const Sty& s) {
    std::vector<Element> rows;

    // Prompt
    rows.push_back(hstack()(
        text("\xe2\x9d\xaf ", s.prompt),
        text(kQuery, s.query)
    ));
    rows.push_back(text(""));

    for (int i = 0; i <= st.phase && i < static_cast<int>(st.blocks.size()); ++i) {
        const auto& b = st.blocks[static_cast<std::size_t>(i)];
        bool current = (i == st.phase);
        int target = static_cast<int>(b.content.size());
        int shown  = current
            ? std::min(static_cast<int>(st.phase_t * b.speed), target)
            : target;
        while (shown < target && (b.content[static_cast<std::size_t>(shown)] & 0xC0) == 0x80)
            shown++;
        bool block_done = shown >= target;
        std::string_view visible{b.content.data(), static_cast<std::size_t>(shown)};

        switch (b.kind) {
        case Kind::Thinking: {
            float dur = block_done
                ? static_cast<float>(target) / b.speed
                : st.phase_t;
            char hdr[48];
            if (block_done) {
                std::snprintf(hdr, sizeof hdr, "\xe2\x9c\x93 thought for %.1fs", static_cast<double>(dur));
            } else {
                std::snprintf(hdr, sizeof hdr, "%s thinking...", spinner(st.total_t));
            }

            rows.push_back(
                box().direction(FlexDirection::Column).padding(0, 0, 0, 2).max_width(w - 2)(
                    text(hdr, block_done ? s.thinking_hdr : s.thinking_active),
                    text(std::string(visible), s.thinking_body)
                )
            );
            rows.push_back(text(""));
            break;
        }

        case Kind::Tool: {
            std::string icon_str = block_done
                ? "\xe2\x9c\x93 " : std::string(spinner(st.total_t)) + " ";
            Style icon_s = block_done ? s.done_icon : s.tool_active;

            rows.push_back(hstack()(
                text(icon_str, icon_s),
                text(b.tool, s.tool_name),
                text(" ", s.tool_arg),
                text(std::string(visible), s.tool_arg)
            ));
            rows.push_back(text(""));
            break;
        }

        case Kind::Result: {
            if (shown == 0) break;

            // Check if this is a diff block (contains + or ─ prefixed lines)
            std::string vis(visible);
            bool is_diff = vis.find('+') != std::string::npos &&
                          (vis.find("\xe2\x94\x80") != std::string::npos);

            if (is_diff) {
                // Diff-colored output: lines starting with ─ are deletions, + are additions
                std::vector<Element> diff_lines;
                std::string line;
                for (char ch : vis) {
                    if (ch == '\n') {
                        Style ls = s.diff_ctx;
                        if (!line.empty()) {
                            if (line[0] == '+') ls = s.diff_add;
                            else if (line.size() >= 3 && line[0] == '\xe2') ls = s.diff_del;
                        }
                        diff_lines.push_back(text(line, ls));
                        line.clear();
                    } else {
                        line += ch;
                    }
                }
                if (!line.empty()) {
                    Style ls = s.diff_ctx;
                    if (line[0] == '+') ls = s.diff_add;
                    else if (line.size() >= 3 && line[0] == '\xe2') ls = s.diff_del;
                    diff_lines.push_back(text(line, ls));
                }

                rows.push_back(
                    box().direction(FlexDirection::Column)
                        .border(BorderStyle::Round)
                        .border_color(Color::rgb(50, 55, 70))
                        .padding(0, 1, 0, 1)
                        .max_width(w - 4)(
                        std::move(diff_lines)
                    )
                );
            } else {
                // Regular result in bordered box
                // Color "ok" green in test output
                rows.push_back(
                    box().direction(FlexDirection::Column)
                        .border(BorderStyle::Round)
                        .border_color(Color::rgb(50, 55, 70))
                        .padding(0, 1, 0, 1)
                        .max_width(w - 4)(
                        text(std::string(visible), s.result_text)
                    )
                );
            }
            rows.push_back(text(""));
            break;
        }

        case Kind::Text: {
            rows.push_back(
                box().direction(FlexDirection::Column).max_width(w - 2)(
                    text(std::string(visible), s.response)
                )
            );
            rows.push_back(text(""));
            break;
        }

        case Kind::Divider: {
            float cost = (st.tokens_in * 3.f + st.tokens_out * 15.f) / 1000000.f;
            char buf[64];
            std::snprintf(buf, sizeof buf,
                "%d input + %d output tokens \xe2\x80\xa2 $%.4f",
                st.tokens_in, st.tokens_out, static_cast<double>(cost));
            rows.push_back(text(buf, s.cost));
            break;
        }
        }
    }

    return vstack().width(w)(std::move(rows));
}

// ── Rendering helpers ────────────────────────────────────────────────────────

static int find_content_height(const Canvas& c, int w, int h) {
    for (int y = h - 1; y >= 0; --y) {
        for (int x = 0; x < w; ++x) {
            auto cell = Cell::unpack(c.cells()[y * w + x]);
            if (cell.character != U' ' && cell.character != 0) return y + 1;
        }
    }
    return 1;
}

static void render_frame(const Element& root, int w, StylePool& pool,
                         std::string& out, int& prev_h) {
    const int max_h = 50;
    Canvas canvas{w, max_h, &pool};
    render_tree(root, canvas, pool, theme::dark);

    int h = find_content_height(canvas, w, max_h);

    // Copy to trimmed canvas
    Canvas trimmed{w, h, &pool};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            auto cell = Cell::unpack(canvas.cells()[y * w + x]);
            trimmed.set(x, y, cell.character, cell.style_id);
        }

    out.clear();
    if (prev_h > 0) ansi::erase_lines(prev_h, out);
    serialize(trimmed, pool, out);
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
    prev_h = h;
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main() {
    // Detect terminal width
    int term_w = 80;
    #ifdef __unix__
    {
        struct winsize ws{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
            term_w = ws.ws_col;
    }
    #endif
    // Leave 1-col margin to prevent wrapping artifacts
    int w = std::min(term_w - 1, 96);
    if (w < 40) w = 40;

    State st;
    st.blocks = make_scenario();

    Sty sty = make_styles();
    StylePool pool;
    std::string out;
    int prev_h = 0;

    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();

    std::fputs("\x1b[?25l", stdout); // hide cursor

    while (!st.done) {
        auto now = Clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;

        advance(st, dt);
        Element root = build_ui(st, w, sty);
        render_frame(root, w, pool, out, prev_h);

        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    // Final render with all content visible
    advance(st, 1.0f);
    {
        Element root = build_ui(st, w, sty);
        render_frame(root, w, pool, out, prev_h);
    }

    std::fputs("\x1b[?25h\n", stdout); // show cursor
    std::fflush(stdout);
}
