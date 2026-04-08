// test_widgets.cpp — Verify all widgets render correctly at various widths
#include <maya/maya.hpp>
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

bool any_row_exceeds(const RenderResult& r, int max_col) {
    for (auto& row : r.rows) {
        if (static_cast<int>(row.size()) > max_col) return true;
    }
    return false;
}

// ============================================================================
// Table tests
// ============================================================================
void test_table() {
    std::println("=== test_table ===");

    // Auto-width table
    Table tbl({{"Property", 0}, {"Value", 0}});
    tbl.add_row({"Language", "C++"});
    tbl.add_row({"Standard", "C++26"});
    tbl.add_row({"Compiler", "g++-15"});

    for (int w : {20, 30, 40, 60, 80}) {
        auto r = render_at(tbl, w);
        dump(std::format("w={}", w), r);
        // Check no row content exceeds canvas width
        for (int y = 0; y < r.content_h; ++y) {
            if (static_cast<int>(r.rows[y].size()) > w) {
                std::println("  WARN: row {} exceeds width {} (len={})", y, w,
                             r.rows[y].size());
            }
        }
    }

    // Fixed-width columns wider than terminal
    std::println("\n  Fixed-width columns (30+30 in 40-wide terminal):");
    Table wide_tbl({{"Name", 30}, {"Value", 30}});
    wide_tbl.add_row({"test", "value"});
    auto r = render_at(wide_tbl, 40);
    dump("w=40", r);

    // Fixed-height rendering (alt-screen mode)
    std::println("\n  Fixed-height (w=80, h=10, auto_height=false):");
    auto r2 = render_at(tbl, 80, 10, false);
    dump("w=80,h=10", r2);

    std::println("  DONE\n");
}

// ============================================================================
// ProgressBar tests
// ============================================================================
void test_progress() {
    std::println("=== test_progress ===");

    ProgressBar bar;
    bar.set(0.5f);

    for (int w : {20, 40, 60, 80}) {
        auto r = render_at(bar, w);
        dump(std::format("w={}", w), r);
        for (int y = 0; y < r.content_h; ++y) {
            if (static_cast<int>(r.rows[y].size()) > w) {
                std::println("  WARN: row {} exceeds width {} (len={})", y, w,
                             r.rows[y].size());
            }
        }
    }
    std::println("  DONE\n");
}

// ============================================================================
// Divider tests
// ============================================================================
void test_divider() {
    std::println("=== test_divider ===");

    Divider plain;
    Divider labeled("Section");

    for (int w : {20, 40, 80}) {
        auto r1 = render_at(plain, w);
        auto r2 = render_at(labeled, w);
        dump(std::format("plain w={}", w), r1);
        dump(std::format("labeled w={}", w), r2);
    }
    std::println("  DONE\n");
}

// ============================================================================
// Badge tests
// ============================================================================
void test_badge() {
    std::println("=== test_badge ===");

    auto b = tool_badge("read_file");
    for (int w : {15, 20, 40, 80}) {
        auto r = render_at(b, w);
        dump(std::format("w={}", w), r);
    }
    std::println("  DONE\n");
}

// ============================================================================
// Breadcrumb tests
// ============================================================================
void test_breadcrumb() {
    std::println("=== test_breadcrumb ===");

    Breadcrumb bc({"project", "src", "widget", "table.hpp"});
    for (int w : {20, 40, 80}) {
        auto r = render_at(bc, w);
        dump(std::format("w={}", w), r);
    }
    std::println("  DONE\n");
}

// ============================================================================
// StatusBar tests
// ============================================================================
void test_statusbar() {
    std::println("=== test_statusbar ===");

    StatusBar bar;
    bar.set_left({" ready ", "main"});
    bar.set_right({"3 files", "UTF-8"});

    for (int w : {30, 60, 80, 120}) {
        auto r = render_at(bar, w);
        dump(std::format("w={}", w), r);
    }
    std::println("  DONE\n");
}

// ============================================================================
// Select tests
// ============================================================================
void test_select() {
    std::println("=== test_select ===");

    Select menu({"Option A", "Option B", "Long option with lots of text"});
    for (int w : {20, 40, 80}) {
        auto r = render_at(menu, w);
        dump(std::format("w={}", w), r);
    }
    std::println("  DONE\n");
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
        dump(std::format("w={}", w), r);
    }
    std::println("  DONE\n");
}

// ============================================================================
// Confirm tests
// ============================================================================
void test_confirm() {
    std::println("=== test_confirm ===");

    Confirm dialog("Delete this file?");
    for (int w : {25, 40, 80}) {
        auto r = render_at(dialog, w);
        dump(std::format("w={}", w), r);
    }
    std::println("  DONE\n");
}

// ============================================================================
// Spinner tests
// ============================================================================
void test_spinner_widget() {
    std::println("=== test_spinner ===");

    Spinner spin;
    for (int w : {10, 20, 40}) {
        auto r = render_at(spin, w);
        dump(std::format("w={}", w), r);
    }
    std::println("  DONE\n");
}

// ============================================================================
// Input tests
// ============================================================================
void test_input_widget() {
    std::println("=== test_input ===");

    Input inp;
    inp.set_value("hello world this is a long input text");
    for (int w : {15, 30, 60, 80}) {
        auto r = render_at(inp, w);
        dump(std::format("w={}", w), r);
    }
    std::println("  DONE\n");
}

