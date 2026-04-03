// Tests for Canvas, StylePool, and Cell
#include <maya/maya.hpp>
#include <cassert>
#include <print>

using namespace maya;

// ============================================================================
// Canvas tests
// ============================================================================

void test_canvas_dimensions() {
    std::println("--- test_canvas_dimensions ---");
    StylePool pool;
    Canvas canvas(20, 10, &pool);
    assert(canvas.width()  == 20);
    assert(canvas.height() == 10);
    std::println("PASS\n");
}

void test_canvas_set_get_char() {
    std::println("--- test_canvas_set_get_char ---");
    StylePool pool;
    Canvas canvas(10, 5, &pool);
    canvas.set(3, 2, U'X', 0);
    Cell c = canvas.get(3, 2);
    assert(c.character == U'X');
    std::println("PASS\n");
}

void test_canvas_set_get_different_chars() {
    std::println("--- test_canvas_set_get_different_chars ---");
    StylePool pool;
    Canvas canvas(10, 5, &pool);
    canvas.set(0, 0, U'A', 0);
    canvas.set(1, 0, U'B', 0);
    canvas.set(0, 1, U'C', 0);
    assert(canvas.get(0, 0).character == U'A');
    assert(canvas.get(1, 0).character == U'B');
    assert(canvas.get(0, 1).character == U'C');
    std::println("PASS\n");
}

void test_canvas_set_get_style() {
    std::println("--- test_canvas_set_get_style ---");
    StylePool pool;
    uint16_t sid = pool.intern(Style{}.with_bold());
    Canvas canvas(10, 5, &pool);
    canvas.set(2, 2, U'Z', sid);
    Cell c = canvas.get(2, 2);
    assert(c.character == U'Z');
    assert(c.style_id == sid);
    std::println("PASS\n");
}

void test_canvas_set_oob_no_crash() {
    std::println("--- test_canvas_set_oob_no_crash ---");
    StylePool pool;
    Canvas canvas(5, 5, &pool);
    // All of these should be silent no-ops
    canvas.set(-1, 0,  U'X', 0);
    canvas.set(5,  0,  U'X', 0);
    canvas.set(0,  -1, U'X', 0);
    canvas.set(0,  5,  U'X', 0);
    canvas.set(-1, -1, U'X', 0);
    // Origin should still be default
    Cell c = canvas.get(0, 0);
    assert(c.character != U'X');
    std::println("PASS\n");
}

void test_canvas_clear_zeroes_cells() {
    std::println("--- test_canvas_clear_zeroes_cells ---");
    StylePool pool;
    Canvas canvas(5, 3, &pool);
    canvas.set(0, 0, U'Q', 0);
    canvas.clear();
    Cell c = canvas.get(0, 0);
    assert(c.character == 0 || c.character == U' ');
    std::println("PASS\n");
}

void test_canvas_write_text_chars() {
    std::println("--- test_canvas_write_text_chars ---");
    StylePool pool;
    Canvas canvas(20, 5, &pool);
    canvas.write_text(2, 1, "hello", 0);
    assert(canvas.get(2, 1).character == U'h');
    assert(canvas.get(3, 1).character == U'e');
    assert(canvas.get(4, 1).character == U'l');
    assert(canvas.get(5, 1).character == U'l');
    assert(canvas.get(6, 1).character == U'o');
    std::println("PASS\n");
}

void test_canvas_write_text_style() {
    std::println("--- test_canvas_write_text_style ---");
    StylePool pool;
    uint16_t sid = pool.intern(Style{}.with_bold());
    Canvas canvas(20, 5, &pool);
    canvas.write_text(0, 0, "hi", sid);
    assert(canvas.get(0, 0).style_id == sid);
    assert(canvas.get(1, 0).style_id == sid);
    std::println("PASS\n");
}

