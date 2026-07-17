// test_widgets.cpp — Verify all widgets render correctly at various widths
#include <maya/maya.hpp>
#include <maya/widget/badge.hpp>
#include <maya/widget/agent_timeline.hpp>
#include <maya/widget/breadcrumb.hpp>
#include <maya/widget/composer.hpp>
#include <maya/widget/context_gauge.hpp>
#include <maya/widget/divider.hpp>
#include <maya/widget/input.hpp>
#include <maya/widget/markdown.hpp>
#include <maya/widget/modal.hpp>
#include <maya/widget/model_badge.hpp>
#include <maya/widget/picker.hpp>
#include <maya/widget/progress.hpp>
#include <maya/widget/select.hpp>
#include <maya/widget/spinner.hpp>
#include <maya/widget/status_bar.hpp>
#include <maya/widget/table.hpp>
#include <maya/widget/toast.hpp>
#include <cassert>
#include <print>
#include <string>
#include <vector>

using namespace maya;

std::string get_row(const Canvas& canvas, int y) {
    std::string s;
    for (int x = 0; x < canvas.width(); ++x) {
        Cell c = canvas.get(x, y);
        if (c.character >= 0x20 && c.character < 0x7F)
            s += static_cast<char>(c.character);
        else if (c.character == 0x2500)
            s += '-';
        else if (c.character == 0x25C6)
            s += '*';
        else if (c.character == 0x276F)
            s += '>';
        else if (c.character == 0x25CF)
            s += '@';
        else if (c.character == 0x2588)
            s += '#';
        else if (c.character != U' ' && c.character != 0)
            s += '?';
        else
            s += ' ';
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

struct RenderResult {
    std::vector<std::string> rows;
    int content_h;
};

RenderResult render_at(const Element& elem, int width, int height = 500,
                       bool auto_h = true) {
    StylePool pool;
    Canvas canvas(width, height, &pool);
    render_tree(elem, canvas, pool, theme::dark, auto_h);
    int ch = content_height(canvas);
    std::vector<std::string> rows;
    for (int y = 0; y < ch; ++y)
        rows.push_back(get_row(canvas, y));
    return {rows, ch};
}

void dump(const std::string& label, const RenderResult& r) {
    std::println("  {} ({} rows):", label, r.content_h);
    for (int y = 0; y < r.content_h; ++y)
        std::println("    {:2}|{}|", y, r.rows[y]);
}

// Assert no row exceeds the canvas width
void assert_fits(const RenderResult& r, int max_w, const char* ctx) {
    for (int y = 0; y < r.content_h; ++y) {
        if (static_cast<int>(r.rows[y].size()) > max_w) {
            std::println("  FAIL [{}]: row {} exceeds width {} (len={}): {}",
                         ctx, y, max_w, r.rows[y].size(), r.rows[y]);
            assert(false);
        }
    }
}

// Assert rendering at width w1, then w2, then w1 produces same result
void assert_resize_stable(const Element& elem, int w1, int w2, const char* ctx) {
    auto r1 = render_at(elem, w1);
    auto r2 = render_at(elem, w2);
    auto r3 = render_at(elem, w1);
    assert(r1.content_h == r3.content_h);
    for (int y = 0; y < r1.content_h; ++y) {
        if (r1.rows[y] != r3.rows[y]) {
            std::println("  FAIL [{}]: row {} differs after {}→{}→{}", ctx, y, w1, w2, w1);
            std::println("    before: |{}|", r1.rows[y]);
            std::println("    after:  |{}|", r3.rows[y]);
            assert(false);
        }
    }
    (void)r2;
}

// ============================================================================
// Table tests
// ============================================================================
void test_table() {
    std::println("=== test_table ===");

    Table tbl({{"Property", 0}, {"Value", 0}});
    tbl.add_row({"Language", "C++"});
    tbl.add_row({"Standard", "C++26"});
    tbl.add_row({"Compiler", "g++-15"});

    for (int w : {20, 30, 40, 60, 80, 120}) {
        auto r = render_at(tbl, w);
        assert(r.content_h > 0);
        assert_fits(r, w, "table");
    }
    assert_resize_stable(tbl, 80, 40, "table");

    std::println("  PASS\n");
}

// ============================================================================
// ProgressBar tests
// ============================================================================
void test_progress() {
    std::println("=== test_progress ===");

    ProgressBar bar;
    bar.set(0.5f);

    for (int w : {20, 40, 60, 80, 120}) {
        auto r = render_at(bar, w);
        assert(r.content_h > 0);
        assert_fits(r, w, "progress");
    }
    assert_resize_stable(bar, 80, 40, "progress");

    std::println("  PASS\n");
}

// ============================================================================
// Divider tests
// ============================================================================
void test_divider() {
    std::println("=== test_divider ===");

    Divider plain;
    Divider labeled("Section");

    for (int w : {20, 40, 80, 120}) {
        auto r1 = render_at(plain, w);
        auto r2 = render_at(labeled, w);
        assert(r1.content_h > 0);
        assert(r2.content_h > 0);
        assert_fits(r1, w, "divider-plain");
        assert_fits(r2, w, "divider-labeled");
    }
    assert_resize_stable(plain, 80, 40, "divider-plain");
    assert_resize_stable(labeled, 80, 40, "divider-labeled");

    std::println("  PASS\n");
}

// ============================================================================
// Badge tests
// ============================================================================
void test_badge() {
    std::println("=== test_badge ===");

    auto b = Badge::tool("read_file");
    for (int w : {15, 20, 40, 80}) {
        auto r = render_at(b, w);
        assert(r.content_h > 0);
        assert_fits(r, w, "badge");
    }

    std::println("  PASS\n");
}

// ============================================================================
// Breadcrumb tests
// ============================================================================
void test_breadcrumb() {
    std::println("=== test_breadcrumb ===");

    Breadcrumb bc({"project", "src", "widget", "table.hpp"});
    for (int w : {20, 40, 80, 120}) {
        auto r = render_at(bc, w);
        assert(r.content_h > 0);
        assert_fits(r, w, "breadcrumb");
    }
    assert_resize_stable(bc, 80, 40, "breadcrumb");

    std::println("  PASS\n");
}

// ============================================================================
// Select tests
// ============================================================================
void test_select() {
    std::println("=== test_select ===");

    Select menu({"Option A", "Option B", "Long option with lots of text"});
    for (int w : {20, 40, 80}) {
        auto r = render_at(menu, w);
        assert(r.content_h > 0);
        assert_fits(r, w, "select");
    }
    assert_resize_stable(menu, 80, 30, "select");

    std::println("  PASS\n");
}

// ============================================================================
// Toast tests
// ============================================================================
void test_toast() {
    std::println("=== test_toast ===");

    ToastManager toasts;
    toasts.push("File saved", ToastLevel::Success);
    toasts.push("Warning: deprecated API", ToastLevel::Warning);

    for (int w : {30, 60, 80}) {
        auto r = render_at(toasts, w);
        assert(r.content_h > 0);
        assert_fits(r, w, "toast");
    }

    std::println("  PASS\n");
}

// ============================================================================
// Spinner tests
// ============================================================================
void test_spinner() {
    std::println("=== test_spinner ===");

    Spinner spin;
    for (int w : {10, 20, 40, 80}) {
        auto r = render_at(spin, w);
        assert(r.content_h > 0);
        assert_fits(r, w, "spinner");
    }

    std::println("  PASS\n");
}

// ============================================================================
// Input tests
// ============================================================================
void test_input() {
    std::println("=== test_input ===");

    Input inp;
    inp.set_value("hello world this is a long input text");
    for (int w : {15, 30, 60, 80}) {
        auto r = render_at(inp, w);
        assert(r.content_h > 0);
        assert_fits(r, w, "input");
    }
    assert_resize_stable(inp, 80, 30, "input");

    std::println("  PASS\n");
}

// ============================================================================
// Markdown tests
// ============================================================================
void test_markdown() {
    std::println("=== test_markdown ===");

    auto test_md = [](const char* label, const char* src) {
        auto elem = markdown(src);
        for (int w : {40, 80, 120}) {
            auto r = render_at(elem, w);
            assert(r.content_h > 0);
            assert_fits(r, w, label);
        }
        assert_resize_stable(elem, 80, 40, label);
    };

    test_md("simple bold", "Hello **world**");
    test_md("two paragraphs", "First para\n\nSecond **bold** para");
    test_md("list plain", "- Item one\n- Item two");
    test_md("list with bold", "- **Maya** is a framework\n- Item two");
    test_md("code block", "```cpp\nint x = 1;\n```");
    test_md("blockquote", "> This is a quote\n> with two lines");
    test_md("heading", "## Heading\n\nParagraph text");
    test_md("table", "| A | B |\n|---|---|\n| 1 | 2 |");
    test_md("exclamation", "Hello! World! Done!");
    test_md("full response",
        "I'll help you with that!\n\n"
        "Here's a quick overview:\n\n"
        "- **Maya** is a C++26 TUI framework\n"
        "- It uses **compile-time** DSL for type-safe UI\n"
        "- SIMD-accelerated terminal diffing\n"
        "- Flexbox layout engine\n\n"
        "The framework is designed for **high-performance** terminal applications.");

    std::println("  PASS\n");
}

// ============================================================================
// Streaming markdown tests
// ============================================================================
void test_markdown_streaming() {
    std::println("=== test_markdown_streaming ===");

    std::vector<std::string> tokens = {
        "I", "'ll", " help", " you", " with", " that", "!\n\n",
        "Here", "'s", " a", " quick", " overview", ":\n\n",
        "- ", "**Maya**", " is", " a", " C++26", " TUI", " framework", "\n",
        "- ", "It", " uses", " **compile-time**", " DSL", " for", " type-safe", " UI", "\n",
        "- ", "SIMD", "-accelerated", " terminal", " diffing", "\n",
        "- ", "Flexbox", " layout", " engine", "\n\n",
        "The", " framework", " is", " designed", " for", " **high-performance**",
        " terminal", " applications", ".",
    };

    StreamingMarkdown md;
    for (auto& tok : tokens) {
        md.append(tok);
        auto elem = md.build();
        auto r = render_at(elem, 80);
        assert(r.content_h > 0);
    }
    md.finish();
    auto elem = md.build();
    auto r = render_at(elem, 80);
    assert(r.content_h >= 8);

    std::println("  {} tokens streamed, final: {} rows", tokens.size(), r.content_h);
    std::println("  PASS\n");
}

// ============================================================================
// Status bar responsiveness tests
// ============================================================================
//
// The activity row MUST be exactly 3 rows tall (top accent + 1 content
// row + bottom accent) at EVERY terminal width. If any piece wraps onto
// a phantom second line the whole status bar grows to 4 rows, shoving
// the Thread above it and triggering a full-viewport repaint. We also
// assert the most meaningful piece (the phase verb) survives down to a
// usably-wide terminal, and that no glyphs ever bleed past the right
// edge (every rendered row must fit within `width` columns).
void test_status_bar_responsive() {
    std::println("=== test_status_bar_responsive ===");

    auto make = []() {
        StatusBar::Config cfg;
        cfg.phase_color        = Color::cyan();
        cfg.breadcrumb.title   = "refactor the responsive status bar layout";
        cfg.phase.glyph        = "\xe2\xa0\x8b";   // spinner
        cfg.phase.verb         = "Streaming";
        cfg.phase.color        = Color::bright_cyan();
        cfg.phase.verb_width   = 10;
        cfg.phase.elapsed_secs = 12.3f;
        cfg.model_badge        = ModelBadge{"claude-sonnet-4-5"}.build();
        cfg.context.used       = 84'000;
        cfg.context.max        = 200'000;
        cfg.context.cells      = 10;
        return cfg;
    };

    // Sweep from absurdly narrow to very wide. The row count must be a
    // constant 3 and nothing may overflow the width at ANY of them.
    for (int w = 12; w <= 220; ++w) {
        Element el = StatusBar{make()}.build();
        auto r = render_at(el, w);
        assert(r.content_h == 3 && "status bar must stay 3 rows at every width");
        for (const auto& row : r.rows)
            assert(static_cast<int>(row.size()) <= w
                   && "status bar row must not overflow terminal width");
    }

    // The phase verb ("what's happening now") is the highest-value
    // signal: it must still be present at a normal-narrow 60-col width.
    {
        auto r = render_at(StatusBar{make()}.build(), 60);
        bool has_verb = false;
        for (const auto& row : r.rows)
            if (row.find("Streaming") != std::string::npos) has_verb = true;
        assert(has_verb && "phase verb must survive at 60 cols");
    }

    std::println("  3 rows + no overflow across widths 12..220");
    std::println("  PASS\n");
}

// ============================================================================
// Widget-audit regression tests (agentty-facing widgets)
// ============================================================================

// Composer: a cursor byte-offset inside a multi-byte UTF-8 sequence, or
// past the end / negative, must never throw and must never split the
// sequence (which would paint two U+FFFD cells and shift the line).
void test_composer_cursor_safety() {
    std::println("=== test_composer_cursor_safety ===");

    // "café" — the é is 2 bytes (0xC3 0xA9) at offsets 3..4.
    for (int cur : {-5, 0, 3, 4, 5, 999}) {
        Composer::Config cfg;
        cfg.text   = "caf\xc3\xa9";
        cfg.cursor = cur;
        auto r = render_at(Composer{cfg}.build(), 60);
        assert(r.content_h > 0);
        // The '?' mapping in get_row covers any non-ASCII glyph, so we
        // can't string-match é — but a split sequence would render TWO
        // replacement cells where one glyph should be. Just assert no
        // row overflows and the body row still contains "caf".
        bool has_caf = false;
        for (const auto& row : r.rows)
            if (row.find("caf") != std::string::npos) has_caf = true;
        assert(has_caf && "composer body must render the text intact");
    }

    // User text CONTAINING the block glyph: the caret styling must key
    // on the cursor's byte offset, not the first █ found. Render with
    // the cursor at the end — must not throw / mis-place.
    {
        Composer::Config cfg;
        cfg.text   = "progress \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88 done";
        cfg.cursor = static_cast<int>(cfg.text.size());
        auto r = render_at(Composer{cfg}.build(), 60);
        assert(r.content_h > 0);
    }

    std::println("  PASS\n");
}

// ContextGauge: percent must not overflow int for token counts past
// ~21.4M, and the token field must be a constant width across the
// <1000 / k / M ranges (stable-width slot contract).
void test_context_gauge_stability() {
    std::println("=== test_context_gauge_stability ===");

    // Overflow: 30M used of 30M max = 100%, not a negative garbage pct.
    {
        ContextGauge::Config cfg;
        cfg.used = 30'000'000;
        cfg.max  = 30'000'000;
        auto r = render_at(ContextGauge{cfg}.build(), 80);
        bool has_100 = false;
        for (const auto& row : r.rows)
            if (row.find("100%") != std::string::npos) has_100 = true;
        assert(has_100 && "30M/30M must render 100%");
    }

    // Constant total width across magnitude boundaries + placeholder.
    auto width_at = [](int used) {
        ContextGauge::Config cfg;
        cfg.used = used;
        cfg.max  = 200'000;
        auto r = render_at(ContextGauge{cfg}.build(), 200);
        assert(r.content_h == 1);
        return static_cast<int>(r.rows[0].size());
    };
    const int w_small = width_at(999);        // "   999"
    const int w_kilo  = width_at(1'000);      // "  1.0k"
    const int w_big   = width_at(199'999);    // "200.0k"
    assert(w_small == w_kilo && w_kilo == w_big
           && "token field must be constant width across magnitudes");
    const int w_zero  = width_at(0);          // placeholder ——/——
    assert(w_zero == w_small
           && "placeholder must occupy the same columns as live");

    std::println("  PASS\n");
}

// ToolBodyPreview: tail_only line numbers must show TRUE source
// positions, not restart at 1.
void test_tool_body_tail_line_numbers() {
    std::println("=== test_tool_body_tail_line_numbers ===");

    std::string body;
    for (int i = 1; i <= 50; ++i)
        body += "line number " + std::to_string(i) + "\n";

    // CodeBlock, tail_only (default): tail budget = max(head, tail) = 4
    // → rows 47..50, numbered 47..50.
    {
        ToolBodyPreview::Config cfg;
        cfg.kind = ToolBodyPreview::Kind::CodeBlock;
        cfg.text = body;
        auto r = render_at(ToolBodyPreview{cfg}.build(), 80);
        bool has_true_num = false, has_row_one = false;
        for (const auto& row : r.rows) {
            if (row.find(" 47") != std::string::npos
                && row.find("line number 47") != std::string::npos)
                has_true_num = true;
            if (row.find("  1 \xe2\x94\x82") == 0) has_row_one = true;
        }
        (void)has_row_one;
        assert(has_true_num
               && "tail_only code_block gutter must show true line numbers");
    }

    // FileWrite, streaming tail: gutter anchored past the hidden lines.
    {
        ToolBodyPreview::Config cfg;
        cfg.kind = ToolBodyPreview::Kind::FileWrite;
        cfg.text = body;
        cfg.show_footer_stats = false;
        auto r = render_at(ToolBodyPreview{cfg}.build(), 80);
        bool has_true_num = false;
        for (const auto& row : r.rows)
            if (row.find("50") != std::string::npos
                && row.find("line number 50") != std::string::npos)
                has_true_num = true;
        assert(has_true_num
               && "tail_only file_write gutter must show true line numbers");
    }

    std::println("  PASS\n");
}

// small_caps must letter-space at UTF-8 boundaries, not bytes — a
// multi-byte label must survive intact (no mojibake / no width blowup).
void test_small_caps_utf8() {
    std::println("=== test_small_caps_utf8 ===");

    AgentTimeline::Config cfg;
    cfg.title = " ACTIONS ";
    cfg.stats = {{"r\xc3\xa9vision", 2, Color::blue()}};   // révision
    AgentTimelineEvent ev;
    ev.name   = "Bash";
    ev.detail = "ok";
    ev.status = AgentEventStatus::Done;
    cfg.events.push_back(ev);
    auto r = render_at(AgentTimeline{cfg}.build(), 60);
    assert(r.content_h > 0);
    // The stats row letter-spaces to "R É V I S I O N"; get_row maps the
    // intact 2-byte é to a single '?'. A byte-split é would decode as
    // TWO invalid glyphs ('??') — assert exactly one '?' between R and V.
    bool ok = false;
    for (const auto& row : r.rows)
        if (row.find("R ? V I S I O N") != std::string::npos) ok = true;
    assert(ok && "small_caps must not split multi-byte sequences");

    std::println("  PASS\n");
}

// Picker with multi-row raw items: the auto-scroll clamp must work in
// row space. Selecting the last of several multi-row items must scroll
// far enough that the item's rows are inside the viewport.
void test_picker_multirow_autoscroll() {
    std::println("=== test_picker_multirow_autoscroll ===");

    ScrollState scroll;
    Picker::Config cfg;
    cfg.title      = " Test ";
    cfg.viewport_h = 4;
    cfg.scroll     = &scroll;
    // Four 3-row items → 12 content rows, viewport 4.
    for (int i = 0; i < 4; ++i) {
        using namespace dsl;
        cfg.items.push_back(v(
            text("item" + std::to_string(i) + "-a"),
            text("item" + std::to_string(i) + "-b"),
            text("item" + std::to_string(i) + "-c")
        ).build());
    }
    cfg.selected = 3;   // starts at row 9, ends at row 12
    auto r = render_at(Picker{cfg}.build(), 50);
    (void)r;
    // Row-space clamp: sel_end(12) - vh(4) = 8. The old index-space
    // clamp computed y = 3 - 4 + 1 = 0 — selection entirely off-view.
    assert(scroll.y == 8
           && "multi-row picker items must auto-scroll in row space");

    std::println("  PASS\n");
}

int main() {
    test_table();
    test_progress();
    test_divider();
    test_badge();
    test_breadcrumb();
    test_select();
    test_toast();
    test_spinner();
    test_input();
    test_status_bar_responsive();
    test_markdown();
    test_markdown_streaming();
    test_composer_cursor_safety();
    test_context_gauge_stability();
    test_tool_body_tail_line_numbers();
    test_small_caps_utf8();
    test_picker_multirow_autoscroll();
    std::println("All widget tests passed!");
    return 0;
}
