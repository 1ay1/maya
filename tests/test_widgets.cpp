// test_widgets.cpp — Verify all widgets render correctly at various widths
#include <maya/maya.hpp>
#include <maya/widget/badge.hpp>
#include <maya/widget/breadcrumb.hpp>
#include <maya/widget/divider.hpp>
#include <maya/widget/input.hpp>
#include <maya/widget/markdown.hpp>
#include <maya/widget/modal.hpp>
#include <maya/widget/progress.hpp>
#include <maya/widget/select.hpp>
#include <maya/widget/spinner.hpp>
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
    test_markdown();
    test_markdown_streaming();
    std::println("All widget tests passed!");
    return 0;
}
