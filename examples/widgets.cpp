// widgets.cpp — Showcase of visualization widgets
//
// Demonstrates 7 data visualization widgets in a single dashboard:
//   ContextWindow, FlameChart, GitGraph, InlineDiff,
//   Timeline, TokenStream, Waterfall
//
// Controls:
//   tab        cycle focus between panels
//   space      animate (advance frame / toggle streaming)
//   r          reset to initial state
//   q/Esc      quit

#include <maya/maya.hpp>
#include <maya/widget/context_window.hpp>
#include <maya/widget/flame_chart.hpp>
#include <maya/widget/git_graph.hpp>
#include <maya/widget/inline_diff.hpp>
#include <maya/widget/timeline.hpp>
#include <maya/widget/token_stream.hpp>
#include <maya/widget/waterfall.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <vector>

using namespace maya;
using namespace maya::dsl;

// ── Helpers ──────────────────────────────────────────────────────────────────

static std::mt19937 rng{42};
static float randf(float lo, float hi) {
    return std::uniform_real_distribution<float>(lo, hi)(rng);
}

// ── Program ──────────────────────────────────────────────────────────────────

struct Widgets {
    struct Model {
        int   frame          = 0;
        float elapsed        = 0.f;
        bool  streaming      = true;
        int   total_tokens   = 0;
        float tokens_per_sec = 0.f;
        float peak_rate      = 0.f;
        std::vector<float> rate_history;
        int   panel          = 0;  // focused panel index (0-6)
    };

    struct Tick {};
    struct ToggleStream {};
    struct NextPanel {};
    struct ResetState {};
    struct Quit {};
    using Msg = std::variant<Tick, ToggleStream, NextPanel, ResetState, Quit>;

    static Model init() {
        Model m;
        m.rate_history.resize(32, 0.f);
        return m;
    }

    static auto update(Model m, Msg msg) -> std::pair<Model, Cmd<Msg>> {
        return std::visit(overload{
            [&](Tick) {
                m.frame++;
                m.elapsed += 0.05f;

                if (m.streaming) {
                    float base = 60.f + 30.f * std::sin(m.elapsed * 0.8f);
                    float noise = randf(-10.f, 10.f);
                    m.tokens_per_sec = std::max(5.f, base + noise);
                    m.total_tokens += static_cast<int>(m.tokens_per_sec * 0.05f);
                    if (m.tokens_per_sec > m.peak_rate)
                        m.peak_rate = m.tokens_per_sec;

                    m.rate_history.push_back(m.tokens_per_sec);
                    if (m.rate_history.size() > 32)
                        m.rate_history.erase(m.rate_history.begin());
                }

                return std::pair{m, Cmd<Msg>{}};
            },
            [&](ToggleStream) {
                m.streaming = !m.streaming;
                return std::pair{m, Cmd<Msg>{}};
            },
            [&](NextPanel) {
                m.panel = (m.panel + 1) % 7;
                return std::pair{m, Cmd<Msg>{}};
            },
            [&](ResetState) {
                auto fresh = init();
                fresh.panel = m.panel;
                return std::pair{fresh, Cmd<Msg>{}};
            },
            [](Quit) {
                return std::pair{Model{}, Cmd<Msg>::quit()};
            },
        }, msg);
    }

    // ── Colors ────────────────────────────────────────────────────────────

    static constexpr Color focus_color   = Color::rgb(97, 175, 239);
    static constexpr Color unfocus_color = Color::rgb(50, 54, 62);

    // ── Panel builders ───────────────────────────────────────────────────

    static Element build_context_window(bool focused) {
        ContextWindow ctx(200000);
        ctx.set_width(44);
        ctx.add_segment("System",   12400, Color::rgb(97, 175, 239));
        ctx.add_segment("History",  89200, Color::rgb(198, 120, 221));
        ctx.add_segment("Tools",    32100, Color::rgb(229, 192, 123));
        ctx.add_segment("Response", 11534, Color::rgb(152, 195, 121));

        return v(ctx)
            | border(BorderStyle::Round)
            | bcolor(focused ? focus_color : unfocus_color)
            | btext(" Context Window ")
            | padding(0, 1);
    }

    static Element build_flame_chart(bool focused) {
        FlameChart flame(12.0f);
        flame.set_width(52);
        flame.add_span("request",    0.0f, 12.0f, 0);
        flame.add_span("thinking",   0.0f,  4.2f, 1);
        flame.add_span("tool calls", 4.5f,  5.0f, 1);
        flame.add_span("responding", 9.8f,  2.2f, 1);
        flame.add_span("read file",  4.5f,  1.5f, 2);
        flame.add_span("edit file",  6.2f,  2.0f, 2);
        flame.add_span("run tests",  8.5f,  1.2f, 2);

        return v(flame)
            | border(BorderStyle::Round)
            | bcolor(focused ? focus_color : unfocus_color)
            | btext(" Flame Chart ")
            | padding(0, 1);
    }

    static Element build_git_graph(bool focused) {
        GitGraph graph;
        graph.add_commit({"a9f3cf1", "fix fps and add space3d",        "", "2m ago",  0, false, true});
        graph.add_commit({"9f2b7d4", "shorten key hold window",        "", "5m ago",  0});
        graph.add_commit({"2047a1e", "held-key tracking",              "", "12m ago", 0});
        graph.add_commit({"5a29502", "fps: dark red walls, twilight",  "", "1h ago",  1});
        graph.add_commit({"09591cd", "sorts: 8 algorithms",            "", "2h ago",  0, true});
        graph.add_commit({"3a5d90e", "make macos hotpath faster",      "", "3h ago",  0});
        graph.add_commit({"600c537", "syntax highlighting in markdown", "", "4h ago",  1});
        graph.add_commit({"2f8ffec", "merge Elm architecture",         "", "5h ago",  0, true});

        return v(graph)
            | border(BorderStyle::Round)
            | bcolor(focused ? focus_color : unfocus_color)
            | btext(" Git Graph ")
            | padding(0, 1);
    }

