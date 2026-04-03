// Tests for maya core types: Strong<>, Size, Position, Rect, Edges, Dimension
#include <maya/maya.hpp>
#include <cassert>
#include <print>

using namespace maya;

void test_strong_arithmetic() {
    std::println("--- test_strong_arithmetic ---");
    Columns a{5}, b{3};
    assert((a + b).value == 8);
    assert((a - b).value == 2);
    assert((a * 2).value == 10);
    assert((a / 2).value == 2);
    assert((a % 3).value == 2);
    std::println("PASS\n");
}

void test_strong_comparison() {
    std::println("--- test_strong_comparison ---");
    Columns a{5}, b{3}, c{5};
    assert(a > b);
    assert(b < a);
    assert(a == c);
    assert(a != b);
    assert(b <= a);
    assert(a >= c);
    std::println("PASS\n");
}

void test_strong_compound_assign() {
    std::println("--- test_strong_compound_assign ---");
    Columns x{10};
    x += Columns{5};
    assert(x.value == 15);
    x -= Columns{3};
    assert(x.value == 12);
    std::println("PASS\n");
}

void test_strong_scalar_multiply() {
    std::println("--- test_strong_scalar_multiply ---");
    Columns c{4};
    assert((3 * c).value == 12);
    assert((c * 3).value == 12);
    std::println("PASS\n");
}

void test_strong_explicit_conversion() {
    std::println("--- test_strong_explicit_conversion ---");
    Columns c{7};
    assert(static_cast<int>(c) == 7);
    assert(c.raw() == 7);
    std::println("PASS\n");
}

void test_strong_default_zero() {
    std::println("--- test_strong_default_zero ---");
    Columns c;
    Rows r;
    assert(c.value == 0);
    assert(r.value == 0);
    std::println("PASS\n");
}

void test_size_is_zero() {
    std::println("--- test_size_is_zero ---");
    Size zero{Columns{0}, Rows{0}};
    assert(zero.is_zero());

    Size nonzero{Columns{1}, Rows{0}};
    assert(!nonzero.is_zero());

    Size nonzero2{Columns{0}, Rows{1}};
    assert(!nonzero2.is_zero());
    std::println("PASS\n");
}

void test_size_area() {
    std::println("--- test_size_area ---");
    Size s{Columns{10}, Rows{5}};
    assert(s.area() == 50);

    Size zero{Columns{0}, Rows{5}};
    assert(zero.area() == 0);
    std::println("PASS\n");
}

void test_size_equality() {
    std::println("--- test_size_equality ---");
    Size a{Columns{3}, Rows{4}};
    Size b{Columns{3}, Rows{4}};
    Size c{Columns{3}, Rows{5}};
    assert(a == b);
    assert(a != c);
    std::println("PASS\n");
}

void test_position_origin() {
    std::println("--- test_position_origin ---");
    auto p = Position::origin();
    assert(p.x.value == 0);
    assert(p.y.value == 0);
    std::println("PASS\n");
}

void test_position_equality() {
    std::println("--- test_position_equality ---");
    Position a{Columns{3}, Rows{7}};
    Position b{Columns{3}, Rows{7}};
    Position c{Columns{4}, Rows{7}};
    assert(a == b);
    assert(a != c);
    assert(a != Position::origin());
    std::println("PASS\n");
}

void test_rect_accessors() {
    std::println("--- test_rect_accessors ---");
    Rect r{{Columns{2}, Rows{3}}, {Columns{4}, Rows{5}}};
    assert(r.left().value   == 2);       // pos.x
    assert(r.top().value    == 3);       // pos.y
    assert(r.right().value  == 2 + 4);  // pos.x + size.width
    assert(r.bottom().value == 3 + 5);  // pos.y + size.height
    std::println("PASS\n");
}

void test_rect_contains() {
    std::println("--- test_rect_contains ---");
    // Rect from (2,3) with size (4,5): covers x=[2,6), y=[3,8)
    Rect r{{Columns{2}, Rows{3}}, {Columns{4}, Rows{5}}};

    assert( r.contains({Columns{2}, Rows{3}}));   // top-left corner (inclusive)
    assert( r.contains({Columns{5}, Rows{7}}));   // inside
    assert(!r.contains({Columns{6}, Rows{3}}));   // right edge (exclusive)
    assert(!r.contains({Columns{2}, Rows{8}}));   // bottom edge (exclusive)
    assert(!r.contains({Columns{1}, Rows{3}}));   // left of rect
    assert(!r.contains({Columns{2}, Rows{2}}));   // above rect
    std::println("PASS\n");
}

