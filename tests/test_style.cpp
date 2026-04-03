// Tests for maya style system: Style attributes, SGR generation, merge, operator|
#include <maya/maya.hpp>
#include <cassert>
#include <print>

using namespace maya;

void test_style_empty_sgr() {
    std::println("--- test_style_empty_sgr ---");
    assert(Style{}.to_sgr().empty());
    std::println("PASS\n");
}

void test_style_bold_sgr() {
    std::println("--- test_style_bold_sgr ---");
    auto s = Style{}.with_bold();
    auto sgr = s.to_sgr();
    // Bold = SGR param "1", wrapped in ESC[...m
    assert(sgr.find("1") != std::string::npos);
    assert(s.bold == true);
    std::println("  bold sgr: '{}'", sgr);
    std::println("PASS\n");
}

void test_style_dim_sgr() {
    std::println("--- test_style_dim_sgr ---");
    auto s = Style{}.with_dim();
    assert(s.dim == true);
    assert(s.to_sgr().find("2") != std::string::npos);
    std::println("PASS\n");
}

void test_style_italic_sgr() {
    std::println("--- test_style_italic_sgr ---");
    auto s = Style{}.with_italic();
    assert(s.italic == true);
    assert(s.to_sgr().find("3") != std::string::npos);
    std::println("PASS\n");
}

void test_style_underline_sgr() {
    std::println("--- test_style_underline_sgr ---");
    auto s = Style{}.with_underline();
    assert(s.underline == true);
    assert(s.to_sgr().find("4") != std::string::npos);
    std::println("PASS\n");
}

void test_style_inverse_sgr() {
    std::println("--- test_style_inverse_sgr ---");
    auto s = Style{}.with_inverse();
    assert(s.inverse == true);
    assert(s.to_sgr().find("7") != std::string::npos);
    std::println("PASS\n");
}

void test_style_strikethrough_sgr() {
    std::println("--- test_style_strikethrough_sgr ---");
    auto s = Style{}.with_strikethrough();
    assert(s.strikethrough == true);
    assert(s.to_sgr().find("9") != std::string::npos);
    std::println("PASS\n");
}

void test_style_fg_color_sgr() {
    std::println("--- test_style_fg_color_sgr ---");
    auto s = Style{}.with_fg(Color::green());
    assert(s.fg.has_value());
    auto sgr = s.to_sgr();
    assert(sgr.find("32") != std::string::npos); // green fg = 32
    std::println("  fg(green) sgr: '{}'", sgr);
    std::println("PASS\n");
}

void test_style_bg_color_sgr() {
    std::println("--- test_style_bg_color_sgr ---");
    auto s = Style{}.with_bg(Color::blue());
    assert(s.bg.has_value());
    auto sgr = s.to_sgr();
    assert(sgr.find("44") != std::string::npos); // blue bg = 44
    std::println("  bg(blue) sgr: '{}'", sgr);
    std::println("PASS\n");
}

void test_style_rgb_fg_sgr() {
    std::println("--- test_style_rgb_fg_sgr ---");
    auto s = Style{}.with_fg(Color::rgb(100, 150, 200));
    auto sgr = s.to_sgr();
    assert(sgr.find("38;2;100;150;200") != std::string::npos);
    std::println("PASS\n");
}

void test_style_combined_sgr() {
    std::println("--- test_style_combined_sgr ---");
    auto s = Style{}.with_bold().with_fg(Color::red());
    auto sgr = s.to_sgr();
    assert(sgr.find("1")  != std::string::npos); // bold
    assert(sgr.find("31") != std::string::npos); // red fg
    std::println("  bold+red sgr: '{}'", sgr);
    std::println("PASS\n");
}

void test_style_merge_additive() {
    std::println("--- test_style_merge_additive ---");
    Style a = Style{}.with_bold();
    Style b = Style{}.with_italic().with_fg(Color::cyan());
    Style merged = a.merge(b);
    auto sgr = merged.to_sgr();
    assert(merged.bold == true);
    assert(merged.italic == true);
    assert(merged.fg.has_value());
    assert(sgr.find("36") != std::string::npos); // cyan fg
    std::println("PASS\n");
}

void test_style_merge_second_wins_fg() {
    std::println("--- test_style_merge_second_wins_fg ---");
    Style a = Style{}.with_fg(Color::red());
    Style b = Style{}.with_fg(Color::blue());
    Style merged = a.merge(b);
    // Second wins for fg color
    assert(merged.fg.has_value());
    assert(merged.fg->fg_sgr() == "34"); // blue
    std::println("PASS\n");
}

void test_style_operator_pipe() {
    std::println("--- test_style_operator_pipe ---");
    Style a = Style{}.with_bold();
    Style b = Style{}.with_fg(Color::magenta());
    Style combined = a | b;
    assert(combined.bold == true);
    assert(combined.fg.has_value());
    assert(combined.fg->fg_sgr() == "35"); // magenta
    std::println("PASS\n");
}

void test_style_pipe_equals_merge() {
    std::println("--- test_style_pipe_equals_merge ---");
    Style a = Style{}.with_bold().with_fg(Color::red());
    Style b = Style{}.with_italic().with_bg(Color::blue());
    assert((a | b).to_sgr() == a.merge(b).to_sgr());
    std::println("PASS\n");
}

void test_style_empty_predicate() {
    std::println("--- test_style_empty_predicate ---");
    assert( Style{}.empty());
    assert(!Style{}.with_bold().empty());
    assert(!Style{}.with_fg(Color::red()).empty());
    assert(!Style{}.with_bg(Color::red()).empty());
    std::println("PASS\n");
}

void test_style_predefined_bold() {
    std::println("--- test_style_predefined_bold ---");
    assert(bold_style.bold == true);
    assert(bold_style.to_sgr().find("1") != std::string::npos);
    std::println("PASS\n");
}

void test_style_predefined_dim() {
    std::println("--- test_style_predefined_dim ---");
    assert(dim_style.dim == true);
    std::println("PASS\n");
}

void test_style_predefined_fg_colors() {
    std::println("--- test_style_predefined_fg_colors ---");
    assert(fg_red.fg.has_value()   && fg_red.fg->fg_sgr()   == "31");
    assert(fg_green.fg.has_value() && fg_green.fg->fg_sgr() == "32");
    std::println("PASS\n");
}

void test_style_equality() {
    std::println("--- test_style_equality ---");
    Style a = Style{}.with_bold().with_fg(Color::red());
    Style b = Style{}.with_bold().with_fg(Color::red());
    Style c = Style{}.with_bold().with_fg(Color::green());
    assert(a == b);
    assert(a != c);
    std::println("PASS\n");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_style_empty_sgr();
    test_style_bold_sgr();
    test_style_dim_sgr();
    test_style_italic_sgr();
    test_style_underline_sgr();
    test_style_inverse_sgr();
    test_style_strikethrough_sgr();
    test_style_fg_color_sgr();
    test_style_bg_color_sgr();
    test_style_rgb_fg_sgr();
    test_style_combined_sgr();
    test_style_merge_additive();
    test_style_merge_second_wins_fg();
    test_style_operator_pipe();
    test_style_pipe_equals_merge();
    test_style_empty_predicate();
    test_style_predefined_bold();
    test_style_predefined_dim();
    test_style_predefined_fg_colors();
    test_style_equality();
    std::println("=== ALL 20 TESTS PASSED ===");
}