void test_canvas_fill_rect() {
    std::println("--- test_canvas_fill_rect ---");
    StylePool pool;
    Canvas canvas(10, 6, &pool);
    Rect r{{Columns{1}, Rows{1}}, {Columns{3}, Rows{2}}};
    canvas.fill(r, U'*', 0);

    // Inside rect
    assert(canvas.get(1, 1).character == U'*');
    assert(canvas.get(2, 1).character == U'*');
    assert(canvas.get(3, 1).character == U'*');
    assert(canvas.get(1, 2).character == U'*');
    assert(canvas.get(2, 2).character == U'*');
    assert(canvas.get(3, 2).character == U'*');

    // Outside rect — untouched
    assert(canvas.get(0, 0).character != U'*');
    assert(canvas.get(4, 1).character != U'*');
    assert(canvas.get(1, 3).character != U'*');
    std::println("PASS\n");
}

void test_canvas_push_pop_clip() {
    std::println("--- test_canvas_push_pop_clip ---");
    StylePool pool;
    Canvas canvas(10, 10, &pool);
    Rect clip{{Columns{2}, Rows{2}}, {Columns{3}, Rows{3}}};
    canvas.push_clip(clip);

    canvas.set(0, 0, U'X', 0); // outside clip → no-op
    canvas.set(3, 3, U'Y', 0); // inside clip → written

    canvas.pop_clip();

    assert(canvas.get(0, 0).character != U'X');
    assert(canvas.get(3, 3).character == U'Y');
    std::println("PASS\n");
}

void test_canvas_clip_restored_after_pop() {
    std::println("--- test_canvas_clip_restored_after_pop ---");
    StylePool pool;
    Canvas canvas(10, 10, &pool);
    {
        Rect clip{{Columns{5}, Rows{5}}, {Columns{2}, Rows{2}}};
        canvas.push_clip(clip);
        canvas.set(0, 0, U'A', 0); // outside clip, blocked
        canvas.pop_clip();
    }
    // After pop, clip is gone — (0,0) should be writable
    canvas.set(0, 0, U'B', 0);
    assert(canvas.get(0, 0).character == U'B');
    std::println("PASS\n");
}

void test_canvas_damage_after_clear() {
    std::println("--- test_canvas_damage_after_clear ---");
    StylePool pool;
    Canvas canvas(10, 5, &pool);
    canvas.clear();
    Rect dmg = canvas.damage();
    assert(!dmg.size.is_zero());
    assert(dmg.size.width.value  > 0);
    assert(dmg.size.height.value > 0);
    std::println("  damage: {}x{}", dmg.size.width.value, dmg.size.height.value);
    std::println("PASS\n");
}

void test_canvas_reset_damage() {
    std::println("--- test_canvas_reset_damage ---");
    StylePool pool;
    Canvas canvas(10, 5, &pool);
    canvas.clear();
    canvas.reset_damage();
    Rect dmg = canvas.damage();
    assert(dmg.size.is_zero());
    std::println("PASS\n");
}

void test_canvas_mark_all_damaged() {
    std::println("--- test_canvas_mark_all_damaged ---");
    StylePool pool;
    Canvas canvas(10, 5, &pool);
    canvas.reset_damage();
    canvas.mark_all_damaged();
    Rect dmg = canvas.damage();
    assert(dmg.size.width.value  == 10);
    assert(dmg.size.height.value == 5);
    std::println("PASS\n");
}

void test_canvas_get_packed_roundtrip() {
    std::println("--- test_canvas_get_packed_roundtrip ---");
    StylePool pool;
    Canvas canvas(5, 5, &pool);
    canvas.set(1, 1, U'Z', 0);
    uint64_t packed = canvas.get_packed(1, 1);
    Cell cell = Cell::unpack(packed);
    assert(cell.character == U'Z');
    std::println("PASS\n");
}

void test_canvas_resize() {
    std::println("--- test_canvas_resize ---");
    StylePool pool;
    Canvas canvas(5, 5, &pool);
    canvas.resize(20, 10);
    assert(canvas.width()  == 20);
    assert(canvas.height() == 10);
    std::println("PASS\n");
}

