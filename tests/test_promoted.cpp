// test_promoted.cpp — Tests for inline mode resize (no alt screen promotion)
//
// Verifies:
//   - Inline rendering works at various widths (simulating resize)
//   - Content stays left-aligned after width changes
//   - No alt screen sequences in output
//   - Markdown with ! renders without hanging
//   - Streaming markdown works across resize

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

void dump(const Canvas& canvas, int rows = -1) {
    int ch = content_height(canvas);
    if (rows < 0) rows = ch;
    rows = std::min(rows, canvas.height());
    for (int y = 0; y < rows; ++y) {
        auto row = get_row(canvas, y);
        if (!row.empty())
            std::println("    {:2}|{}|", y, row);
    }
}

struct RenderResult {
    int content_h;
    std::vector<std::string> rows;
};

RenderResult render_chat(ChatView& chat, int w, int h = 500) {
    chat.resize(w, h);
    StylePool pool;
    Canvas canvas(std::max(1, w - 1), h, &pool);
    canvas.clear();
    render_tree(chat.build(), canvas, pool, theme::dark, /*auto_height=*/true);
    int ch = content_height(canvas);
    std::vector<std::string> rows;
    for (int y = 0; y < ch; ++y)
        rows.push_back(get_row(canvas, y));
    return {ch, std::move(rows)};
}

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

// ============================================================================
// Test: Inline render produces correct content at various widths
// ============================================================================
void test_inline_resize() {
    std::println("=== test_inline_resize ===");

    ChatView chat;
    chat.add_user("hi");
    chat.begin_stream();
    chat.stream_set("I'll help you with that!");
    chat.end_stream();

    for (int w : {40, 80, 120, 160}) {
        auto r = render_chat(chat, w);
        std::println("  w={}: {} rows", w, r.content_h);
        assert(r.content_h >= 3);  // at least: user msg, assistant label, text

        // Content must start near column 0
        for (int y = 0; y < r.content_h; ++y) {
            if (r.rows[y].empty()) continue;
            auto first = r.rows[y].find_first_not_of(' ');
            if (first != std::string::npos) {
                assert(static_cast<int>(first) <= 15);
            }
        }
    }

    std::println("  PASS\n");
}

// ============================================================================
// Test: Content is consistent across width changes (no garbling)
// ============================================================================
void test_resize_content_consistency() {
    std::println("=== test_resize_content_consistency ===");

    ChatView chat;
    chat.add_user("test");
    chat.begin_stream();
    chat.stream_set("Hello **world**! This is a response.");
    chat.end_stream();

    // Render at 80, then 120, then back to 80
    auto r1 = render_chat(chat, 80);
    auto r2 = render_chat(chat, 120);
    auto r3 = render_chat(chat, 80);

    // First and third render (same width) should be identical
    assert(r1.content_h == r3.content_h);
    for (int y = 0; y < r1.content_h; ++y) {
        assert(r1.rows[y] == r3.rows[y]);
    }

    std::println("  80→120→80: content matches ({} rows)", r1.content_h);
    std::println("  PASS\n");
}

// ============================================================================
// Test: Serialized output never contains alt screen sequences
// ============================================================================
void test_no_alt_screen_in_output() {
    std::println("=== test_no_alt_screen_in_output ===");

    ChatView chat;
    chat.add_user("hi");
    chat.begin_stream();
    chat.stream_set("Response!");
    chat.end_stream();
    chat.resize(80, 24);

    StylePool pool;
    Canvas canvas(79, 500, &pool);
    render_tree(chat.build(), canvas, pool, theme::dark, /*auto_height=*/true);

    std::string out;
    serialize(canvas, pool, out);

    assert(!contains(out, "\x1b[?1049h"));
    assert(!contains(out, "\x1b[?1049l"));

    std::println("  {} bytes, no alt screen sequences", out.size());
    std::println("  PASS\n");
}

// ============================================================================
// Test: Multi-message chat survives resize
// ============================================================================
void test_multi_message_resize() {
    std::println("=== test_multi_message_resize ===");

    ChatView chat;
    chat.add_user("first");
    chat.begin_stream();
    chat.stream_set("Response one");
    chat.end_stream();
    chat.add_user("second");
    chat.begin_stream();
    chat.stream_set("Response two with **bold** text.");
    chat.end_stream();

    for (int w : {60, 100, 60}) {
        auto r = render_chat(chat, w);
        std::println("  w={}: {} rows", w, r.content_h);
        assert(r.content_h >= 6);  // 2 user + 2 assistant msgs + labels

        // Check no row overflows width
        for (int y = 0; y < r.content_h; ++y) {
            assert(static_cast<int>(r.rows[y].size()) <= w);
        }
    }

    std::println("  PASS\n");
}

