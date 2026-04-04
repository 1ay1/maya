// Tests for border rendering: single, double, round, bold, classic styles
#include <maya/maya.hpp>
#include <cassert>
#include <print>

using namespace maya;
using namespace maya::detail;
using namespace maya::dsl;

static std::string get_row(const Canvas& canvas, int y) {
    std::string s;
    for (int x = 0; x < canvas.width(); ++x) {
        Cell c = canvas.get(x, y);
        if (c.character >= 0x20 && c.character < 0x7F)
            s += static_cast<char>(c.character);
        else
            s += '#'; // All non-ASCII mapped to '#' for simplicity
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

static void dump(const Canvas& canvas, int rows = -1) {
    if (rows < 0) rows = canvas.height();
    for (int y = 0; y < rows && y < canvas.height(); ++y)
        std::println("  {:2}|{}|", y, get_row(canvas, y));
}

// ============================================================================
// Single border (─ │ ┌ ┐ └ ┘ — U+2500 range)
// ============================================================================

void test_border_single_corners_are_box_drawing() {
    std::println("--- test_border_single_corners_are_box_drawing ---");
    StylePool pool;
    Canvas canvas(10, 5, &pool);
    render_tree(
        box().direction(Column).border(BorderStyle::Single)(text("hi")),
        canvas, pool, theme::dark);
    dump(canvas, 3);
    // All four corners should be box-drawing chars (U+2500+)
    assert(canvas.get(0, 0).character >= 0x2500); // top-left
    assert(canvas.get(9, 0).character >= 0x2500); // top-right
    assert(canvas.get(0, 4).character >= 0x2500); // bottom-left
    assert(canvas.get(9, 4).character >= 0x2500); // bottom-right
    std::println("  TL=U+{:04X}", (uint32_t)canvas.get(0, 0).character);
    std::println("PASS\n");
}

void test_border_single_content_inside() {
    std::println("--- test_border_single_content_inside ---");
    StylePool pool;
    Canvas canvas(10, 5, &pool);
    render_tree(
        box().direction(Column).border(BorderStyle::Single)(text("hi")),
        canvas, pool, theme::dark);
    dump(canvas, 3);
    // "hi" should appear at (1,1) — one cell inside the border
    assert(canvas.get(1, 1).character == U'h');
    assert(canvas.get(2, 1).character == U'i');
    std::println("PASS\n");
}

void test_border_single_top_left_corner() {
    std::println("--- test_border_single_top_left_corner ---");
    // Single border TL = ┌ (U+250C)
    StylePool pool;
    Canvas canvas(10, 5, &pool);
    render_tree(
        box().direction(Column).border(BorderStyle::Single)(text("x")),
        canvas, pool, theme::dark);
    assert(canvas.get(0, 0).character == U'\u250C'); // ┌
    std::println("  TL=U+{:04X}", (uint32_t)canvas.get(0, 0).character);
    std::println("PASS\n");
}

void test_border_single_horizontal_char() {
    std::println("--- test_border_single_horizontal_char ---");
    // Single horizontal border = ─ (U+2500)
    StylePool pool;
    Canvas canvas(10, 5, &pool);
    render_tree(
        box().direction(Column).border(BorderStyle::Single)(text("x")),
        canvas, pool, theme::dark);
    // Top edge interior (between corners)
    assert(canvas.get(1, 0).character == U'\u2500'); // ─
    std::println("PASS\n");
}

void test_border_single_vertical_char() {
    std::println("--- test_border_single_vertical_char ---");
    // Single vertical border = │ (U+2502)
    StylePool pool;
    Canvas canvas(10, 5, &pool);
    render_tree(
        box().direction(Column).border(BorderStyle::Single)(text("x")),
        canvas, pool, theme::dark);
    // Left edge interior (between corners)
    assert(canvas.get(0, 1).character == U'\u2502'); // │
    std::println("PASS\n");
}

// ============================================================================
// Round border (╭ ╮ ╰ ╯ — U+256D..U+2570)
// ============================================================================

void test_border_round_top_left() {
    std::println("--- test_border_round_top_left ---");
    // Round TL = ╭ (U+256D)
    StylePool pool;
    Canvas canvas(10, 5, &pool);
    render_tree(
        box().direction(Column).border(BorderStyle::Round)(text("x")),
        canvas, pool, theme::dark);
    assert(canvas.get(0, 0).character == U'\u256D'); // ╭
    std::println("  TL=U+{:04X}", (uint32_t)canvas.get(0, 0).character);
    std::println("PASS\n");
}

void test_border_round_top_right() {
    std::println("--- test_border_round_top_right ---");
    // Round TR = ╮ (U+256E)
    StylePool pool;
    Canvas canvas(10, 5, &pool);
    render_tree(
        box().direction(Column).border(BorderStyle::Round)(text("x")),
        canvas, pool, theme::dark);
    assert(canvas.get(9, 0).character == U'\u256E'); // ╮
    std::println("PASS\n");
}

void test_border_round_bottom_left() {
    std::println("--- test_border_round_bottom_left ---");
    // Round BL = ╰ (U+2570)
    StylePool pool;
    Canvas canvas(10, 5, &pool);
    render_tree(
        box().direction(Column).border(BorderStyle::Round)(text("x")),
        canvas, pool, theme::dark);
    assert(canvas.get(0, 4).character == U'\u2570'); // ╰
    std::println("PASS\n");
}

void test_border_round_bottom_right() {
    std::println("--- test_border_round_bottom_right ---");
    // Round BR = ╯ (U+256F)
    StylePool pool;
    Canvas canvas(10, 5, &pool);
    render_tree(
        box().direction(Column).border(BorderStyle::Round)(text("x")),
        canvas, pool, theme::dark);
    assert(canvas.get(9, 4).character == U'\u256F'); // ╯
    std::println("PASS\n");
}

// ============================================================================
// Double border (╔ ╗ ╚ ╝ ═ ║)
// ============================================================================

void test_border_double_top_left() {
    std::println("--- test_border_double_top_left ---");
    // Double TL = ╔ (U+2554)
    StylePool pool;
    Canvas canvas(10, 5, &pool);
    render_tree(
        box().direction(Column).border(BorderStyle::Double)(text("x")),
        canvas, pool, theme::dark);
    assert(canvas.get(0, 0).character == U'\u2554'); // ╔
    std::println("  TL=U+{:04X}", (uint32_t)canvas.get(0, 0).character);
    std::println("PASS\n");
}

void test_border_double_horizontal_char() {
    std::println("--- test_border_double_horizontal_char ---");
    // Double horizontal = ═ (U+2550)
    StylePool pool;
    Canvas canvas(10, 5, &pool);
    render_tree(
        box().direction(Column).border(BorderStyle::Double)(text("x")),
        canvas, pool, theme::dark);
    assert(canvas.get(1, 0).character == U'\u2550'); // ═
    std::println("PASS\n");
}

void test_border_double_vertical_char() {
    std::println("--- test_border_double_vertical_char ---");
    // Double vertical = ║ (U+2551)
    StylePool pool;
    Canvas canvas(10, 5, &pool);
    render_tree(
        box().direction(Column).border(BorderStyle::Double)(text("x")),
        canvas, pool, theme::dark);
    assert(canvas.get(0, 1).character == U'\u2551'); // ║
    std::println("PASS\n");
}

// ============================================================================
// Bold border (┏ ┓ ┗ ┛ ━ ┃)
// ============================================================================

void test_border_bold_top_left() {
    std::println("--- test_border_bold_top_left ---");
    // Bold TL = ┏ (U+250F)
    StylePool pool;
    Canvas canvas(10, 5, &pool);
    render_tree(
        box().direction(Column).border(BorderStyle::Bold)(text("x")),
        canvas, pool, theme::dark);
    assert(canvas.get(0, 0).character == U'\u250F'); // ┏
    std::println("  TL=U+{:04X}", (uint32_t)canvas.get(0, 0).character);
    std::println("PASS\n");
}

// ============================================================================
// Classic border (+ - |)
// ============================================================================

void test_border_classic_ascii_corners() {
    std::println("--- test_border_classic_ascii_corners ---");
    StylePool pool;
    Canvas canvas(10, 5, &pool);
    render_tree(
        box().direction(Column).border(BorderStyle::Classic)(text("x")),
        canvas, pool, theme::dark);
    dump(canvas, 3);
    // Classic corners are ASCII '+' (0x2B)
    assert(canvas.get(0, 0).character == U'+');
    assert(canvas.get(9, 0).character == U'+');
    assert(canvas.get(0, 4).character == U'+');
    assert(canvas.get(9, 4).character == U'+');
    std::println("PASS\n");
}

void test_border_classic_horizontal_dash() {
    std::println("--- test_border_classic_horizontal_dash ---");
    StylePool pool;
    Canvas canvas(10, 5, &pool);
    render_tree(
        box().direction(Column).border(BorderStyle::Classic)(text("x")),
        canvas, pool, theme::dark);
    // Classic horizontal = '-'
    assert(canvas.get(1, 0).character == U'-');
    std::println("PASS\n");
}

void test_border_classic_vertical_pipe() {
    std::println("--- test_border_classic_vertical_pipe ---");
    StylePool pool;
    Canvas canvas(10, 5, &pool);
    render_tree(
        box().direction(Column).border(BorderStyle::Classic)(text("x")),
        canvas, pool, theme::dark);
    // Classic vertical = '|'
    assert(canvas.get(0, 1).character == U'|');
    std::println("PASS\n");
}

// ============================================================================
// Border with color
// ============================================================================

void test_border_with_color() {
    std::println("--- test_border_with_color ---");
    StylePool pool;
    Canvas canvas(10, 5, &pool);
    render_tree(
        box().direction(Column).border(BorderStyle::Single, Color::red())(text("x")),
        canvas, pool, theme::dark);
    // Border should still be present regardless of color
    assert(canvas.get(0, 0).character >= 0x2500);
    // The border cell should have a non-default style (red color was applied)
    assert(canvas.get(0, 0).style_id != 0);
    std::println("PASS\n");
}

// ============================================================================
// Border text title
// ============================================================================

void test_border_title() {
    std::println("--- test_border_title ---");
    StylePool pool;
    Canvas canvas(20, 5, &pool);
    render_tree(
        box().direction(Column)
            .border(BorderStyle::Single)
            .border_text("Title")(text("content")),
        canvas, pool, theme::dark);
    dump(canvas, 2);
    // Title should appear in the top border row
    std::string top = get_row(canvas, 0);
    assert(top.find("Title") != std::string::npos);
    std::println("PASS\n");
}

// ============================================================================
// No border (default)
// ============================================================================

void test_no_border_no_box_chars() {
    std::println("--- test_no_border_no_box_chars ---");
    StylePool pool;
    Canvas canvas(10, 3, &pool);
    render_tree(
        box().direction(Column)(text("hello")),
        canvas, pool, theme::dark);
    // Without border, no box-drawing characters should appear in corners
    Cell tl = canvas.get(0, 0);
    assert(tl.character < 0x2500 || tl.character == U'h'); // first char is 'h' from text
    std::println("PASS\n");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_border_single_corners_are_box_drawing();
    test_border_single_content_inside();
    test_border_single_top_left_corner();
    test_border_single_horizontal_char();
    test_border_single_vertical_char();
    test_border_round_top_left();
    test_border_round_top_right();
    test_border_round_bottom_left();
    test_border_round_bottom_right();
    test_border_double_top_left();
    test_border_double_horizontal_char();
    test_border_double_vertical_char();
    test_border_bold_top_left();
    test_border_classic_ascii_corners();
    test_border_classic_horizontal_dash();
    test_border_classic_vertical_pipe();
    test_border_with_color();
    test_border_title();
    test_no_border_no_box_chars();
    std::println("=== ALL 19 TESTS PASSED ===");
}
