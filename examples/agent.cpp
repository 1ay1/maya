// maya — AI coding agent (inline, no alt screen)
//
// Automatic playthrough of an AI agent fixing a bug: thinking, reading files,
// editing code, running tests — all rendered inline with streaming text,
// spinners, diff-colored edits, and bordered tool output.
//
// Uses maya::inline_run() — the inline equivalent of maya::run().

#include <maya/maya.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace maya;
using namespace maya::dsl;

// ── Types ────────────────────────────────────────────────────────────────────

enum class Kind { Thinking, Tool, Result, Text, Divider };

struct Block {
    Kind        kind;
    std::string tool;
    std::string content;
    float       speed;      // chars per second
    float       pause;      // seconds to wait after done
};

// ── Scenario ─────────────────────────────────────────────────────────────────

static const char* kQuery =
    "Fix the failing test in auth_middleware.rs \xe2\x80\x94 "
    "it panics on expired JWT tokens";

static std::vector<Block> make_scenario() {
    return {
        {Kind::Thinking, "",
         "The test `test_expired_jwt_returns_401` panics instead of returning 401. "
         "The JWT validation likely calls `.unwrap()` on the decode result rather "
         "than handling `ExpiredSignature`. Let me check the middleware code.",
         160, 0.4f},

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

        {Kind::Tool, "Edit", "src/middleware/auth.rs:10", 400, 0.2f},

        {Kind::Result, "",
         "\xe2\x94\x80    let claims = decode_jwt(token).unwrap();\n"
         "\xe2\x94\x80\n"
         "+    let claims = match decode_jwt(token) {\n"
         "+        Ok(c) => c,\n"
         "+        Err(e) if e.kind() == &ErrorKind::ExpiredSignature =>\n"
         "+            return Err(unauthorized(\"token expired\")),\n"
         "+        Err(e) => return Err(unauthorized(&e.to_string())),\n"
         "+    };",
         500, 0.5f},

        {Kind::Tool, "Bash", "cargo test test_expired_jwt_returns_401", 300, 0.2f},

        {Kind::Result, "",
         "running 1 test\n"
         "test middleware::auth::test_expired_jwt_returns_401 ... ok\n"
         "\n"
         "test result: ok. 1 passed; 0 failed; 0 ignored",
         600, 0.4f},

        {Kind::Text, "",
         "Fixed the panic. The issue was `.unwrap()` on `decode_jwt()` at line 10 "
         "\xe2\x80\x94 expired tokens triggered a panic instead of a 401 response. "
         "Replaced it with a `match` that handles `ExpiredSignature` explicitly. "
         "The test passes now.",
         90, 0.3f},

        {Kind::Divider, "", "", 0, 0},
    };
}

// ── State ────────────────────────────────────────────────────────────────────

struct State {
    std::vector<Block> blocks = make_scenario();
    int   phase = 0;
    float phase_t = 0;
    float total_t = 0;
    int   tokens_in = 2847, tokens_out = 0;
};

static const char* spin(float t) {
    static const char* f[] = {
        "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
        "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7",
        "\xe2\xa0\x87", "\xe2\xa0\x8f",
    };
    return f[static_cast<int>(t * 10.f) % 10];
}

static void advance(State& st, float dt) {
    st.total_t += dt;
    if (st.phase >= static_cast<int>(st.blocks.size())) { quit(); return; }

    auto& b = st.blocks[static_cast<std::size_t>(st.phase)];
    st.phase_t += dt;

    if (b.kind == Kind::Divider) { quit(); return; }

    int target = static_cast<int>(b.content.size());
    int shown  = std::min(static_cast<int>(st.phase_t * b.speed), target);
    while (shown < target && (b.content[static_cast<std::size_t>(shown)] & 0xC0) == 0x80)
        shown++;

    st.tokens_out = 0;
    for (int i = 0; i <= st.phase; ++i) {
        auto& bl = st.blocks[static_cast<std::size_t>(i)];
        int s = (i < st.phase) ? static_cast<int>(bl.content.size()) : shown;
        st.tokens_out += s / 4;
    }

    if (shown >= target && st.phase_t > (static_cast<float>(target) / b.speed) + b.pause) {
        st.phase++;
        st.phase_t = 0;
    }
}

// ── Styles ───────────────────────────────────────────────────────────────────

static const Style sPrompt   = Style{}.with_bold().with_fg(Color::rgb(100, 180, 255));
static const Style sQuery    = Style{}.with_bold().with_fg(Color::rgb(225, 225, 240));
static const Style sThinkHdr = Style{}.with_fg(Color::rgb(100, 100, 120));
static const Style sThinkAct = Style{}.with_fg(Color::rgb(180, 130, 255));
static const Style sThinkTxt = Style{}.with_italic().with_fg(Color::rgb(130, 130, 155));
static const Style sToolName = Style{}.with_bold().with_fg(Color::rgb(80, 190, 255));
static const Style sToolArg  = Style{}.with_fg(Color::rgb(180, 180, 200));
static const Style sActive   = Style{}.with_fg(Color::rgb(180, 130, 255));
static const Style sDone     = Style{}.with_bold().with_fg(Color::rgb(80, 220, 120));
static const Style sResult   = Style{}.with_fg(Color::rgb(170, 175, 185));
static const Style sDiffDel  = Style{}.with_fg(Color::rgb(255, 100, 100));
static const Style sDiffAdd  = Style{}.with_fg(Color::rgb(80, 220, 120));
static const Style sResponse = Style{}.with_fg(Color::rgb(215, 215, 235));
static const Style sCost     = Style{}.with_fg(Color::rgb(75, 75, 95));

