// Tests for the diff algorithm: direct ANSI output, zero intermediate allocations
#include <maya/maya.hpp>
#include <cassert>
#include <print>

using namespace maya;
using namespace maya::dsl;

void test_diff_identical_canvases_empty_output() {
    std::println("--- test_diff_identical_canvases_empty_output ---");
    StylePool pool;
    Canvas old_c(20, 3, &pool);
    Canvas new_c(20, 3, &pool);
    render_tree(text("same content"), old_c, pool, theme::dark);
    render_tree(text("same content"), new_c, pool, theme::dark);

    std::string result;
    diff(old_c, new_c, pool, result);
    assert(result.empty());
    std::println("PASS\n");
}

void test_diff_no_damage_empty_output() {
    std::println("--- test_diff_no_damage_empty_output ---");
    StylePool pool;
    Canvas old_c(10, 3, &pool);
    Canvas new_c(10, 3, &pool);
    new_c.reset_damage();

    std::string result;
    diff(old_c, new_c, pool, result);
    assert(result.empty());
    std::println("PASS\n");
}

void test_diff_changed_cell_nonempty() {
    std::println("--- test_diff_changed_cell_nonempty ---");
    StylePool pool;
    Canvas old_c(20, 3, &pool);
    Canvas new_c(20, 3, &pool);
    render_tree(text("before"), old_c, pool, theme::dark);
    render_tree(text("after "), new_c, pool, theme::dark);

    std::string result;
    diff(old_c, new_c, pool, result);
    assert(!result.empty());
    std::println("  diff bytes: {}", result.size());
    std::println("PASS\n");
}

void test_diff_single_cell_change() {
    std::println("--- test_diff_single_cell_change ---");
    StylePool pool;
    Canvas old_c(10, 3, &pool);
    Canvas new_c(10, 3, &pool);
    // Start with identical canvases
    render_tree(text("hello"), old_c, pool, theme::dark);
    render_tree(text("hello"), new_c, pool, theme::dark);

    // Mutate one cell in new canvas
    new_c.set(0, 0, U'H', 0);

    std::string result;
    diff(old_c, new_c, pool, result);
    // Should have a cursor move + the changed char
    assert(!result.empty());
    // The changed character 'H' should appear in the diff
    assert(result.find('H') != std::string::npos);
    std::println("  single-cell diff bytes: {}", result.size());
    std::println("PASS\n");
}

void test_diff_size_mismatch_full_repaint() {
    std::println("--- test_diff_size_mismatch_full_repaint ---");
    StylePool pool;
    Canvas old_c(20, 5, &pool);
    Canvas new_c(10, 3, &pool);
    render_tree(text("hello"), old_c, pool, theme::dark);
    render_tree(text("hello"), new_c, pool, theme::dark);

    std::string result;
    diff(old_c, new_c, pool, result);
    assert(!result.empty()); // size mismatch forces full repaint
    std::println("  size-mismatch diff bytes: {}", result.size());
    std::println("PASS\n");
}

void test_diff_unicode_char_encoded_correctly() {
    std::println("--- test_diff_unicode_char_encoded_correctly ---");
    StylePool pool;
    Canvas old_c(10, 3, &pool);
    Canvas new_c(10, 3, &pool);
    // Write snowman ☃ (U+2603 = E2 98 83 in UTF-8) to new canvas
    new_c.set(0, 0, U'\u2603', 0);

    std::string result;
    diff(old_c, new_c, pool, result);
    assert(!result.empty());
    // Snowman UTF-8 bytes: E2 98 83
    assert(result.find("\xe2\x98\x83") != std::string::npos);
    std::println("PASS\n");
}

void test_diff_multi_byte_unicode_sequence() {
    std::println("--- test_diff_multi_byte_unicode_sequence ---");
    StylePool pool;
    Canvas old_c(10, 3, &pool);
    Canvas new_c(10, 3, &pool);
    // U+1F600 GRINNING FACE = F0 9F 98 80 (4-byte UTF-8)
    new_c.set(0, 0, U'\U0001F600', 0);

    std::string result;
    diff(old_c, new_c, pool, result);
    assert(!result.empty());
    assert(result.find("\xf0\x9f\x98\x80") != std::string::npos);
    std::println("PASS\n");
}

void test_diff_two_byte_utf8() {
    std::println("--- test_diff_two_byte_utf8 ---");
    StylePool pool;
    Canvas old_c(10, 3, &pool);
    Canvas new_c(10, 3, &pool);
    // U+00E9 'é' = C3 A9 (2-byte UTF-8)
    new_c.set(0, 0, U'\u00e9', 0);

    std::string result;
    diff(old_c, new_c, pool, result);
    assert(!result.empty());
    assert(result.find("\xc3\xa9") != std::string::npos);
    std::println("PASS\n");
}

void test_diff_contains_cursor_move() {
    std::println("--- test_diff_contains_cursor_move ---");
    StylePool pool;
    Canvas old_c(20, 3, &pool);
    Canvas new_c(20, 3, &pool);
    // Change a cell not at (0,0) so a cursor move is needed
    new_c.set(5, 1, U'X', 0);

    std::string result;
    diff(old_c, new_c, pool, result);
    // ESC[row;colH cursor move sequence must be present
    assert(!result.empty());
    assert(result.find("\x1b[") != std::string::npos);
    std::println("PASS\n");
}

void test_diff_full_repaint_all_cells() {
    std::println("--- test_diff_full_repaint_all_cells ---");
    StylePool pool;
    Canvas old_c(5, 3, &pool);
    Canvas new_c(5, 3, &pool);
    // Fill entire new canvas with 'X'
    Rect all{{Columns{0}, Rows{0}}, {Columns{5}, Rows{3}}};
    new_c.fill(all, U'X', 0);

    std::string result;
    diff(old_c, new_c, pool, result);
    assert(!result.empty());
    // Count 'X' occurrences in result — should be 5*3 = 15
    int count = 0;
    for (char ch : result) if (ch == 'X') ++count;
    assert(count == 15);
    std::println("  full-repaint diff bytes: {}", result.size());
    std::println("PASS\n");
}

void test_diff_style_change_emits_sgr() {
    std::println("--- test_diff_style_change_emits_sgr ---");
    StylePool pool;
    uint16_t bold_id = pool.intern(Style{}.with_bold());
    Canvas old_c(10, 3, &pool);
    Canvas new_c(10, 3, &pool);
    new_c.set(0, 0, U'A', bold_id);

    std::string result;
    diff(old_c, new_c, pool, result);
    // Should contain a reset + bold SGR sequence
    assert(!result.empty());
    assert(result.find("\x1b[") != std::string::npos);
    std::println("PASS\n");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_diff_identical_canvases_empty_output();
    test_diff_no_damage_empty_output();
    test_diff_changed_cell_nonempty();
    test_diff_single_cell_change();
    test_diff_size_mismatch_full_repaint();
    test_diff_unicode_char_encoded_correctly();
    test_diff_multi_byte_unicode_sequence();
    test_diff_two_byte_utf8();
    test_diff_contains_cursor_move();
    test_diff_full_repaint_all_cells();
    test_diff_style_change_emits_sgr();
    std::println("=== ALL 11 TESTS PASSED ===");
}
