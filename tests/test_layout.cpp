// Tests for element layout, render_tree, and the builder DSL
#include <maya/maya.hpp>
#include <cassert>
#include <print>

using namespace maya;

// Extract ASCII/printable row from canvas (box-drawing chars → '#')
static std::string get_row(const Canvas& canvas, int y) {
    std::string s;
    for (int x = 0; x < canvas.width(); ++x) {
        Cell c = canvas.get(x, y);
        if (c.character >= 0x20 && c.character < 0x7F)
            s += static_cast<char>(c.character);
        else if (c.character >= 0x2500)
            s += '#';
        else
            s += ' ';
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
// Text element
// ============================================================================

void test_bare_text_renders_at_origin() {
    std::println("--- test_bare_text_renders_at_origin ---");
    StylePool pool;
    Canvas canvas(30, 3, &pool);
    render_tree(text("hello"), canvas, pool, theme::dark);
    dump(canvas, 1);
    assert(get_row(canvas, 0).starts_with("hello"));
    std::println("PASS\n");
}

void test_text_with_style_renders() {
    std::println("--- test_text_with_style_renders ---");
    StylePool pool;
    Canvas canvas(20, 3, &pool);
    render_tree(text("styled", bold_style), canvas, pool, theme::dark);
    // Check the character is present and its style_id is non-zero (bold interned)
    assert(canvas.get(0, 0).character == U's');
    assert(canvas.get(0, 0).style_id != 0); // non-default style
    std::println("PASS\n");
}

void test_text_truncate_end() {
    std::println("--- test_text_truncate_end ---");
    StylePool pool;
    Canvas canvas(5, 3, &pool);
    render_tree(
        text("hello world", Style{}, TextWrap::TruncateEnd),
        canvas, pool, theme::dark);
    dump(canvas, 1);
    std::string r = get_row(canvas, 0);
    // Content must fit within 5 columns
    assert(r.size() <= 5);
    std::println("  truncated: '{}'", r);
    std::println("PASS\n");
}

void test_text_no_wrap_stays_on_one_row() {
    std::println("--- test_text_no_wrap_stays_on_one_row ---");
    StylePool pool;
    Canvas canvas(5, 5, &pool);
    render_tree(
        text("abcdefghij", Style{}, TextWrap::NoWrap),
        canvas, pool, theme::dark);
    // Row 1+ should be empty
    assert(get_row(canvas, 1).empty());
    std::println("PASS\n");
}

// ============================================================================
// Column layout
// ============================================================================

void test_column_stacks_children_vertically() {
    std::println("--- test_column_stacks_children_vertically ---");
    StylePool pool;
    Canvas canvas(30, 5, &pool);
    render_tree(
        box().direction(Column)(
            text("AAA"),
            text("BBB"),
            text("CCC")
        ), canvas, pool, theme::dark);
    dump(canvas, 4);
    assert(get_row(canvas, 0).find("AAA") != std::string::npos);
    assert(get_row(canvas, 1).find("BBB") != std::string::npos);
    assert(get_row(canvas, 2).find("CCC") != std::string::npos);
    std::println("PASS\n");
}

void test_column_with_padding() {
    std::println("--- test_column_with_padding ---");
    StylePool pool;
    Canvas canvas(30, 8, &pool);
    render_tree(
        box().direction(Column).padding(1)(
            text("AAA"),
            text("BBB")
        ), canvas, pool, theme::dark);
    dump(canvas, 5);
    assert(get_row(canvas, 0).empty()); // top padding
    assert(get_row(canvas, 1).find("AAA") != std::string::npos);
    assert(get_row(canvas, 2).find("BBB") != std::string::npos);
    std::println("PASS\n");
}

void test_column_gap() {
    std::println("--- test_column_gap ---");
    StylePool pool;
    Canvas canvas(20, 8, &pool);
    render_tree(
        box().direction(Column).gap(2)(
            text("A"),
            text("B")
        ), canvas, pool, theme::dark);
    dump(canvas, 6);
    auto arow = -1, brow = -1;
    for (int y = 0; y < canvas.height(); ++y) {
        if (get_row(canvas, y).find("A") != std::string::npos) arow = y;
        if (get_row(canvas, y).find("B") != std::string::npos) brow = y;
    }
    assert(arow >= 0 && brow >= 0);
    // B should be at least gap=2 rows after A
    assert(brow - arow >= 3); // 1 for "A" row + 2 gap rows
    std::println("  A@row{} B@row{}", arow, brow);
    std::println("PASS\n");
}

// ============================================================================
// Row layout
// ============================================================================

void test_row_places_children_horizontally() {
    std::println("--- test_row_places_children_horizontally ---");
    StylePool pool;
    Canvas canvas(30, 3, &pool);
    render_tree(
        box().direction(Row)(
            text("LEFT"),
            text("RIGHT")
        ), canvas, pool, theme::dark);
    dump(canvas, 1);
    std::string r = get_row(canvas, 0);
    auto lpos = r.find("LEFT");
    auto rpos = r.find("RIGHT");
    assert(lpos != std::string::npos);
    assert(rpos != std::string::npos);
    assert(rpos > lpos);
    std::println("  LEFT@{} RIGHT@{}", lpos, rpos);
    std::println("PASS\n");
}

void test_row_gap() {
    std::println("--- test_row_gap ---");
    StylePool pool;
    Canvas canvas(30, 3, &pool);
    render_tree(
        box().direction(Row).gap(3)(
            text("A"),
            text("B")
        ), canvas, pool, theme::dark);
    dump(canvas, 1);
    std::string r = get_row(canvas, 0);
    auto apos = r.find("A");
    auto bpos = r.find("B");
    assert(apos != std::string::npos && bpos != std::string::npos);
    // B must be at least gap+1 chars after A
    assert(bpos >= apos + 4);
    std::println("PASS\n");
}

void test_row_with_padding() {
    std::println("--- test_row_with_padding ---");
    StylePool pool;
    Canvas canvas(30, 5, &pool);
    render_tree(
        box().direction(Row).padding(1)(
            text("X")
        ), canvas, pool, theme::dark);
    dump(canvas, 3);
    assert(get_row(canvas, 0).empty()); // top padding
    std::string r = get_row(canvas, 1);
    assert(r.find("X") != std::string::npos);
    // X should be at column >= 1 (left padding)
    assert(r.find("X") >= 1);
    std::println("PASS\n");
}

// ============================================================================
// Flex properties
// ============================================================================

void test_spacer_pushes_content() {
    std::println("--- test_spacer_pushes_content ---");
    StylePool pool;
    Canvas canvas(20, 10, &pool);
    render_tree(
        box().direction(Column)(
            text("TOP"),
            spacer(),
            text("BOT")
        ), canvas, pool, theme::dark);
    dump(canvas);
    assert(get_row(canvas, 0).find("TOP") != std::string::npos);
    assert(get_row(canvas, 9).find("BOT") != std::string::npos);
    std::println("PASS\n");
}

void test_grow_fills_remaining_space() {
    std::println("--- test_grow_fills_remaining_space ---");
    StylePool pool;
    Canvas canvas(30, 3, &pool);
    render_tree(
        box().direction(Row)(
            text("A"),
            box().grow(1.0f), // fills remaining width
            text("B")
        ), canvas, pool, theme::dark);
    dump(canvas, 1);
    std::string r = get_row(canvas, 0);
    // B should be far to the right (pushed by grow box)
    auto bpos = r.find("B");
    assert(bpos != std::string::npos);
    assert(bpos > 5);
    std::println("  B@col{}", bpos);
    std::println("PASS\n");
}

void test_align_items_center() {
    std::println("--- test_align_items_center ---");
    StylePool pool;
    Canvas canvas(20, 10, &pool);
    render_tree(
        box().direction(Column).align_items(Align::Center)(
            text("X")
        ), canvas, pool, theme::dark);
    dump(canvas, 2);
    std::string r = get_row(canvas, 0);
    auto xpos = r.find("X");
    assert(xpos != std::string::npos);
    // Center-aligned in 20 cols: X should be around column 9-10
    assert(xpos > 0); // not at left edge
    std::println("  X@col{}", xpos);
    std::println("PASS\n");
}

void test_justify_space_between() {
    std::println("--- test_justify_space_between ---");
    StylePool pool;
    Canvas canvas(20, 3, &pool);
    render_tree(
        box().direction(Row).justify(Justify::SpaceBetween)(
            text("L"),
            text("R")
        ), canvas, pool, theme::dark);
    dump(canvas, 1);
    std::string r = get_row(canvas, 0);
    auto lpos = r.find("L");
    auto rpos = r.find("R");
    assert(lpos != std::string::npos && rpos != std::string::npos);
    // SpaceBetween: L at 0, R near end
    assert(lpos == 0);
    assert(rpos > 15); // R pushed to right end of 20-col canvas
    std::println("  L@{} R@{}", lpos, rpos);
    std::println("PASS\n");
}

void test_justify_center() {
    std::println("--- test_justify_center ---");
    StylePool pool;
    Canvas canvas(20, 3, &pool);
    render_tree(
        box().direction(Row).justify(Justify::Center)(
            text("MID")
        ), canvas, pool, theme::dark);
    dump(canvas, 1);
    std::string r = get_row(canvas, 0);
    auto pos = r.find("MID");
    assert(pos != std::string::npos);
    // Should be around the middle of 20 columns
    assert(pos >= 7 && pos <= 10);
    std::println("  MID@col{}", pos);
    std::println("PASS\n");
}

// ============================================================================
// Fixed size constraints
// ============================================================================

void test_fixed_width_box() {
    std::println("--- test_fixed_width_box ---");
    StylePool pool;
    Canvas canvas(40, 3, &pool);
    render_tree(
        box().direction(Row)(
            box().width(Dimension::fixed(10)).direction(Column)(text("A")),
            text("B")
        ), canvas, pool, theme::dark);
    dump(canvas, 1);
    std::string r = get_row(canvas, 0);
    auto apos = r.find("A");
    auto bpos = r.find("B");
    assert(apos != std::string::npos && bpos != std::string::npos);
    // B must start at or after column 10
    assert(bpos >= 10);
    std::println("  A@{} B@{}", apos, bpos);
    std::println("PASS\n");
}

void test_fixed_height_box() {
    std::println("--- test_fixed_height_box ---");
    StylePool pool;
    Canvas canvas(20, 10, &pool);
    render_tree(
        box().direction(Column)(
            box().height(Dimension::fixed(3)).direction(Column)(text("TOP")),
            text("AFTER")
        ), canvas, pool, theme::dark);
    dump(canvas, 6);
    assert(get_row(canvas, 0).find("TOP")   != std::string::npos);
    assert(get_row(canvas, 3).find("AFTER") != std::string::npos);
    std::println("PASS\n");
}

// ============================================================================
// Nested layouts
// ============================================================================

void test_nested_column_inside_row() {
    std::println("--- test_nested_column_inside_row ---");
    StylePool pool;
    Canvas canvas(40, 5, &pool);
    render_tree(
        box().direction(Row)(
            box().direction(Column)(
                text("A1"),
                text("A2")
            ),
            box().direction(Column)(
                text("B1"),
                text("B2")
            )
        ), canvas, pool, theme::dark);
    dump(canvas, 4);
    assert(get_row(canvas, 0).find("A1") != std::string::npos);
    assert(get_row(canvas, 1).find("A2") != std::string::npos);
    assert(get_row(canvas, 0).find("B1") != std::string::npos);
    assert(get_row(canvas, 1).find("B2") != std::string::npos);
    std::println("PASS\n");
}

void test_deeply_nested() {
    std::println("--- test_deeply_nested ---");
    StylePool pool;
    Canvas canvas(40, 10, &pool);
    render_tree(
        box().direction(Column).padding(1)(
            text("title"),
            box().direction(Row).gap(1)(
                text("A"),
                text("B")
            )
        ), canvas, pool, theme::dark);
    dump(canvas, 5);
    assert(get_row(canvas, 1).find("title") != std::string::npos);
    std::string r2 = get_row(canvas, 2);
    assert(r2.find("A") != std::string::npos);
    assert(r2.find("B") != std::string::npos);
    std::println("PASS\n");
}

// ============================================================================
// Margin
// ============================================================================

void test_margin_offsets_child() {
    std::println("--- test_margin_offsets_child ---");
    StylePool pool;
    Canvas canvas(20, 8, &pool);
    render_tree(
        box().direction(Column)(
            box().direction(Column).margin(2)(
                text("M")
            )
        ), canvas, pool, theme::dark);
    dump(canvas, 5);
    // With margin=2 on all sides, content starts at row 2
    assert(get_row(canvas, 0).find("M") == std::string::npos);
    assert(get_row(canvas, 1).find("M") == std::string::npos);
    assert(get_row(canvas, 2).find("M") != std::string::npos);
    std::println("PASS\n");
}

// ============================================================================
// Separators
// ============================================================================

void test_separator_draws_horizontal_line() {
    std::println("--- test_separator_draws_horizontal_line ---");
    StylePool pool;
    Canvas canvas(20, 5, &pool);
    render_tree(
        box().direction(Column)(
            text("above"),
            separator(),
            text("below")
        ), canvas, pool, theme::dark);
    dump(canvas, 4);
    // Middle row should contain box-drawing characters (#)
    std::string sep_row = get_row(canvas, 1);
    assert(sep_row.find('#') != std::string::npos);
    std::println("PASS\n");
}

// ============================================================================
// Counter-style tree (real-world pattern)
// ============================================================================

void test_counter_tree() {
    std::println("--- test_counter_tree ---");
    StylePool pool;
    Canvas canvas(40, 10, &pool);
    render_tree(
        box().direction(Column).padding(1)(
            text("Counter: 0", bold_style),
            text("[+/-] change  [q] quit", dim_style)
        ), canvas, pool, theme::dark);
    dump(canvas, 5);
    assert(get_row(canvas, 1).find("Counter: 0")  != std::string::npos);
    assert(get_row(canvas, 2).find("[+/-]")        != std::string::npos);
    std::println("PASS\n");
}

// ============================================================================
// String children (auto-wrapped as text elements)
// ============================================================================

void test_string_child_auto_text() {
    std::println("--- test_string_child_auto_text ---");
    StylePool pool;
    Canvas canvas(20, 3, &pool);
    // Passing a string literal directly — should auto-wrap as text()
    render_tree(
        box().direction(Column)("auto wrapped"),
        canvas, pool, theme::dark);
    dump(canvas, 1);
    assert(get_row(canvas, 0).find("auto wrapped") != std::string::npos);
    std::println("PASS\n");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_bare_text_renders_at_origin();
    test_text_with_style_renders();
    test_text_truncate_end();
    test_text_no_wrap_stays_on_one_row();
    test_column_stacks_children_vertically();
    test_column_with_padding();
    test_column_gap();
    test_row_places_children_horizontally();
    test_row_gap();
    test_row_with_padding();
    test_spacer_pushes_content();
    test_grow_fills_remaining_space();
    test_align_items_center();
    test_justify_space_between();
    test_justify_center();
    test_fixed_width_box();
    test_fixed_height_box();
    test_nested_column_inside_row();
    test_deeply_nested();
    test_margin_offsets_child();
    test_separator_draws_horizontal_line();
    test_counter_tree();
    test_string_child_auto_text();
    std::println("=== ALL 23 TESTS PASSED ===");
}