// ── Build UI ─────────────────────────────────────────────────────────────────

static int shown_chars(const State& st, int i) {
    const auto& b = st.blocks[static_cast<std::size_t>(i)];
    int target = static_cast<int>(b.content.size());
    if (i < st.phase) return target;
    int s = std::min(static_cast<int>(st.phase_t * b.speed), target);
    while (s < target && (b.content[static_cast<std::size_t>(s)] & 0xC0) == 0x80) s++;
    return s;
}

static Element build_ui(const State& st) {
    // Dynamic content built inside dyn() — the DSL orchestrates the tree
    std::vector<Element> rows;

    rows.push_back(h(
        dyn([&] { return text("\xe2\x9d\xaf ", sPrompt); }),
        dyn([&] { return text(kQuery, sQuery); })
    ).build());
    rows.push_back(text(""));

    for (int i = 0; i <= st.phase && i < static_cast<int>(st.blocks.size()); ++i) {
        const auto& b = st.blocks[static_cast<std::size_t>(i)];
        bool current = (i == st.phase);
        int shown = shown_chars(st, i);
        bool done = shown >= static_cast<int>(b.content.size());
        std::string vis(b.content.data(), static_cast<std::size_t>(shown));

        switch (b.kind) {
        case Kind::Thinking: {
            float dur = done ? static_cast<float>(b.content.size()) / b.speed : st.phase_t;
            char hdr[48];
            if (done)
                std::snprintf(hdr, sizeof hdr, "\xe2\x9c\x93 thought for %.1fs", static_cast<double>(dur));
            else
                std::snprintf(hdr, sizeof hdr, "%s thinking...", spin(st.total_t));

            rows.push_back(
                vstack().padding(0, 0, 0, 2)(
                    text(hdr, done ? sThinkHdr : sThinkAct),
                    text(vis, sThinkTxt)
                )
            );
            rows.push_back(text(""));
            break;
        }

        case Kind::Tool: {
            std::string icon = done ? "\xe2\x9c\x93 " : std::string(spin(st.total_t)) + " ";
            rows.push_back(h(
                dyn([icon, done] { return text(icon, done ? sDone : sActive); }),
                dyn([&b] { return text(b.tool, sToolName); }),
                dyn([] { return text(" ", sToolArg); }),
                dyn([vis] { return text(vis, sToolArg); })
            ).build());
            rows.push_back(text(""));
            break;
        }

        case Kind::Result: {
            if (shown == 0) break;
            bool is_diff = vis.find('+') != std::string::npos &&
                           vis.find("\xe2\x94\x80") != std::string::npos;

            if (is_diff) {
                std::vector<Element> lines;
                std::string line;
                auto flush = [&] {
                    Style s = sResult;
                    if (!line.empty()) {
                        if (line[0] == '+') s = sDiffAdd;
                        else if (line.size() >= 3 && line[0] == '\xe2') s = sDiffDel;
                    }
                    lines.push_back(text(line, s));
                    line.clear();
                };
                for (char ch : vis) { if (ch == '\n') flush(); else line += ch; }
                if (!line.empty()) flush();

                rows.push_back(
                    vstack().border(BorderStyle::Round)
                        .border_color(Color::rgb(50, 55, 70))
                        .padding(0, 1, 0, 1)(std::move(lines))
                );
            } else {
                rows.push_back(
                    vstack().border(BorderStyle::Round)
                        .border_color(Color::rgb(50, 55, 70))
                        .padding(0, 1, 0, 1)(
                        text(vis, sResult)
                    )
                );
            }
            rows.push_back(text(""));
            break;
        }

        case Kind::Text:
            rows.push_back(text(vis, sResponse));
            rows.push_back(text(""));
            break;

        case Kind::Divider: {
            float cost = (st.tokens_in * 3.f + st.tokens_out * 15.f) / 1000000.f;
            char buf[64];
            std::snprintf(buf, sizeof buf, "%d input + %d output tokens \xe2\x80\xa2 $%.4f",
                          st.tokens_in, st.tokens_out, static_cast<double>(cost));
            rows.push_back(text(buf, sCost));
            break;
        }
        }
    }

    return vstack()(std::move(rows));
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main() {
    State st;

    inline_run({.fps = 30, .max_width = 96}, [&](float dt) {
        advance(st, dt);
        return build_ui(st);
    });
}
