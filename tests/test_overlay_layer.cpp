// Tests for the absolute-positioned z-ordered overlay compositor.
#include <maya/maya.hpp>
#include <maya/app/overlay_layer.hpp>
#include <cassert>
#include <print>
#include <string>

using namespace maya;
using namespace maya::dsl;

static std::string row_text(const Canvas& c, int y) {
    std::string s;
    for (int x = 0; x < c.width(); ++x) {
        Cell cell = c.get(x, y);
        if (cell.character >= 0x20 && cell.character < 0x7F)
            s += static_cast<char>(cell.character);
        else
            s += ' ';
    }
    return s;
}

static int find_col(const Canvas& c, int y, const std::string& needle) {
    return static_cast<int>(row_text(c, y).find(needle));
}

// Flow layer present, no overlays ⇒ base untouched.
void test_base_only() {
    std::println("--- test_base_only ---");
    StylePool pool;
    Canvas canvas(40, 10, &pool);
    canvas.clear();
    render_tree(text("BASE"), canvas, pool, theme::dark, /*auto_height=*/true);

    OverlayStack stack;
    assert(stack.empty());
    stack.composite(canvas, pool);            // no-op
    assert(find_col(canvas, 0, "BASE") == 0);
    std::println("PASS\n");
}

// TopRight anchor lands the float at the right edge.
void test_anchor_top_right() {
    std::println("--- test_anchor_top_right ---");
    StylePool pool;
    Canvas canvas(40, 10, &pool);
    canvas.clear();
    render_tree(text("flow"), canvas, pool, theme::dark, true);

    OverlayStack stack;
    stack += (Element(text("HI")) |
              overlay_<overlay_cfg(Anchor::TopRight)>);
    stack.composite(canvas, pool);

    int col = find_col(canvas, 0, "HI");
    // "HI" is 2 wide, screen 40 ⇒ origin at column 38.
    assert(col == 38);
    std::println("PASS\n");
}

// Center anchor centers both axes.
void test_anchor_center() {
    std::println("--- test_anchor_center ---");
    StylePool pool;
    Canvas canvas(40, 11, &pool);
    canvas.clear();

    OverlayStack stack;
    stack += (Element(text("XX")) | overlay_<overlay_cfg(Anchor::Center)>);
    stack.composite(canvas, pool);

    // (40-2)/2 = 19 horizontally; height 1 ⇒ (11-1)/2 = 5 vertically.
    int col = find_col(canvas, 5, "XX");
    assert(col == 19);
    std::println("PASS\n");
}

// Runtime offset positions the float at an explicit cell.
void test_overlay_at() {
    std::println("--- test_overlay_at ---");
    StylePool pool;
    Canvas canvas(40, 10, &pool);
    canvas.clear();

    OverlayStack stack;
    stack += overlay_at(text("PIN"), /*x=*/12, /*y=*/3);
    stack.composite(canvas, pool);

    assert(find_col(canvas, 3, "PIN") == 12);
    std::println("PASS\n");
}

// z-index: higher z paints over lower z at the same cell.
void test_z_order() {
    std::println("--- test_z_order ---");
    StylePool pool;
    Canvas canvas(40, 10, &pool);
    canvas.clear();

    OverlayStack stack;
    // Both pinned at (5,2); LOW has z=0, HIGH has z=10 ⇒ HIGH wins.
    stack += (Element(text("LOWLOWLOW")) |
              overlay_<overlay_cfg(Anchor::TopLeft, /*z=*/0, /*dx=*/5, /*dy=*/2)>);
    stack += (Element(text("HIGH")) |
              overlay_<overlay_cfg(Anchor::TopLeft, /*z=*/10, /*dx=*/5, /*dy=*/2)>);
    stack.composite(canvas, pool);

    std::string r = row_text(canvas, 2);
    assert(r.substr(5, 4) == "HIGH");
    std::println("PASS\n");
}

// Insertion order breaks z ties (stable sort).
void test_z_tie_stable() {
    std::println("--- test_z_tie_stable ---");
    StylePool pool;
    Canvas canvas(40, 10, &pool);
    canvas.clear();

    OverlayStack stack;
    stack += (Element(text("AAAA")) |
              overlay_<overlay_cfg(Anchor::TopLeft, 0, 5, 2)>);
    stack += (Element(text("BBBB")) |
              overlay_<overlay_cfg(Anchor::TopLeft, 0, 5, 2)>);  // later ⇒ on top
    stack.composite(canvas, pool);

    assert(row_text(canvas, 2).substr(5, 4) == "BBBB");
    std::println("PASS\n");
}

// Clamp keeps an off-screen-nudged float fully visible.
void test_clamp_on_screen() {
    std::println("--- test_clamp_on_screen ---");
    StylePool pool;
    Canvas canvas(20, 6, &pool);
    canvas.clear();

    OverlayStack stack;
    // Nudge far past the right edge; clamp pulls it back so it fits.
    stack += (Element(text("EDGE")) |
              overlay_<overlay_cfg(Anchor::TopLeft, 0, /*dx=*/100, /*dy=*/0,
                                   /*max_w=*/0, /*clamp=*/true)>);
    stack.composite(canvas, pool);

    int col = find_col(canvas, 0, "EDGE");
    assert(col == 16);   // 20 - 4
    std::println("PASS\n");
}

// Overlay floats over flow content without disturbing it elsewhere.
void test_float_over_flow() {
    std::println("--- test_float_over_flow ---");
    StylePool pool;
    Canvas canvas(40, 6, &pool);
    canvas.clear();
    render_tree(box().direction(Column)(
                    text("line-one-content"),
                    text("line-two-content"),
                    text("line-three-xxxx")),
                canvas, pool, theme::dark, true);

    OverlayStack stack;
    stack += overlay_at(text("[TIP]"), /*x=*/20, /*y=*/1);
    stack.composite(canvas, pool);

    // Float lands on row 1 at col 20...
    assert(row_text(canvas, 1).substr(20, 5) == "[TIP]");
    // ...while rows 0 and 2 remain pure flow content.
    assert(find_col(canvas, 0, "line-one-content") == 0);
    assert(find_col(canvas, 2, "line-three-xxxx") == 0);
    std::println("PASS\n");
}

int main() {
    test_base_only();
    test_anchor_top_right();
    test_anchor_center();
    test_overlay_at();
    test_z_order();
    test_z_tie_stable();
    test_clamp_on_screen();
    test_float_over_flow();
    std::println("All overlay tests passed.");
    return 0;
}