// ============================================================================
// Scroll tests (in fixed-height mode simulating alt-screen)
// ============================================================================
void test_scroll_widget() {
    std::println("=== test_scroll ===");

    ScrollState state;
    auto content_fn = [](int /*w*/, int /*h*/) -> Element {
        std::vector<Element> items;
        for (int i = 0; i < 20; ++i) {
            items.push_back(Element{TextElement{
                .content = std::format("Line {}: some content here", i),
            }});
        }
        return detail::vstack()(std::move(items));
    };

    auto s = scroll({.auto_bottom = false, .show_bar = true}, state, content_fn);

    // auto_height=true — should show all content
    {
        auto r = render_at(Element(s), 60, 500, true);
        std::println("  auto_height=true: {} rows", r.content_h);
        assert(r.content_h >= 20);
    }

    // auto_height=false, h=10 — should show viewport of 10 rows
    {
        auto r = render_at(Element(s), 60, 10, false);
        std::println("  auto_height=false h=10: {} rows", r.content_h);
        dump("h=10", r);
        // Should show first 10 lines (no scroll offset yet)
        assert(r.rows[0].find("Line 0") != std::string::npos);
    }

    // Scroll down and render again
    state.scroll_by(5);
    {
        auto r = render_at(Element(s), 60, 10, false);
        std::println("  after scroll_by(5): {} rows", r.content_h);
        dump("scrolled", r);
        // Should now show Line 5 at top
        assert(r.rows[0].find("Line 5") != std::string::npos);
    }

    std::println("  DONE\n");
}

// ============================================================================
// Markdown streaming test — reproduces the chat streaming scenario
// ============================================================================
// ============================================================================
// Markdown streaming test — reproduces the chat streaming scenario
// ============================================================================
void test_markdown_streaming() {
    std::println("=== test_markdown_streaming ===");

    // Test 1: simple paragraph — does it render?
    {
        std::println("  Test 1: simple paragraph");
        auto elem = markdown("Hello **world**");
        auto r = render_at(elem, 80);
        dump("simple", r);
    }

    // Test 2: list with bold
    {
        std::println("  Test 2: list with bold");
        auto elem = markdown("- **Maya** is a framework\n- Item two");
        auto r = render_at(elem, 80);
        dump("list", r);
    }

    // Test 3: streaming append token by token, testing each
    {
        std::println("  Test 3: streaming token by token");
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
        std::string accumulated;
        int frame = 0;
        for (auto& tok : tokens) {
            accumulated += tok;
            md.append(tok);
            std::println("    frame {}: appending \"{}\" (total {} bytes)", frame,
                         tok == "\n" ? "\\n" : tok, accumulated.size());
            auto elem = md.build();
            std::println("    frame {}: built element, now rendering...", frame);
            auto r = render_at(elem, 80);
            std::println("    frame {}: rendered → {} rows", frame, r.content_h);
            ++frame;
        }
    }

    std::println("  DONE\n");
}

int main() {
    std::fflush(stdout);
    auto test_md = [](const char* label, const char* src, int width = 80) {
        std::println("  testing: {} (w={})", label, width);
        std::fflush(stdout);
        auto elem = markdown(src);
        std::println("    parsed");
        std::fflush(stdout);
        auto r = render_at(elem, width);
        std::println("    rendered: {} rows", r.content_h);
        std::fflush(stdout);
    };
    test_md("simple bold", "Hello **world**");
    test_md("two paragraphs", "First para\n\nSecond **bold** para");
    test_md("list plain", "- Item one\n- Item two");
    test_md("list with bold", "- **Maya** is a framework\n- Item two");
    test_md("para + list",
        "Overview:\n\n"
        "- **Maya** is a framework\n"
        "- Item two");
    test_md("list 4 items",
        "- **Maya** is a C++26 TUI framework\n"
        "- It uses **compile-time** DSL for type-safe UI\n"
        "- SIMD-accelerated terminal diffing\n"
        "- Flexbox layout engine");
    test_md("list + trailing para",
        "- Item one\n"
        "- Item two\n\n"
        "Trailing paragraph.");
    test_md("excl-1blk", "AA!");
    test_md("excl-2blk", "AA!\n\nBB");
    test_md("excl-3blk", "AA!\n\nBB\n\n- X");
    test_md("full response",
        "I'll help you with that!\n\n"
        "Here's a quick overview:\n\n"
        "- **Maya** is a C++26 TUI framework\n"
        "- It uses **compile-time** DSL for type-safe UI\n"
        "- SIMD-accelerated terminal diffing\n"
        "- Flexbox layout engine\n\n"
        "The framework is designed for **high-performance** terminal applications.");
    std::println("ALL PASSED");
    std::fflush(stdout);
    return 0;

    test_markdown_streaming();
    test_table();
    test_progress();
    test_divider();
    test_badge();
    test_breadcrumb();
    test_statusbar();
    test_select();
    test_toast();
    test_confirm();
    test_spinner_widget();
    test_input_widget();
    test_scroll_widget();

    std::println("All widget tests completed!");
    return 0;
}
