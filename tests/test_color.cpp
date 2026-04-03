// Tests for maya color system: named colors, RGB, indexed, HSL, SGR generation
#include <maya/maya.hpp>
#include <cassert>
#include <print>

using namespace maya;

void test_color_named_fg_sgr() {
    std::println("--- test_color_named_fg_sgr ---");
    // AnsiColor values: Black=0, Red=1, Green=2, Yellow=3, Blue=4, Cyan=6, White=7
    // fg SGR = 30 + index for dark colors
    assert(Color::black().fg_sgr()   == "30");
    assert(Color::red().fg_sgr()     == "31");
    assert(Color::green().fg_sgr()   == "32");
    assert(Color::yellow().fg_sgr()  == "33");
    assert(Color::blue().fg_sgr()    == "34");
    assert(Color::magenta().fg_sgr() == "35");
    assert(Color::cyan().fg_sgr()    == "36");
    assert(Color::white().fg_sgr()   == "37");
    std::println("PASS\n");
}

void test_color_bright_fg_sgr() {
    std::println("--- test_color_bright_fg_sgr ---");
    // BrightX values: 8-15, fg SGR = 90 + (index - 8)
    assert(Color::bright_black().fg_sgr()   == "90");
    assert(Color::bright_red().fg_sgr()     == "91");
    assert(Color::bright_green().fg_sgr()   == "92");
    assert(Color::bright_white().fg_sgr()   == "97");
    assert(Color::gray().fg_sgr()           == "90"); // alias for bright_black
    assert(Color::grey().fg_sgr()           == "90"); // alias for bright_black
    std::println("PASS\n");
}

void test_color_named_bg_sgr() {
    std::println("--- test_color_named_bg_sgr ---");
    // bg SGR = 40 + index for dark colors
    assert(Color::black().bg_sgr()   == "40");
    assert(Color::red().bg_sgr()     == "41");
    assert(Color::green().bg_sgr()   == "42");
    assert(Color::blue().bg_sgr()    == "44");
    assert(Color::cyan().bg_sgr()    == "46");
    assert(Color::white().bg_sgr()   == "47");
    std::println("PASS\n");
}

void test_color_bright_bg_sgr() {
    std::println("--- test_color_bright_bg_sgr ---");
    // bg SGR = 100 + (index - 8) for bright colors
    assert(Color::bright_black().bg_sgr() == "100");
    assert(Color::bright_red().bg_sgr()   == "101");
    assert(Color::bright_white().bg_sgr() == "107");
    std::println("PASS\n");
}

void test_color_rgb_fg_sgr() {
    std::println("--- test_color_rgb_fg_sgr ---");
    auto c = Color::rgb(255, 128, 0);
    assert(c.fg_sgr() == "38;2;255;128;0");
    assert(c.kind() == Color::Kind::Rgb);
    assert(c.r() == 255);
    assert(c.g() == 128);
    assert(c.b() == 0);
    std::println("PASS\n");
}

void test_color_rgb_bg_sgr() {
    std::println("--- test_color_rgb_bg_sgr ---");
    auto c = Color::rgb(10, 20, 30);
    assert(c.bg_sgr() == "48;2;10;20;30");
    std::println("PASS\n");
}

void test_color_indexed_fg_sgr() {
    std::println("--- test_color_indexed_fg_sgr ---");
    auto c = Color::indexed(42);
    assert(c.fg_sgr()  == "38;5;42");
    assert(c.kind()    == Color::Kind::Indexed);
    assert(c.index()   == 42);
    std::println("PASS\n");
}

void test_color_indexed_bg_sgr() {
    std::println("--- test_color_indexed_bg_sgr ---");
    auto c = Color::indexed(200);
    assert(c.bg_sgr() == "48;5;200");
    std::println("PASS\n");
}

