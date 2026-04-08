// test_promoted.cpp — Tests for inline mode resize robustness
//
// Exercises every widget type through width-change cycles to verify
// no content garbling, overflow, or crashes after resize.

#include <maya/maya.hpp>
#include <cassert>
#include <print>
#include <string>
#include <vector>

using namespace maya;

// ============================================================================
// Helpers
// ============================================================================

std::string get_row(const Canvas& canvas, int y) {
    std::string s;
    for (int x = 0; x < canvas.width(); ++x) {
        Cell c = canvas.get(x, y);
        if (c.character >= 0x20 && c.character < 0x7F)
            s += static_cast<char>(c.character);
        else if (c.character != U' ' && c.character != 0)
            s += '?';
        else
            s += ' ';
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

struct RR {
    int content_h;
    std::vector<std::string> rows;
};

RR render_elem(const Element& elem, int w, int h = 500,
               bool auto_h = true) {
    StylePool pool;
    Canvas canvas(std::max(1, w - 1), h, &pool);
    canvas.clear();
    render_tree(elem, canvas, pool, theme::dark, auto_h);
    int ch = content_height(canvas);
    std::vector<std::string> rows;
    for (int y = 0; y < ch; ++y)
        rows.push_back(get_row(canvas, y));
    return {ch, std::move(rows)};
}

RR render_chat(ChatView& chat, int w, int h = 50) {
    chat.resize(w, h);
    return render_elem(chat.build(), w, h, /*auto_h=*/false);
}

void assert_fits(const RR& r, int w, const char* ctx) {
    for (int y = 0; y < r.content_h; ++y) {
        if (static_cast<int>(r.rows[y].size()) >= w) {
            std::println("  FAIL [{}]: row {} overflows w={} (len={}): {}",
                         ctx, y, w, r.rows[y].size(), r.rows[y]);
            assert(false);
        }
    }
}

void assert_stable(const RR& a, const RR& b, const char* ctx) {
    if (a.content_h != b.content_h) {
        std::println("  FAIL [{}]: height changed {} → {}", ctx, a.content_h, b.content_h);
        assert(false);
    }
    for (int y = 0; y < a.content_h; ++y) {
        if (a.rows[y] != b.rows[y]) {
            std::println("  FAIL [{}]: row {} differs", ctx, y);
            std::println("    before: |{}|", a.rows[y]);
            std::println("    after:  |{}|", b.rows[y]);
            assert(false);
        }
    }
}

// Render at w1 → w2 → w1 and assert first == last
void cycle(const Element& elem, int w1, int w2, const char* ctx) {
    auto r1 = render_elem(elem, w1);
    auto r2 = render_elem(elem, w2);
    auto r3 = render_elem(elem, w1);
    assert_fits(r1, w1, ctx);
    assert_fits(r2, w2, ctx);
    assert_stable(r1, r3, ctx);
}

// ============================================================================
// Test: Every standalone widget survives resize cycle
// ============================================================================
void test_all_widgets_resize() {
    std::println("=== test_all_widgets_resize ===");

    // Table
    {
        Table tbl({{"Key", 0}, {"Value", 0}});
        tbl.add_row({"Language", "C++26"});
        tbl.add_row({"Compiler", "g++-15"});
        tbl.add_row({"Framework", "Maya TUI"});
        cycle(tbl, 80, 40, "table-80-40");
        cycle(tbl, 120, 30, "table-120-30");
        std::println("  table: ok");
    }

    // ProgressBar
    {
        ProgressBar bar;
        bar.set(0.75f);
        cycle(bar, 80, 40, "progress-80-40");
        cycle(bar, 120, 20, "progress-120-20");
        std::println("  progress: ok");
    }

    // Divider
    {
        Divider plain;
        Divider labeled("Section Title");
        cycle(plain, 80, 40, "divider-plain");
        cycle(labeled, 80, 40, "divider-labeled");
        cycle(labeled, 120, 25, "divider-labeled-narrow");
        std::println("  divider: ok");
    }

    // Badge
    {
        auto b = tool_badge("read_file");
        cycle(b, 80, 20, "badge");
        std::println("  badge: ok");
    }

    // Breadcrumb
    {
        Breadcrumb bc({"project", "src", "widget", "table.hpp"});
        cycle(bc, 80, 30, "breadcrumb");
        std::println("  breadcrumb: ok");
    }

    // StatusBar
    {
        StatusBar bar;
        bar.set_left({" ready ", "main"});
        bar.set_right({"3 files", "UTF-8"});
        cycle(bar, 80, 40, "statusbar-80-40");
        cycle(bar, 120, 30, "statusbar-120-30");
        std::println("  statusbar: ok");
    }

    // Select
    {
        Select menu({"Option A", "Option B", "Long option with lots of text"});
        cycle(menu, 80, 25, "select");
        std::println("  select: ok");
    }

    // Toast
    {
        ToastManager toasts;
        toasts.push("File saved", ToastLevel::Success);
        toasts.push("Deprecated API", ToastLevel::Warning);
        cycle(toasts, 80, 30, "toast");
        std::println("  toast: ok");
    }

    // Confirm
    {
        Confirm dialog("Delete this file?");
        cycle(dialog, 80, 30, "confirm");
        std::println("  confirm: ok");
    }

    // Spinner
    {
        Spinner spin;
        cycle(spin, 80, 20, "spinner");
        std::println("  spinner: ok");
    }

    // Input
    {
        Input inp;
        inp.set_value("hello world this is a long input");
        cycle(inp, 80, 20, "input-80-20");
        cycle(inp, 120, 40, "input-120-40");
        std::println("  input: ok");
    }

    // Markdown (various patterns)
    {
        auto md_cycle = [](const char* label, const char* src) {
            auto elem = markdown(src);
            cycle(elem, 80, 40, label);
            cycle(elem, 120, 30, label);
        };
        md_cycle("md-bold", "Hello **world** and *italic*");
        md_cycle("md-list", "- **Maya** framework\n- SIMD diffing\n- Flexbox");
        md_cycle("md-code", "```cpp\nint x = 42;\n```");
        md_cycle("md-quote", "> Important note\n> continued here");
        md_cycle("md-table", "| A | B |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |");
        md_cycle("md-heading", "## Title\n\nBody text here.");
        md_cycle("md-full",
            "I'll help you with that!\n\n"
            "Here's a quick overview:\n\n"
            "- **Maya** is a C++26 TUI framework\n"
            "- It uses **compile-time** DSL for type-safe UI\n"
            "- SIMD-accelerated terminal diffing\n\n"
            "The framework is designed for **high-performance** apps.");
        std::println("  markdown: ok");
    }

    std::println("  PASS\n");
}

// ============================================================================
// Test: ChatView with all widget types survives resize
// ============================================================================
void test_chatview_full_resize() {
    std::println("=== test_chatview_full_resize ===");

    ChatView chat;

    // User message
    chat.add_user("Show me the project structure");

    // Tool card
    chat.add_tool("read_file",
        "Path: CMakeLists.txt\nLanguage: C++\nStandard: C++26\nCompiler: g++-15");

    // Assistant with markdown (code block, list, bold, exclamation)
    chat.begin_stream();
    chat.stream_set(
        "I found the project!\n\n"
        "## Overview\n\n"
        "Here's what I see:\n\n"
        "- **Maya** is a C++26 TUI framework\n"
        "- It uses compile-time DSL for type-safe UI\n"
        "- SIMD-accelerated terminal diffing\n\n"
        "```cpp\nmaya::run(\n    {.mode = Mode::Inline},\n    handler,\n    render\n);\n```\n\n"
        "> This is a well-structured project!\n\n"
        "The framework is designed for **high-performance** terminal applications.");
    chat.end_stream();

    // Second exchange
    chat.add_user("What widgets are available?");
    chat.begin_stream();
    chat.stream_set(
        "Here are the widgets:\n\n"
        "| Widget | Purpose |\n"
        "|--------|--------|\n"
        "| `Input` | Text input |\n"
        "| `Scroll` | Scrollable viewport |\n"
        "| `Table` | Tabular data |\n\n"
        "All widgets are **automatically responsive**.");
    chat.end_stream();

    // Resize cycle: wide → narrow → wide
    int widths[] = {120, 80, 60, 40, 60, 80, 120};
    RR first_120{};
    for (int i = 0; i < 7; ++i) {
        int w = widths[i];
        auto r = render_chat(chat, w);
        assert(r.content_h > 0);
        assert_fits(r, w, std::format("chatview-w{}", w).c_str());

        if (i == 0) first_120 = r;
        if (i == 6) assert_stable(first_120, r, "chatview-120-roundtrip");

        std::println("  w={}: {} rows", w, r.content_h);
    }

    std::println("  PASS\n");
}

// ============================================================================
// Test: Streaming during resize
// ============================================================================
void test_streaming_during_resize() {
    std::println("=== test_streaming_during_resize ===");

    ChatView chat;
    chat.add_user("1");
    chat.begin_stream();

    std::vector<std::string> tokens = {
        "I", "'ll", " help", " you", " with", " that", "!\n\n",
        "Here", "'s", " a", " quick", " overview", ":\n\n",
        "- ", "**Maya**", " is", " a", " C++26", " TUI", " framework", "\n",
        "- ", "It", " uses", " **compile-time**", " DSL", "\n",
        "- ", "SIMD", "-accelerated", " diffing", "\n\n",
        "The", " framework", " is", " **great**", "!",
    };

    // Alternate widths during streaming
    for (size_t i = 0; i < tokens.size(); ++i) {
        chat.stream_append(tokens[i]);
        chat.advance(1.0f / 30.0f);
        int w = (i % 3 == 0) ? 120 : (i % 3 == 1) ? 80 : 60;
        auto r = render_chat(chat, w);
        assert(r.content_h > 0);
        assert_fits(r, w, "streaming-resize");
    }
    chat.end_stream();

    // Final render at each width
    for (int w : {60, 80, 120}) {
        auto r = render_chat(chat, w);
        assert(r.content_h > 5);
        assert_fits(r, w, "streaming-final");
    }

    // Stable after stream ends
    auto r1 = render_chat(chat, 80);
    auto r2 = render_chat(chat, 120);
    auto r3 = render_chat(chat, 80);
    assert_stable(r1, r3, "streaming-stable");

    std::println("  {} tokens, stable after resize", tokens.size());
    std::println("  PASS\n");
}

// ============================================================================
// Test: Extreme widths (very narrow, very wide)
// ============================================================================
void test_extreme_widths() {
    std::println("=== test_extreme_widths ===");

    ChatView chat;
    chat.add_user("test");
    chat.add_tool("cmd", "Key: Value\nOther: Data");
    chat.begin_stream();
    chat.stream_set("**Bold** `code` *italic* ~~strike~~ [link](url)");
    chat.end_stream();

    for (int w : {10, 15, 20, 200, 300}) {
        auto r = render_chat(chat, w);
        assert(r.content_h > 0);
        assert_fits(r, w, std::format("extreme-w{}", w).c_str());
        std::println("  w={}: {} rows", w, r.content_h);
    }

    std::println("  PASS\n");
}

// ============================================================================
// Test: No alt screen sequences in any output
// ============================================================================
void test_no_fullscreen_sequences() {
    std::println("=== test_no_fullscreen_sequences ===");

    ChatView chat;
    chat.add_user("hi");
    chat.add_tool("read_file", "Status: OK");
    chat.begin_stream();
    chat.stream_set("Response!");
    chat.end_stream();

    for (int w : {40, 80, 120}) {
        chat.resize(w, 24);
        StylePool pool;
        Canvas canvas(w - 1, 500, &pool);
        render_tree(chat.build(), canvas, pool, theme::dark, /*auto_height=*/true);

        std::string out;
        serialize(canvas, pool, out);
        assert(out.find("\x1b[?1049h") == std::string::npos);
        assert(out.find("\x1b[?1049l") == std::string::npos);
    }

    std::println("  PASS\n");
}

int main() {
    test_all_widgets_resize();
    test_chatview_full_resize();
    test_streaming_during_resize();
    test_extreme_widths();
    test_no_fullscreen_sequences();
    std::println("All resize tests passed!");
    return 0;
}