// ============================================================================
// Test: Tool cards + markdown survive resize
// ============================================================================
void test_tools_and_markdown_resize() {
    std::println("=== test_tools_and_markdown_resize ===");

    ChatView chat;
    chat.add_user("hi");
    chat.add_tool("read_file",
        "Language: C++\nStandard: C++26\nCompiler: g++-15");
    chat.begin_stream();
    chat.stream_set(
        "Here's what I found:\n\n"
        "- **Maya** is a C++26 TUI framework\n"
        "- Uses SIMD-accelerated diffing\n\n"
        "The project looks great!");
    chat.end_stream();

    auto r80 = render_chat(chat, 80);
    auto r120 = render_chat(chat, 120);
    auto r80b = render_chat(chat, 80);

    // Same width → same output
    assert(r80.content_h == r80b.content_h);
    for (int y = 0; y < r80.content_h; ++y) {
        assert(r80.rows[y] == r80b.rows[y]);
    }

    std::println("  80: {} rows, 120: {} rows, 80 again: {} rows",
                 r80.content_h, r120.content_h, r80b.content_h);
    std::println("  PASS\n");
}

// ============================================================================
// Test: Markdown with ! doesn't hang
// ============================================================================
void test_markdown_exclamation() {
    std::println("=== test_markdown_exclamation ===");

    const char* cases[] = {
        "Hello!",
        "Done!",
        "Help! More text",
        "A!\n\nB!\n\n- Item",
        "I'll help you with that!\n\nHere's a quick overview:\n\n"
        "- **Maya** is a C++26 TUI framework\n"
        "- It uses **compile-time** DSL for type-safe UI\n"
        "- SIMD-accelerated terminal diffing\n"
        "- Flexbox layout engine\n\n"
        "The framework is designed for **high-performance** terminal applications.",
    };

    for (auto* src : cases) {
        auto elem = markdown(src);
        StylePool pool;
        Canvas canvas(80, 500, &pool);
        render_tree(elem, canvas, pool, theme::dark, /*auto_height=*/true);
        int ch = content_height(canvas);
        std::string label(src, std::min(strlen(src), size_t{40}));
        if (strlen(src) > 40) label += "...";
        for (auto& c : label) if (c == '\n') c = ' ';
        std::println("  ok: \"{}\" → {} rows", label, ch);
        assert(ch > 0);
    }

    std::println("  PASS\n");
}

// ============================================================================
// Test: Streaming markdown across simulated resize
// ============================================================================
void test_streaming_across_resize() {
    std::println("=== test_streaming_across_resize ===");

    ChatView chat;
    chat.add_user("1");
    chat.begin_stream();

    std::vector<std::string> tokens = {
        "I", "'ll", " help", " you", " with", " that", "!\n\n",
        "Here", "'s", " a", " quick", " overview", ":\n\n",
        "- ", "**Maya**", " is", " a", " framework",
    };

    int widths[] = {80, 80, 80, 80, 80, 80, 80,
                    120, 120, 120, 120, 120, 120,
                    80, 80, 80, 80, 80};

    for (size_t i = 0; i < tokens.size(); ++i) {
        chat.stream_append(tokens[i]);
        chat.advance(1.0f / 30.0f);
        auto r = render_chat(chat, widths[i]);
        // Should not crash or produce zero output
        assert(r.content_h > 0);
    }
    chat.end_stream();

    auto final_r = render_chat(chat, 80);
    std::println("  streamed {} tokens across resize, final: {} rows",
                 tokens.size(), final_r.content_h);
    for (int y = 0; y < final_r.content_h; ++y) {
        if (!final_r.rows[y].empty())
            std::println("    {:2}|{}|", y, final_r.rows[y]);
    }
    assert(final_r.content_h > 3);
    std::println("  PASS\n");
}

int main() {
    test_inline_resize();
    test_resize_content_consistency();
    test_no_alt_screen_in_output();
    test_multi_message_resize();
    test_tools_and_markdown_resize();
    test_markdown_exclamation();
    test_streaming_across_resize();
    std::println("All promoted/resize tests passed!");
    return 0;
}