    static Element build_inline_diff(bool focused) {
        auto diff1 = InlineDiff(
            "const SESSION_TIMEOUT = 3600;",
            "const TOKEN_EXPIRY = '1h';");
        diff1.set_label("src/config.ts");

        auto diff2 = InlineDiff(
            "app.use(session({ secret: process.env.SECRET }))",
            "app.use(jwt({ algorithm: 'RS256', publicKey: KEY }))");
        diff2.set_label("src/middleware/auth.ts");

        return v(diff1, text(""), diff2)
            | border(BorderStyle::Round)
            | bcolor(focused ? focus_color : unfocus_color)
            | btext(" Inline Diff ")
            | padding(0, 1);
    }

    static Element build_timeline(const Model& m, bool focused) {
        Timeline tl;
        tl.set_frame(m.frame);
        tl.set_track_width(36);

        tl.add({"Read auth middleware",    "Read 42 lines from src/auth.ts",
                "1.2s", TaskStatus::Completed});
        tl.add({"Analyze dependencies",    "Found 3 affected modules",
                "0.8s", TaskStatus::Completed});
        tl.add({"Edit auth.ts",            "Replaced session with JWT tokens",
                "2.0s", TaskStatus::Completed, 12});
        tl.add({"Edit middleware/index.ts", "Updated exports",
                "0.4s", TaskStatus::InProgress, 6});
        tl.add({"Run test suite",          "",
                "",     TaskStatus::Pending});
        tl.add({"Verify deployment",       "",
                "",     TaskStatus::Pending});

        return v(tl)
            | border(BorderStyle::Round)
            | bcolor(focused ? focus_color : unfocus_color)
            | btext(" Timeline ")
            | padding(0, 1);
    }

    static Element build_token_stream(const Model& m, bool focused) {
        TokenStream ts;
        ts.set_total_tokens(m.total_tokens);
        ts.set_tokens_per_sec(m.tokens_per_sec);
        ts.set_peak_rate(m.peak_rate);
        ts.set_elapsed(m.elapsed);
        ts.set_rate_history(m.rate_history);

        return v(ts)
            | border(BorderStyle::Round)
            | bcolor(focused ? focus_color : unfocus_color)
            | btext(" Token Stream ")
            | padding(0, 1);
    }

    static Element build_waterfall(const Model& m, bool focused) {
        Waterfall wf;
        wf.set_bar_width(36);
        wf.set_frame(m.frame);

        wf.add({"Read auth.ts",      0.0f, 1.2f, Color::rgb(97, 175, 239)});
        wf.add({"Read config.ts",    0.3f, 0.8f, Color::rgb(97, 175, 239)});
        wf.add({"Analyze imports",   1.0f, 1.5f, Color::rgb(198, 120, 221)});
        wf.add({"Edit auth.ts",      2.0f, 2.3f, Color::rgb(229, 192, 123)});
        wf.add({"Edit middleware.ts", 2.5f, 1.8f, Color::rgb(229, 192, 123)});
        wf.add({"Write token.ts",    3.5f, 1.2f, Color::rgb(152, 195, 121)});
        wf.add({"Run tests",         4.8f, 3.5f, Color::rgb(152, 195, 121),
                TaskStatus::InProgress});

        return v(wf)
            | border(BorderStyle::Round)
            | bcolor(focused ? focus_color : unfocus_color)
            | btext(" Waterfall ")
            | padding(0, 1);
    }

    // ── view ─────────────────────────────────────────────────────────────

    static Element view(const Model& m) {
        auto title_bar = h(
            text("Widget Showcase") | Bold | Fg<100, 180, 255>,
            text("   "),
            when(m.streaming,
                text("STREAMING") | Bold | Fg<80, 220, 120>,
                text("PAUSED") | Dim),
            text("   "),
            text(std::format("frame:{} elapsed:{:.1f}s tokens:{}",
                m.frame, static_cast<double>(m.elapsed), m.total_tokens)) | Dim
        );

        // Left column: context, flame chart, inline diff
        auto left_col = v(
            build_context_window(m.panel == 0),
            build_flame_chart(m.panel == 1),
            build_inline_diff(m.panel == 3)
        );

        // Right column: git graph, token stream, waterfall
        auto right_col = v(
            build_git_graph(m.panel == 2),
            build_token_stream(m, m.panel == 5),
            build_waterfall(m, m.panel == 6)
        );

        auto main_area = h(
            std::move(left_col)  | grow(),
            std::move(right_col) | grow()
        );

        auto bottom_row = build_timeline(m, m.panel == 4);

        auto help = text("tab cycle panels  space stream  r reset  q quit") | Dim;

        return v(
            std::move(title_bar),
            blank_,
            std::move(main_area),
            std::move(bottom_row),
            blank_,
            std::move(help)
        ) | pad<1>;
    }

    // ── subscribe ────────────────────────────────────────────────────────

    static auto subscribe(const Model& m) -> Sub<Msg> {
        auto keys = key_map<Msg>({
            {'q',                Quit{}},
            {SpecialKey::Escape, Quit{}},
            {' ',                ToggleStream{}},
            {'\t',               NextPanel{}},
            {'r',                ResetState{}},
        });

        return Sub<Msg>::batch(
            std::move(keys),
            Sub<Msg>::every(std::chrono::milliseconds(50), Tick{})
        );
    }
};

static_assert(Program<Widgets>);

int main() {
    run<Widgets>({.title = "widgets", .fps = 20});
}
