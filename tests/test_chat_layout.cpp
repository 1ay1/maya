// test_chat_layout.cpp — Headless ChatView layout verification
//
// Renders ChatView to a canvas and dumps the layout so we can
// inspect positioning without running the terminal app.

#include <maya/maya.hpp>
#include <cassert>
#include <print>
#include <string>

using namespace maya;

std::string get_row(const Canvas& canvas, int y) {
    std::string s;
    for (int x = 0; x < canvas.width(); ++x) {
        Cell c = canvas.get(x, y);
        if (c.character >= 0x20 && c.character < 0x7F)
            s += static_cast<char>(c.character);
        else if (c.character == 0x2500)
            s += '-';  // ─ → '-'
        else if (c.character >= 0x2500 && c.character < 0x2600)
            s += '#';  // other box-drawing → '#'
        else if (c.character == 0x25C6)
            s += '*';  // ◆ → '*'
        else if (c.character == 0x276F)
            s += '>';  // ❯ → '>'
        else if (c.character == 0x25CF)
            s += '@';  // ● → '@'
        else if (c.character == 0x2588)
            s += '|';  // █ cursor block
        else if (c.character != U' ' && c.character != 0)
            s += '?';  // unknown non-ascii
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
    std::println("  Canvas: {}x{}, content_height={}", canvas.width(), canvas.height(), ch);
    for (int y = 0; y < rows; ++y) {
        auto row = get_row(canvas, y);
        std::println("  {:2}|{}|", y, row);
    }
}

// ============================================================================
// Test 1: Empty ChatView — should show placeholder, divider, prompt, status
// ============================================================================
void test_empty_chat() {
    std::println("=== test_empty_chat (width=80) ===");
    ChatView chat;
    Element ui = chat.build();

    StylePool pool;
    Canvas canvas(80, 500, &pool);

    // Also test without auto_height to compare
    std::println("  With auto_height=true:");
    render_tree(ui, canvas, pool, theme::dark, /*auto_height=*/true);
    dump(canvas);

    // Reproduce ChatView structure exactly
    std::println("  ChatView-like structure:");
    {
        std::string div_str;
        for (int i = 0; i < 300; ++i) div_str += "\xe2\x94\x80";

        auto status = detail::hstack()(
            Element{TextElement{.content = "ready"}},
            detail::box().grow(1),
            Element{TextElement{.content = "0 msgs"}}
        );

        std::vector<Element> layout;
        layout.push_back(Element{TextElement{.content = "placeholder"}});
        layout.push_back(Element{TextElement{
            .content = std::move(div_str),
            .style = Style{}.with_dim(),
            .wrap = TextWrap::NoWrap,
        }});
        layout.push_back(Element{TextElement{.content = "prompt"}});
        layout.push_back(std::move(status));

        Element root = detail::vstack()(std::move(layout));
        Canvas c2(80, 500, &pool);
        render_tree(root, c2, pool, theme::dark, /*auto_height=*/true);
        int ch2 = content_height(c2);
        std::println("  content_height={}", ch2);
        dump(c2, ch2);
    }

    // Row 0 should have the placeholder
    auto r0 = get_row(canvas, 0);
    std::println("  row0: '{}'", r0);
    assert(r0.find("Type a message") != std::string::npos);

    // Should have divider (---) somewhere
    int ch = content_height(canvas);
    bool found_divider = false;
    for (int y = 0; y < ch; ++y) {
        if (get_row(canvas, y).find("---") != std::string::npos) {
            found_divider = true;
            break;
        }
    }
    assert(found_divider);
    std::println("  PASS\n");
}

// ============================================================================
// Test 2: ChatView with messages — check vertical stacking
// ============================================================================
void test_messages_layout() {
    std::println("=== test_messages_layout (width=80) ===");
    ChatView chat;
    chat.add_user("hello world");
    chat.begin_stream();
    chat.stream_set("I can help with that!");
    chat.end_stream();
    Element ui = chat.build();

    StylePool pool;
    Canvas canvas(80, 500, &pool);
    render_tree(ui, canvas, pool, theme::dark, /*auto_height=*/true);
    dump(canvas, 20);

    int ch = content_height(canvas);
    std::println("  content_height = {}", ch);

    // Check that user message starts at row 0
    auto r0 = get_row(canvas, 0);
    std::println("  row0: '{}'", r0);
    assert(r0.find("hello world") != std::string::npos);

    // Find assistant label
    bool found_assistant = false;
    int assistant_row = -1;
    for (int y = 0; y < ch; ++y) {
        auto r = get_row(canvas, y);
        if (r.find("Assistant") != std::string::npos) {
            found_assistant = true;
            assistant_row = y;
            break;
        }
    }
    assert(found_assistant);
    std::println("  Assistant label at row {}", assistant_row);

    // Assistant label should be left-aligned (starts at column 0-2)
    auto ar = get_row(canvas, assistant_row);
    auto pos = ar.find("Assistant");
    std::println("  Assistant column = {}", pos);
    assert(pos < 5);  // should be near left edge

    std::println("  PASS\n");
}

// ============================================================================
// Test 3: Tool message layout
// ============================================================================
void test_tool_layout() {
    std::println("=== test_tool_layout (width=80) ===");
    ChatView chat;
    chat.add_user("hi");
    chat.add_tool("read_file", "Language: C++\nStandard: C++26");
    chat.begin_stream();
    chat.stream_set("Got it!");
    chat.end_stream();
    Element ui = chat.build();

    StylePool pool;
    Canvas canvas(80, 500, &pool);
    render_tree(ui, canvas, pool, theme::dark, /*auto_height=*/true);
    dump(canvas, 30);

    int ch = content_height(canvas);

    // Find "Property" header — should be near left edge
    for (int y = 0; y < ch; ++y) {
        auto r = get_row(canvas, y);
        if (r.find("Property") != std::string::npos) {
            auto pos = r.find("Property");
            std::println("  Property header at row {}, col {}", y, pos);
            assert(pos < 20);  // should NOT be pushed to the right
            break;
        }
    }

    // Find "Language" — should be near left edge
    for (int y = 0; y < ch; ++y) {
        auto r = get_row(canvas, y);
        if (r.find("Language") != std::string::npos) {
            auto pos = r.find("Language");
            std::println("  Language at row {}, col {}", y, pos);
            assert(pos < 20);
            break;
        }
    }

    std::println("  PASS\n");
}

// ============================================================================
// Test 4: Different widths — layout should adapt
// ============================================================================
void test_width_adaptation() {
    std::println("=== test_width_adaptation ===");

    ChatView chat;
    chat.add_user("test");
    chat.add_tool("read_file", "Key: Value");
    chat.begin_stream();
    chat.stream_set("Response text here");
    chat.end_stream();

    for (int w : {40, 80, 120, 145}) {
        chat.resize(w, 24);
        Element ui = chat.build();

        StylePool pool;
        Canvas canvas(w, 500, &pool);
        render_tree(ui, canvas, pool, theme::dark, /*auto_height=*/true);

        int ch = content_height(canvas);
        std::println("  width={}: content_height={}", w, ch);
        dump(canvas, ch);

        // Everything should be left-aligned — no element should start
        // beyond column w/2
        for (int y = 0; y < ch; ++y) {
            auto r = get_row(canvas, y);
            if (r.empty()) continue;
            // Find first non-space
            auto first = r.find_first_not_of(' ');
            if (first != std::string::npos && first > static_cast<size_t>(w / 2)) {
                std::println("  FAIL: row {} starts at col {} (width={}): '{}'",
                             y, first, w, r);
                assert(false);
            }
        }
        std::println("  width={}: OK", w);
    }

    std::println("  PASS\n");
}

// ============================================================================
// Test 5: Promoted path simulation — render at new width after resize
// ============================================================================
void test_promoted_resize() {
    std::println("=== test_promoted_resize (80 -> 145) ===");

    ChatView chat;
    chat.add_user("hello");
    chat.add_tool("read_file", "Language: C++\nStandard: C++26\nCompiler: g++-15\nStatus: OK");
    chat.begin_stream();
    chat.stream_set("I'll help you with that! Let me check the project files first.");
    chat.end_stream();

    // First render at width 80 (caches elements)
    {
        Element ui = chat.build();
        StylePool pool;
        Canvas canvas(80, 500, &pool);
        render_tree(ui, canvas, pool, theme::dark, /*auto_height=*/true);
        std::println("  Initial render at 80:");
        dump(canvas);
    }

    // Simulate resize to 145
    chat.resize(145, 48);

    // Second render at width 145 (should invalidate caches)
    {
        Element ui = chat.build();
        StylePool pool;
        Canvas canvas(145, 500, &pool);
        render_tree(ui, canvas, pool, theme::dark, /*auto_height=*/true);
        std::println("  After resize to 145:");
        dump(canvas);

        int ch = content_height(canvas);
        // Check nothing is pushed far right
        for (int y = 0; y < ch; ++y) {
            auto r = get_row(canvas, y);
            if (r.empty()) continue;
            auto first = r.find_first_not_of(' ');
            if (first != std::string::npos && first > 72) {
                std::println("  FAIL: row {} starts at col {}: '{}'", y, first, r);
                assert(false);
            }
        }
    }

    std::println("  PASS\n");
}

int main() {
    test_empty_chat();
    test_messages_layout();
    test_tool_layout();
    test_width_adaptation();
    test_promoted_resize();
    std::println("All ChatView layout tests passed!");
    return 0;
}
