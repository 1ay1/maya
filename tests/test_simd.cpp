// Tests for SIMD-accelerated bulk cell comparison operations
#include <maya/maya.hpp>
#include <cassert>
#include <cstring>
#include <print>

using namespace maya;

void test_bulk_eq_identical_arrays() {
    std::println("--- test_bulk_eq_identical_arrays ---");
    uint64_t a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint64_t b[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    assert(simd::bulk_eq(a, b, 8));
    std::println("PASS\n");
}

void test_bulk_eq_all_zeros() {
    std::println("--- test_bulk_eq_all_zeros ---");
    uint64_t a[16] = {};
    uint64_t b[16] = {};
    assert(simd::bulk_eq(a, b, 16));
    std::println("PASS\n");
}

void test_bulk_eq_last_element_differs() {
    std::println("--- test_bulk_eq_last_element_differs ---");
    uint64_t a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint64_t b[8] = {1, 2, 3, 4, 5, 6, 7, 9}; // last differs
    assert(!simd::bulk_eq(a, b, 8));
    std::println("PASS\n");
}

void test_bulk_eq_first_element_differs() {
    std::println("--- test_bulk_eq_first_element_differs ---");
    uint64_t a[8] = {99, 2, 3, 4, 5, 6, 7, 8};
    uint64_t b[8] = {0,  2, 3, 4, 5, 6, 7, 8};
    assert(!simd::bulk_eq(a, b, 8));
    std::println("PASS\n");
}

void test_bulk_eq_middle_element_differs() {
    std::println("--- test_bulk_eq_middle_element_differs ---");
    uint64_t a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint64_t b[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    b[4] = 0xDEADBEEF;
    assert(!simd::bulk_eq(a, b, 8));
    std::println("PASS\n");
}

void test_bulk_eq_single_element_same() {
    std::println("--- test_bulk_eq_single_element_same ---");
    uint64_t a[1] = {42};
    uint64_t b[1] = {42};
    assert(simd::bulk_eq(a, b, 1));
    std::println("PASS\n");
}

void test_bulk_eq_single_element_differs() {
    std::println("--- test_bulk_eq_single_element_differs ---");
    uint64_t a[1] = {1};
    uint64_t b[1] = {2};
    assert(!simd::bulk_eq(a, b, 1));
    std::println("PASS\n");
}

void test_bulk_eq_avx2_boundary_size_4() {
    std::println("--- test_bulk_eq_avx2_boundary_size_4 ---");
    // Exactly 4 elements: one AVX2 iteration, tests AVX2 boundary
    uint64_t a[4] = {10, 20, 30, 40};
    uint64_t b[4] = {10, 20, 30, 40};
    assert(simd::bulk_eq(a, b, 4));
    b[3] = 41;
    assert(!simd::bulk_eq(a, b, 4));
    std::println("PASS\n");
}

void test_find_first_diff_all_identical() {
    std::println("--- test_find_first_diff_all_identical ---");
    uint64_t a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint64_t b[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    // Should return count (8) when all are equal
    assert(simd::find_first_diff(a, b, 8) == 8);
    std::println("PASS\n");
}

void test_find_first_diff_first_element() {
    std::println("--- test_find_first_diff_first_element ---");
    uint64_t a[8] = {99, 2, 3, 4, 5, 6, 7, 8};
    uint64_t b[8] = {0,  2, 3, 4, 5, 6, 7, 8};
    assert(simd::find_first_diff(a, b, 8) == 0);
    std::println("PASS\n");
}

void test_find_first_diff_middle_element() {
    std::println("--- test_find_first_diff_middle_element ---");
    uint64_t a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint64_t b[8] = {1, 2, 3, 0, 5, 6, 7, 8}; // index 3 differs
    assert(simd::find_first_diff(a, b, 8) == 3);
    std::println("PASS\n");
}

void test_find_first_diff_last_element() {
    std::println("--- test_find_first_diff_last_element ---");
    uint64_t a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint64_t b[8] = {1, 2, 3, 4, 5, 6, 7, 0}; // index 7 differs
    assert(simd::find_first_diff(a, b, 8) == 7);
    std::println("PASS\n");
}

void test_find_first_diff_single_element_same() {
    std::println("--- test_find_first_diff_single_element_same ---");
    uint64_t a[1] = {5};
    uint64_t b[1] = {5};
    assert(simd::find_first_diff(a, b, 1) == 1); // returns count
    std::println("PASS\n");
}

void test_find_first_diff_single_element_differs() {
    std::println("--- test_find_first_diff_single_element_differs ---");
    uint64_t a[1] = {5};
    uint64_t b[1] = {6};
    assert(simd::find_first_diff(a, b, 6) == 0);
    std::println("PASS\n");
}

void test_find_first_diff_large_array() {
    std::println("--- test_find_first_diff_large_array ---");
    constexpr std::size_t N = 256;
    uint64_t a[N] = {};
    uint64_t b[N] = {};
    // All identical
    assert(simd::find_first_diff(a, b, N) == N);
    // Make element 200 differ
    b[200] = 0xFFFFFFFFFFFFFFFFULL;
    assert(simd::find_first_diff(a, b, N) == 200);
    std::println("PASS\n");
}

void test_skip_equal_finds_first_diff() {
    std::println("--- test_skip_equal_finds_first_diff ---");
    uint64_t a[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    uint64_t b[8] = {0, 0, 0, 42, 0, 0, 0, 0}; // index 3 differs
    // skip_equal(a, b, start=0, end=8) should return 3
    assert(simd::skip_equal(a, b, 0, 8) == 3);
    std::println("PASS\n");
}

void test_skip_equal_with_start_offset() {
    std::println("--- test_skip_equal_with_start_offset ---");
    uint64_t a[8] = {0, 0, 0, 0, 0, 99, 0, 0};
    uint64_t b[8] = {0, 0, 0, 0, 0, 0,  0, 0}; // index 5 differs
    // Start search from index 2
    assert(simd::skip_equal(a, b, 2, 8) == 5);
    std::println("PASS\n");
}

void test_skip_equal_all_same_returns_end() {
    std::println("--- test_skip_equal_all_same_returns_end ---");
    uint64_t a[4] = {7, 7, 7, 7};
    uint64_t b[4] = {7, 7, 7, 7};
    assert(simd::skip_equal(a, b, 0, 4) == 4);
    std::println("PASS\n");
}

void test_bulk_eq_large_terminal_row() {
    std::println("--- test_bulk_eq_large_terminal_row ---");
    // Simulate a 120-column terminal row (120 cells)
    constexpr int W = 120;
    uint64_t a[W] = {};
    uint64_t b[W] = {};
    // Blank rows match
    assert(simd::bulk_eq(a, b, W));
    // Single character change
    b[60] = 0x41; // 'A' codepoint in low bits
    assert(!simd::bulk_eq(a, b, W));
    assert(simd::find_first_diff(a, b, W) == 60);
    std::println("PASS\n");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_bulk_eq_identical_arrays();
    test_bulk_eq_all_zeros();
    test_bulk_eq_last_element_differs();
    test_bulk_eq_first_element_differs();
    test_bulk_eq_middle_element_differs();
    test_bulk_eq_single_element_same();
    test_bulk_eq_single_element_differs();
    test_bulk_eq_avx2_boundary_size_4();
    test_find_first_diff_all_identical();
    test_find_first_diff_first_element();
    test_find_first_diff_middle_element();
    test_find_first_diff_last_element();
    test_find_first_diff_single_element_same();
    test_find_first_diff_single_element_differs();
    test_find_first_diff_large_array();
    test_skip_equal_finds_first_diff();
    test_skip_equal_with_start_offset();
    test_skip_equal_all_same_returns_end();
    test_bulk_eq_large_terminal_row();
    std::println("=== ALL 19 TESTS PASSED ===");
}