void test_rect_intersect() {
    std::println("--- test_rect_intersect ---");
    Rect a{{Columns{0}, Rows{0}}, {Columns{6}, Rows{6}}};
    Rect b{{Columns{3}, Rows{3}}, {Columns{4}, Rows{4}}};
    Rect i = a.intersect(b);
    assert(i.pos.x.value     == 3);
    assert(i.pos.y.value     == 3);
    assert(i.size.width.value  == 3); // min(6,7)=6 - max(0,3)=3 → 3
    assert(i.size.height.value == 3);
    std::println("PASS\n");
}

void test_rect_intersect_disjoint() {
    std::println("--- test_rect_intersect_disjoint ---");
    Rect a{{Columns{0}, Rows{0}}, {Columns{3}, Rows{3}}};
    Rect b{{Columns{5}, Rows{5}}, {Columns{3}, Rows{3}}};
    Rect i = a.intersect(b);
    assert(i.size.is_zero());
    std::println("PASS\n");
}

void test_rect_unite() {
    std::println("--- test_rect_unite ---");
    Rect a{{Columns{0}, Rows{0}}, {Columns{3}, Rows{3}}};
    Rect b{{Columns{2}, Rows{2}}, {Columns{4}, Rows{4}}};
    Rect u = a.unite(b);
    assert(u.pos.x.value     == 0);
    assert(u.pos.y.value     == 0);
    assert(u.right().value   == 6);
    assert(u.bottom().value  == 6);
    std::println("PASS\n");
}

void test_rect_unite_with_zero() {
    std::println("--- test_rect_unite_with_zero ---");
    Rect zero{Position::origin(), {Columns{0}, Rows{0}}};
    Rect r{{Columns{2}, Rows{3}}, {Columns{4}, Rows{5}}};
    assert(r.unite(zero) == r);
    assert(zero.unite(r) == r);
    std::println("PASS\n");
}

void test_edges_uniform() {
    std::println("--- test_edges_uniform ---");
    Edges<int> e(5);
    assert(e.top == 5 && e.right == 5 && e.bottom == 5 && e.left == 5);
    std::println("PASS\n");
}

void test_edges_vertical_horizontal() {
    std::println("--- test_edges_vertical_horizontal ---");
    Edges<int> e(2, 4); // vertical=2, horizontal=4
    assert(e.top == 2 && e.bottom == 2);
    assert(e.left == 4 && e.right == 4);
    std::println("PASS\n");
}

void test_edges_all_four() {
    std::println("--- test_edges_all_four ---");
    Edges<int> e(1, 2, 3, 4); // top, right, bottom, left
    assert(e.top == 1 && e.right == 2 && e.bottom == 3 && e.left == 4);
    std::println("PASS\n");
}

void test_edges_subscript() {
    std::println("--- test_edges_subscript ---");
    Edges<int> e(1, 2, 3, 4);
    assert(e[Edge::Top]    == 1);
    assert(e[Edge::Right]  == 2);
    assert(e[Edge::Bottom] == 3);
    assert(e[Edge::Left]   == 4);
    std::println("PASS\n");
}

void test_dimension_auto() {
    std::println("--- test_dimension_auto ---");
    Dimension d;
    assert(d.is_auto());
    assert(!d.is_fixed());
    assert(!d.is_percent());
    std::println("PASS\n");
}

void test_dimension_fixed() {
    std::println("--- test_dimension_fixed ---");
    Dimension d = Dimension::fixed(42);
    assert(d.is_fixed());
    assert(!d.is_auto());
    assert(d.resolve(100) == 42);
    std::println("PASS\n");
}

void test_dimension_percent() {
    std::println("--- test_dimension_percent ---");
    Dimension d = Dimension::percent(50.0f);
    assert(d.is_percent());
    assert(d.resolve(200) == 100); // 50% of 200
    std::println("PASS\n");
}

void test_dimension_from_int() {
    std::println("--- test_dimension_from_int ---");
    Dimension d(10);
    assert(d.is_fixed());
    assert(d.resolve(999) == 10);
    std::println("PASS\n");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_strong_arithmetic();
    test_strong_comparison();
    test_strong_compound_assign();
    test_strong_scalar_multiply();
    test_strong_explicit_conversion();
    test_strong_default_zero();
    test_size_is_zero();
    test_size_area();
    test_size_equality();
    test_position_origin();
    test_position_equality();
    test_rect_accessors();
    test_rect_contains();
    test_rect_intersect();
    test_rect_intersect_disjoint();
    test_rect_unite();
    test_rect_unite_with_zero();
    test_edges_uniform();
    test_edges_vertical_horizontal();
    test_edges_all_four();
    test_edges_subscript();
    test_dimension_auto();
    test_dimension_fixed();
    test_dimension_percent();
    test_dimension_from_int();
    std::println("=== ALL 25 TESTS PASSED ===");
}
