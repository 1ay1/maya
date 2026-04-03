// Tests for ANSI escape sequence generation and StyleApplier
#include <maya/maya.hpp>
#include <cassert>
#include <print>

using namespace maya;

// ============================================================================
// write_move_to (zero-alloc) vs move_to (allocating) consistency
// ============================================================================

void test_move_to_format() {
    std::println("--- test_move_to_format ---");
    // move_to(col, row) → ESC[row;colH
    assert(ansi::move_to(1, 1) == "\x1b[1;1H");
    assert(ansi::move_to(10, 5) == "\x1b[5;10H");
    assert(ansi::move_to(80, 24) == "\x1b[24;80H");
    std::println("PASS\n");
}

void test_write_move_to_matches_move_to() {
    std::println("--- test_write_move_to_matches_move_to ---");
    // Zero-alloc write_move_to must produce identical output to move_to
    for (auto [col, row] : std::initializer_list<std::pair<int,int>>{{1,1},{10,5},{80,24},{1,100}}) {
        std::string expected = ansi::move_to(col, row);
        std::string got;
        ansi::write_move_to(got, col, row);
        assert(got == expected);
    }
    std::println("PASS\n");
}

void test_write_move_to_appends() {
    std::println("--- test_write_move_to_appends ---");
    std::string out = "PREFIX";
    ansi::write_move_to(out, 1, 1);
    assert(out.starts_with("PREFIX"));
    assert(out.size() > 6);
    std::println("PASS\n");
}

// ============================================================================
// Cursor movement helpers
// ============================================================================

void test_move_up_down_left_right() {
    std::println("--- test_move_up_down_left_right ---");
    assert(ansi::move_up(1)    == "\x1b[1A");
    assert(ansi::move_down(1)  == "\x1b[1B");
    assert(ansi::move_right(1) == "\x1b[1C");
    assert(ansi::move_left(1)  == "\x1b[1D");
    assert(ansi::move_up(3)    == "\x1b[3A");
    assert(ansi::move_down(5)  == "\x1b[5B");
    std::println("PASS\n");
}

void test_move_zero_returns_empty() {
    std::println("--- test_move_zero_returns_empty ---");
    assert(ansi::move_up(0).empty());
    assert(ansi::move_down(0).empty());
    assert(ansi::move_right(0).empty());
    assert(ansi::move_left(0).empty());
    std::println("PASS\n");
}

void test_home_sequence() {
    std::println("--- test_home_sequence ---");
    assert(ansi::home() == "\x1b[H");
    std::println("PASS\n");
}

// ============================================================================
// Screen clearing
// ============================================================================

void test_clear_screen_sequence() {
    std::println("--- test_clear_screen_sequence ---");
    assert(ansi::clear_screen() == "\x1b[2J");
    std::println("PASS\n");
}

void test_clear_line_sequence() {
    std::println("--- test_clear_line_sequence ---");
    assert(ansi::clear_line() == "\x1b[2K");
    std::println("PASS\n");
}

// ============================================================================
// Known ANSI constants
// ============================================================================

void test_cursor_show_hide_constants() {
    std::println("--- test_cursor_show_hide_constants ---");
    // DEC private mode 25: show/hide cursor
    std::string show(ansi::show_cursor);
    std::string hide(ansi::hide_cursor);
    assert(show.find("?25h") != std::string::npos);
    assert(hide.find("?25l") != std::string::npos);
    std::println("PASS\n");
}

void test_sync_markers() {
    std::println("--- test_sync_markers ---");
    // DEC private mode 2026: synchronized output
    std::string start(ansi::sync_start);
    std::string end(ansi::sync_end);
    assert(start.find("?2026h") != std::string::npos);
    assert(end.find("?2026l")   != std::string::npos);
    std::println("PASS\n");
}

void test_reset_constant() {
    std::println("--- test_reset_constant ---");
    std::string r(ansi::reset);
    assert(r == "\x1b[0m" || r == "\x1b[m");
    std::println("PASS\n");
}

// ============================================================================
// Color SGR sequences
// ============================================================================

void test_ansi_fg_sequence() {
    std::println("--- test_ansi_fg_sequence ---");
    std::string s = ansi::fg(Color::red());
    assert(s == "\x1b[31m");
    std::println("PASS\n");
}

void test_ansi_bg_sequence() {
    std::println("--- test_ansi_bg_sequence ---");
    std::string s = ansi::bg(Color::blue());
    assert(s == "\x1b[44m");
    std::println("PASS\n");
}

// ============================================================================
// StyleApplier - allocating variants
// ============================================================================