void test_canvas_cells_span_size() {
    std::println("--- test_canvas_cells_span_size ---");
    StylePool pool;
    Canvas canvas(8, 4, &pool);
    assert(canvas.cell_count() == 8 * 4);
    std::println("PASS\n");
}

// ============================================================================
// StylePool tests
// ============================================================================

void test_style_pool_intern_dedup() {
    std::println("--- test_style_pool_intern_dedup ---");
    StylePool pool;
    Style s = Style{}.with_bold();
    uint16_t id1 = pool.intern(s);
    uint16_t id2 = pool.intern(s);
    assert(id1 == id2);
    std::println("PASS\n");
}

void test_style_pool_different_styles_different_ids() {
    std::println("--- test_style_pool_different_styles_different_ids ---");
    StylePool pool;
    uint16_t id1 = pool.intern(Style{}.with_bold());
    uint16_t id2 = pool.intern(Style{}.with_italic());
    assert(id1 != id2);
    std::println("PASS\n");
}

void test_style_pool_get_roundtrip() {
    std::println("--- test_style_pool_get_roundtrip ---");
    StylePool pool;
    Style s = Style{}.with_bold().with_fg(Color::red());
    uint16_t id = pool.intern(s);
    Style got = pool.get(id);
    assert(got.bold == true);
    assert(got.fg.has_value());
    assert(got.fg->fg_sgr() == "31");
    std::println("PASS\n");
}

void test_style_pool_default_id_zero_is_empty_style() {
    std::println("--- test_style_pool_default_id_zero_is_empty_style ---");
    StylePool pool;
    // Style id 0 should be the default (empty) style
    uint16_t id = pool.intern(Style{});
    assert(id == 0);
    std::println("PASS\n");
}

// ============================================================================
// Cell tests
// ============================================================================

void test_cell_pack_unpack_char() {
    std::println("--- test_cell_pack_unpack_char ---");
    Cell c{U'\u2603', 0, 0, 0}; // snowman ☃
    uint64_t packed = c.pack();
    Cell got = Cell::unpack(packed);
    assert(got.character == U'\u2603');
    std::println("PASS\n");
}

void test_cell_pack_unpack_style_id() {
    std::println("--- test_cell_pack_unpack_style_id ---");
    Cell c{U'A', 42, 0, 0};
    Cell got = Cell::unpack(c.pack());
    assert(got.character == U'A');
    assert(got.style_id == 42);
    std::println("PASS\n");
}

void test_cell_default_character() {
    std::println("--- test_cell_default_character ---");
    StylePool pool;
    Canvas canvas(5, 5, &pool);
    canvas.clear();
    Cell c = canvas.get(2, 2);
    assert(c.character == 0 || c.character == U' ');
    std::println("PASS\n");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_canvas_dimensions();
    test_canvas_set_get_char();
    test_canvas_set_get_different_chars();
    test_canvas_set_get_style();
    test_canvas_set_oob_no_crash();
    test_canvas_clear_zeroes_cells();
    test_canvas_write_text_chars();
    test_canvas_write_text_style();
    test_canvas_fill_rect();
    test_canvas_push_pop_clip();
    test_canvas_clip_restored_after_pop();
    test_canvas_damage_after_clear();
    test_canvas_reset_damage();
    test_canvas_mark_all_damaged();
    test_canvas_get_packed_roundtrip();
    test_canvas_resize();
    test_canvas_cells_span_size();
    test_style_pool_intern_dedup();
    test_style_pool_different_styles_different_ids();
    test_style_pool_get_roundtrip();
    test_style_pool_default_id_zero_is_empty_style();
    test_cell_pack_unpack_char();
    test_cell_pack_unpack_style_id();
    test_cell_default_character();
    std::println("=== ALL 24 TESTS PASSED ===");
}
