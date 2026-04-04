// Tests for element layout, render_tree, and the builder DSL
#include <maya/maya.hpp>
#include <cassert>
#include <print>

using namespace maya;
using namespace maya::detail;
using namespace maya::dsl;

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

// ============================================================================
// Dashboard-like layout: vstack(header, hstack(left_col, right_col), procs, footer)
// ============================================================================

void test_dashboard_layout() {
    std::println("--- test_dashboard_layout ---");
    StylePool pool;
    Canvas canvas(80, 40, &pool);

    // Simulate dashboard structure:
    // - header (1 row)
    // - grid: hstack containing two vstacks with bordered panels
    // - procs: bordered vstack with several rows
    // - footer (1 row)

    // Use realistic content widths — sparklines are wide strings
    std::string spark40(40, 'X');  // 40-char sparkline
    std::string spark30(30, 'Y');  // 30-char sparkline
    std::string spark20(20, 'Z');  // 20-char sparkline
    std::string gauge20(20, '=');  // 20-char gauge

    auto cpu_panel = box().direction(Column).border(BorderStyle::Round).padding(0,1,0,1)(
        // hstack: "avg " + 40-char sparkline
        box().direction(Row)(text("avg "), text(spark40)),
        text(" 50.0%  max 92%  min 12%"),
        text(""),
        box().direction(Row)(text("c0 "), text(spark30), text(" 79%")),
        box().direction(Row)(text("c1 "), text(spark30), text(" 21%")),
        box().direction(Row)(text("c2 "), text(spark30), text(" 77%")),
        box().direction(Row)(text("c3 "), text(spark30), text(" 53%")),
        box().direction(Row)(text("c4 "), text(spark30), text(" 52%")),
        box().direction(Row)(text("c5 "), text(spark30), text(" 14%")),
        box().direction(Row)(text("c6 "), text(spark30), text(" 92%")),
        box().direction(Row)(text("c7 "), text(spark30), text(" 12%"))
    );
    auto net_panel = box().direction(Column).border(BorderStyle::Round).padding(0,1,0,1)(
        text("eth0  up 6.6 MB/s  dn 51.9 MB/s"),
        box().direction(Row)(text(" rx "), text(spark30)),
        box().direction(Row)(text(" tx "), text(spark30)),
        text("tcp 842  udp 156  err 0  drop 0"),
        text("rx 2.4G  tx 0.6G")
    );
    auto mem_panel = box().direction(Column).border(BorderStyle::Round).padding(0,1,0,1)(
        text("92% used"),
        text(gauge20),
        text(""),
        text("used   9.0G"),
        text("buff   1.8G"),
        text("cache  3.9G"),
        text("free   1.3G"),
        text("total 16.0G"),
        text(""),
        text(spark20)
    );
    auto disk_panel = box().direction(Column).border(BorderStyle::Round).padding(0,1,0,1)(
        text("sda  R  87 MB/s  W  13 MB/s"),
        box().direction(Row)(text("read "), text(spark20)),
        box().direction(Row)(text("write"), text(spark20)),
        text("iops 18.3k r/s  12.1k w/s"),
        text("lat r 1.8ms  w 3.3ms"),
        box().direction(Row)(text("util "), text(std::string(14, '=')), text(" 43%"))
    );

    auto left_col  = box().direction(Column).grow(2)(std::move(cpu_panel), std::move(net_panel));
    auto right_col = box().direction(Column).grow(1)(std::move(mem_panel), std::move(disk_panel));
    auto grid      = box().direction(Row)(std::move(left_col), std::move(right_col));

    auto procs = box().direction(Column).border(BorderStyle::Round)(
        text("PID  USER  NAME"),
        text("1284 www   node"),
        text("892  pg    postgres"),
        text("2041 www   nginx")
    );

    auto tree = box().direction(Column)(
        text("HEADER"),
        std::move(grid),
        std::move(procs),
        text("FOOTER")
    );

    // Build layout tree and compute layout manually to inspect sizes
    std::vector<layout::LayoutNode> dbg_nodes;
    dbg_nodes.reserve(128);
    render_detail::build_layout_tree(tree, dbg_nodes, theme::dark);
    dbg_nodes[0].style.width = Dimension::fixed(80);
    dbg_nodes[0].style.height = Dimension::fixed(40);
    layout::compute(dbg_nodes, 0, 80, 40);

    // Print root children sizes
    std::println("  Root children:");
    for (auto ci : dbg_nodes[0].children) {
        auto& n = dbg_nodes[ci];
        std::println("    node[{}]: pos=({},{}) size={}x{}",
            ci, n.computed.pos.x.value, n.computed.pos.y.value,
            n.computed.size.width.value, n.computed.size.height.value);
        // Print grandchildren
        for (auto gi : n.children) {
            auto& g = dbg_nodes[gi];
            std::println("      node[{}]: pos=({},{}) size={}x{}",
                gi, g.computed.pos.x.value, g.computed.pos.y.value,
                g.computed.size.width.value, g.computed.size.height.value);
        }
    }

    render_tree(std::move(tree), canvas, pool, theme::dark);
    dump(canvas, 30);

    // Header should be at row 0
    assert(get_row(canvas, 0).find("HEADER") != std::string::npos);

    // CPU panel border should start at row 1 (right after header)
    std::string row1 = get_row(canvas, 1);
    std::println("  row1: '{}'", row1);
    assert(row1.find('#') != std::string::npos); // border chars

    // Check there's no huge empty gap - row 1 should have content, not be empty
    assert(!row1.empty());

    // Footer should be right after procs, not at the very bottom
    // Find footer row
    int footer_row = -1;
    for (int y = 0; y < 40; ++y) {
        if (get_row(canvas, y).find("FOOTER") != std::string::npos) {
            footer_row = y;
            break;
        }
    }
    std::println("  footer at row: {}", footer_row);
    assert(footer_row > 0 && footer_row < 30); // should be tightly packed

    std::println("PASS\n");
}