void test_style_applier_apply_empty() {
    std::println("--- test_style_applier_apply_empty ---");
    std::string s = ansi::StyleApplier::apply(Style{});
    assert(s.empty()); // empty style produces no SGR
    std::println("PASS\n");
}

void test_style_applier_apply_bold() {
    std::println("--- test_style_applier_apply_bold ---");
    std::string s = ansi::StyleApplier::apply(Style{}.with_bold());
    assert(s.find("1") != std::string::npos);
    assert(s.starts_with("\x1b["));
    assert(s.ends_with("m"));
    std::println("PASS\n");
}

void test_style_applier_apply_multiple() {
    std::println("--- test_style_applier_apply_multiple ---");
    Style s = Style{}.with_bold().with_italic().with_fg(Color::red());
    std::string sgr = ansi::StyleApplier::apply(s);
    assert(sgr.find("1")  != std::string::npos); // bold
    assert(sgr.find("3")  != std::string::npos); // italic
    assert(sgr.find("31") != std::string::npos); // red fg
    std::println("PASS\n");
}

void test_style_applier_transition_same_style() {
    std::println("--- test_style_applier_transition_same_style ---");
    Style s = Style{}.with_bold().with_fg(Color::green());
    assert(ansi::StyleApplier::transition(s, s).empty());
    std::println("PASS\n");
}

void test_style_applier_transition_add_attribute() {
    std::println("--- test_style_applier_transition_add_attribute ---");
    Style a = Style{}.with_bold();
    Style b = Style{}.with_bold().with_fg(Color::cyan());
    std::string t = ansi::StyleApplier::transition(a, b);
    assert(!t.empty());
    assert(t.find("36") != std::string::npos); // cyan fg = 36
    std::println("PASS\n");
}

void test_style_applier_transition_remove_attribute_resets() {
    std::println("--- test_style_applier_transition_remove_attribute_resets ---");
    // Removing bold requires a reset because SGR has no individual "un-bold"
    Style a = Style{}.with_bold().with_fg(Color::red());
    Style b = Style{}.with_fg(Color::red()); // bold removed
    std::string t = ansi::StyleApplier::transition(a, b);
    assert(!t.empty());
    assert(t.find("\x1b[0m") != std::string::npos); // reset
    std::println("PASS\n");
}

// ============================================================================
// StyleApplier - zero-alloc variants
// ============================================================================

void test_style_applier_apply_to_matches_apply() {
    std::println("--- test_style_applier_apply_to_matches_apply ---");
    Style s = Style{}.with_bold().with_fg(Color::green());
    std::string expected = ansi::StyleApplier::apply(s);
    std::string got;
    ansi::StyleApplier::apply_to(s, got);
    assert(got == expected);
    std::println("PASS\n");
}

void test_style_applier_transition_to_matches_transition() {
    std::println("--- test_style_applier_transition_to_matches_transition ---");
    Style a = Style{}.with_bold();
    Style b = Style{}.with_bold().with_fg(Color::blue());
    std::string expected = ansi::StyleApplier::transition(a, b);
    std::string got;
    ansi::StyleApplier::transition_to(a, b, got);
    assert(got == expected);
    std::println("PASS\n");
}

void test_style_applier_apply_to_appends() {
    std::println("--- test_style_applier_apply_to_appends ---");
    Style s = Style{}.with_bold();
    std::string out = "START";
    ansi::StyleApplier::apply_to(s, out);
    assert(out.starts_with("START"));
    assert(out.size() > 5);
    std::println("PASS\n");
}

void test_style_applier_transition_to_empty_on_same() {
    std::println("--- test_style_applier_transition_to_empty_on_same ---");
    Style s = Style{}.with_italic();
    std::string out;
    ansi::StyleApplier::transition_to(s, s, out);
    assert(out.empty());
    std::println("PASS\n");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_move_to_format();
    test_write_move_to_matches_move_to();
    test_write_move_to_appends();
    test_move_up_down_left_right();
    test_move_zero_returns_empty();
    test_home_sequence();
    test_clear_screen_sequence();
    test_clear_line_sequence();
    test_cursor_show_hide_constants();
    test_sync_markers();
    test_reset_constant();
    test_ansi_fg_sequence();
    test_ansi_bg_sequence();
    test_style_applier_apply_empty();
    test_style_applier_apply_bold();
    test_style_applier_apply_multiple();
    test_style_applier_transition_same_style();
    test_style_applier_transition_add_attribute();
    test_style_applier_transition_remove_attribute_resets();
    test_style_applier_apply_to_matches_apply();
    test_style_applier_transition_to_matches_transition();
    test_style_applier_apply_to_appends();
    test_style_applier_transition_to_empty_on_same();
    std::println("=== ALL 23 TESTS PASSED ===");
}
