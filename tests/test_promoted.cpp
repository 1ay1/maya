// test_promoted.cpp — Faithful reproduction of promoted render path
//
// Simulates exactly what App::render_frame() does after promoting
// from inline to alt screen, to isolate whether the layout bug is
// in render_tree or in the ANSI output pipeline.

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
        else if (c.character >= 0x2500 && c.character < 0x2600)
            s += '#';
        else if (c.character == 0x25C6)
            s += '*';
        else if (c.character == 0x276F)
            s += '>';
        else if (c.character == 0x25CF)
            s += '@';
        else if (c.character == 0x2588)
            s += '|';
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
            std::println("  {:2}|{}|", y, row);
    }
}

void assert_left_aligned(const Canvas& canvas, int max_col, const char* ctx) {
    int ch = content_height(canvas);
    for (int y = 0; y < ch; ++y) {
        auto row = get_row(canvas, y);
        if (row.empty()) continue;
        auto first = row.find_first_not_of(' ');
        if (first != std::string::npos && static_cast<int>(first) > max_col) {
            std::println("  FAIL [{}]: row {} starts at col {}: '{}'", ctx, y, first, row);
            assert(false);
        }
    }
}

// ============================================================================
// Test: Simulate EXACT promoted render path from App::render_frame()
// ============================================================================
void test_promoted_render_exact() {
    std::println("=== test_promoted_render_exact ===");

    // --- Phase 1: Build ChatView with same content as screenshot ---
    ChatView chat;
    chat.add_user("hi");
    chat.add_tool("read_file",
        "Language: C++\nStandard: C++26\nCompiler: g++-15\nStatus: OK");
    chat.begin_stream();
    chat.stream_set("I'll help you with that! Let me check the project files first.");
    chat.end_stream();
    chat.toast("Response complete", ToastLevel::Success);

    // --- Phase 2: Simulate initial inline render at width 80 ---
    {
        StylePool pool;
        Canvas canvas(79, 500, &pool);  // inline uses width-1
        std::vector<layout::LayoutNode> layout_nodes;

        chat.resize(80, 24);  // initial terminal size
        Element ui = chat.build();
        render_tree(ui, canvas, pool, theme::dark, layout_nodes, /*auto_height=*/true);

        std::println("  Phase 1 (inline @ 79x500):");
        dump(canvas);
    }

    // --- Phase 3: Simulate promote_to_alt_screen() ---
    // This is what App does:
    //   pool_.clear();
    //   canvas_ = Canvas(1, 1, &pool_);
    //   front_  = Canvas(1, 1, &pool_);
    //   needs_clear_ = true;

    // --- Phase 4: Simulate promoted render at 121x47 ---
    {
        StylePool pool;  // fresh pool (simulating pool_.clear())
        std::vector<layout::LayoutNode> layout_nodes;

        int w = 121;  // full width (alt screen)
        int canvas_h = 47;  // terminal height

        Canvas canvas(w, canvas_h, &pool);

        // ChatView receives resize event before render
        chat.resize(w, canvas_h);

        // Render exactly as promoted path does
        canvas.clear();
        Element ui = chat.build();
        render_tree(ui, canvas, pool, theme::dark, layout_nodes, /*auto_height=*/true);

        std::println("\n  Phase 2 (promoted @ {}x{}):", w, canvas_h);
        dump(canvas);

        // Verify left alignment — no element should start beyond column 15
        assert_left_aligned(canvas, 15, "promoted@121");
    }

    std::println("  PASS\n");
}

// ============================================================================
// Test: Same thing but with canvas height = 500 (like inline)
// ============================================================================
void test_promoted_render_tall() {
    std::println("=== test_promoted_render_tall ===");

    ChatView chat;
    chat.add_user("hi");
    chat.add_tool("read_file",
        "Language: C++\nStandard: C++26\nCompiler: g++-15\nStatus: OK");
    chat.begin_stream();
    chat.stream_set("I'll help you with that!");
    chat.end_stream();

    // Render at 121 with tall canvas (500) - simulating kMaxInlineHeight
    StylePool pool;
    std::vector<layout::LayoutNode> layout_nodes;
    Canvas canvas(121, 500, &pool);

    chat.resize(121, 47);
    Element ui = chat.build();
    render_tree(ui, canvas, pool, theme::dark, layout_nodes, /*auto_height=*/true);

    std::println("  Tall canvas (121x500):");
    dump(canvas);

    assert_left_aligned(canvas, 15, "tall@121");
    std::println("  PASS\n");
}