void test_color_hex() {
    std::println("--- test_color_hex ---");
    constexpr auto c = Color::hex(0xFF8000);
    assert(c.r() == 0xFF);
    assert(c.g() == 0x80);
    assert(c.b() == 0x00);
    assert(c.kind() == Color::Kind::Rgb);
    assert(c.fg_sgr() == "38;2;255;128;0");
    std::println("PASS\n");
}

void test_color_hex_black_white() {
    std::println("--- test_color_hex_black_white ---");
    constexpr auto black = Color::hex(0x000000);
    constexpr auto white = Color::hex(0xFFFFFF);
    assert(black.r() == 0 && black.g() == 0 && black.b() == 0);
    assert(white.r() == 255 && white.g() == 255 && white.b() == 255);
    std::println("PASS\n");
}

void test_color_hsl_red() {
    std::println("--- test_color_hsl_red ---");
    // Pure red: hue=0, saturation=1, lightness=0.5
    auto c = Color::hsl(0.0f, 1.0f, 0.5f);
    assert(c.r() > 200); // close to 255
    assert(c.g() < 10);
    assert(c.b() < 10);
    std::println("  hsl(0,1,0.5) → rgb({},{},{})", c.r(), c.g(), c.b());
    std::println("PASS\n");
}

void test_color_hsl_gray() {
    std::println("--- test_color_hsl_gray ---");
    // Gray: saturation=0, lightness=0.5 → r=g=b=127 or 128
    auto c = Color::hsl(0.0f, 0.0f, 0.5f);
    assert(c.r() == c.g() && c.g() == c.b());
    assert(c.r() >= 127 && c.r() <= 128);
    std::println("  hsl(0,0,0.5) → rgb({},{},{})", c.r(), c.g(), c.b());
    std::println("PASS\n");
}

void test_color_append_fg_matches_fg_sgr() {
    std::println("--- test_color_append_fg_matches_fg_sgr ---");
    // Zero-alloc append must produce the same result as the allocating method
    for (auto c : {Color::red(), Color::rgb(10,20,30), Color::indexed(7)}) {
        std::string direct = c.fg_sgr();
        std::string appended;
        c.append_fg_sgr(appended);
        assert(direct == appended);
    }
    std::println("PASS\n");
}

void test_color_append_bg_matches_bg_sgr() {
    std::println("--- test_color_append_bg_matches_bg_sgr ---");
    for (auto c : {Color::blue(), Color::rgb(50,100,150), Color::indexed(42)}) {
        std::string direct = c.bg_sgr();
        std::string appended;
        c.append_bg_sgr(appended);
        assert(direct == appended);
    }
    std::println("PASS\n");
}

void test_color_kind_variants() {
    std::println("--- test_color_kind_variants ---");
    assert(Color::red().kind()      == Color::Kind::Named);
    assert(Color::indexed(5).kind() == Color::Kind::Indexed);
    assert(Color::rgb(1,2,3).kind() == Color::Kind::Rgb);
    std::println("PASS\n");
}

void test_color_default_is_white() {
    std::println("--- test_color_default_is_white ---");
    // Default Color() is white (AnsiColor::White = 7)
    Color c;
    assert(c.kind() == Color::Kind::Named);
    assert(c.fg_sgr() == "37"); // white fg
    std::println("PASS\n");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_color_named_fg_sgr();
    test_color_bright_fg_sgr();
    test_color_named_bg_sgr();
    test_color_bright_bg_sgr();
    test_color_rgb_fg_sgr();
    test_color_rgb_bg_sgr();
    test_color_indexed_fg_sgr();
    test_color_indexed_bg_sgr();
    test_color_hex();
    test_color_hex_black_white();
    test_color_hsl_red();
    test_color_hsl_gray();
    test_color_append_fg_matches_fg_sgr();
    test_color_append_bg_matches_bg_sgr();
    test_color_kind_variants();
    test_color_default_is_white();
    std::println("=== ALL 16 TESTS PASSED ===");
}