// ============================================================================
// Regression: column child's height should recompute when width changes
// ============================================================================

void test_column_child_height_recomputes() {
    std::println("--- test_column_child_height_recomputes ---");
    StylePool pool;
    Canvas canvas(40, 20, &pool);

    // An hstack with two children whose combined natural width exceeds
    // the container. The hstack shrinks them. Content that fit at natural
    // width may wrap at the shrunk width, increasing height. The outer
    // column must accommodate the taller recomputed height.

    // Left: 30-char text x3 = 3 rows at 30+ cols, 6 rows at 15 cols
    // Right: 30-char text x2 = 2 rows at 30+ cols
    // In 40 cols, total hypo = 60, shrink to 40. left→20, right→20.
    // At 20 cols, 30-char text wraps to 2 rows. Left = 6 rows, right = 4.
    std::string wide_text(30, 'A');
    auto left = box().direction(Column)(
        text(wide_text),
        text(wide_text),
        text(wide_text)
    );
    auto right = box().direction(Column)(
        text(std::string(30, 'B')),
        text(std::string(30, 'B'))
    );
    auto row_box = box().direction(Row)(std::move(left), std::move(right));

    auto tree = box().direction(Column)(
        text("TOP"),
        std::move(row_box),
        text("BOT")
    );

    render_tree(std::move(tree), canvas, pool, theme::dark);
    dump(canvas, 10);

    // TOP should be at row 0
    assert(get_row(canvas, 0).find("TOP") != std::string::npos);

    // At 30 cols each (natural width), left=3 rows, right=2, hstack cross=3.
    // After shrink to 20 cols each, text wraps: left=6 rows, right=4,
    // hstack cross should be 6. BOT at row 7.
    int bot_row = -1;
    for (int y = 0; y < 20; ++y) {
        if (get_row(canvas, y).find("BOT") != std::string::npos) {
            bot_row = y;
            break;
        }
    }
    std::println("  BOT at row: {}", bot_row);
    // Without the fix, BOT at row 4 (hstack height=3 from full-width measurement).
    // With the fix, BOT at row 7 (hstack height=6 after text wraps at shrunk width).
    assert(bot_row >= 5);

    std::println("PASS\n");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_column_child_height_recomputes();
    test_dashboard_layout();
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
    std::println("=== ALL 25 TESTS PASSED ===");
}