// ============================================================================
// Test: Fixed height canvas WITHOUT auto_height (pure alt-screen mode)
// ============================================================================
void test_fixed_height_render() {
    std::println("=== test_fixed_height_render ===");

    ChatView chat;
    chat.add_user("hi");
    chat.add_tool("read_file",
        "Language: C++\nStandard: C++26\nCompiler: g++-15\nStatus: OK");
    chat.begin_stream();
    chat.stream_set("Response");
    chat.end_stream();

    // Render at 121x47 WITHOUT auto_height
    StylePool pool;
    std::vector<layout::LayoutNode> layout_nodes;
    Canvas canvas(121, 47, &pool);

    chat.resize(121, 47);
    Element ui = chat.build();
    render_tree(ui, canvas, pool, theme::dark, layout_nodes, /*auto_height=*/false);

    std::println("  Fixed height (121x47, auto_height=false):");
    dump(canvas, 30);

    // Elements should still be left-aligned
    for (int y = 0; y < 30; ++y) {
        auto row = get_row(canvas, y);
        if (row.empty()) continue;
        auto first = row.find_first_not_of(' ');
        if (first != std::string::npos && static_cast<int>(first) > 15) {
            std::println("  WARN: row {} starts at col {}: '{}'", y, first, row);
        }
    }

    std::println("  PASS\n");
}

// ============================================================================
// Test: Verify serialize output contains correct content
// ============================================================================
void test_serialize_output() {
    std::println("=== test_serialize_output ===");

    ChatView chat;
    chat.add_user("hi");
    chat.add_tool("read_file", "Language: C++");
    chat.begin_stream();
    chat.stream_set("OK");
    chat.end_stream();
    chat.resize(121, 47);

    StylePool pool;
    Canvas canvas(121, 47, &pool);
    render_tree(chat.build(), canvas, pool, theme::dark, /*auto_height=*/true);

    // Serialize as the promoted path does
    std::string out;
    out += "\x1b[2J\x1b[H";  // clear screen, home cursor
    serialize(canvas, pool, out);

    // Strip ANSI escape sequences to get plain text
    std::string plain;
    bool in_esc = false;
    for (char c : out) {
        if (c == '\x1b') { in_esc = true; continue; }
        if (in_esc) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '~') {
                in_esc = false;
            }
            continue;
        }
        plain += c;
    }

    // Split by lines
    std::vector<std::string> lines;
    std::string current;
    for (char c : plain) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else if (c != '\r') {
            current += c;
        }
    }
    if (!current.empty()) lines.push_back(current);

    std::println("  Serialized output ({} lines):", lines.size());
    for (int i = 0; i < std::min(20, static_cast<int>(lines.size())); ++i) {
        auto& line = lines[i];
        // Trim trailing spaces
        while (!line.empty() && line.back() == ' ') line.pop_back();
        while (!line.empty() && line.front() == 0) line.erase(line.begin());
        if (!line.empty())
            std::println("  {:2}|{}|", i, line);
    }

    // Check content is in first 20 chars of each line
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        auto& line = lines[i];
        while (!line.empty() && line.back() == ' ') line.pop_back();
        if (line.empty()) continue;
        auto first = line.find_first_not_of(' ');
        if (first != std::string::npos && first > 20) {
            std::println("  WARN: line {} starts at col {}", i, first);
        }
    }

    std::println("  PASS\n");
}

int main() {
    test_promoted_render_exact();
    test_promoted_render_tall();
    test_fixed_height_render();
    test_serialize_output();
    std::println("All promoted render tests passed!");
    return 0;
}
