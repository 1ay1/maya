// Tests for the input parser FSM: key events, mouse, UTF-8, modifiers
#include <maya/maya.hpp>
#include <cassert>
#include <print>

using namespace maya;

// Convenience: extract KeyEvent or assert failure
static const KeyEvent& get_key(const Event& ev) {
    const auto* ke = std::get_if<KeyEvent>(&ev);
    assert(ke != nullptr);
    return *ke;
}

static const MouseEvent& get_mouse(const Event& ev) {
    const auto* me = std::get_if<MouseEvent>(&ev);
    assert(me != nullptr);
    return *me;
}

void test_input_ascii_char() {
    std::println("--- test_input_ascii_char ---");
    InputParser p;
    auto events = p.feed("a");
    assert(events.size() == 1);
    const KeyEvent& ke = get_key(events[0]);
    const auto* ck = std::get_if<CharKey>(&ke.key);
    assert(ck != nullptr && ck->codepoint == U'a');
    assert(ke.mods.none());
    std::println("PASS\n");
}

void test_input_multiple_ascii_chars() {
    std::println("--- test_input_multiple_ascii_chars ---");
    InputParser p;
    auto events = p.feed("xyz");
    assert(events.size() == 3);
    assert(std::get<CharKey>(get_key(events[0]).key).codepoint == U'x');
    assert(std::get<CharKey>(get_key(events[1]).key).codepoint == U'y');
    assert(std::get<CharKey>(get_key(events[2]).key).codepoint == U'z');
    std::println("PASS\n");
}

void test_input_space_char() {
    std::println("--- test_input_space_char ---");
    InputParser p;
    auto events = p.feed(" ");
    assert(!events.empty());
    const KeyEvent& ke = get_key(events[0]);
    const auto* ck = std::get_if<CharKey>(&ke.key);
    assert(ck != nullptr && ck->codepoint == U' ');
    std::println("PASS\n");
}

void test_input_up_arrow() {
    std::println("--- test_input_up_arrow ---");
    InputParser p;
    auto events = p.feed("\x1b[A");
    assert(!events.empty());
    const KeyEvent& ke = get_key(events[0]);
    const auto* sk = std::get_if<SpecialKey>(&ke.key);
    assert(sk != nullptr && *sk == SpecialKey::Up);
    std::println("PASS\n");
}

void test_input_down_arrow() {
    std::println("--- test_input_down_arrow ---");
    InputParser p;
    auto events = p.feed("\x1b[B");
    assert(!events.empty());
    const KeyEvent& ke = get_key(events[0]);
    const auto* sk = std::get_if<SpecialKey>(&ke.key);
    assert(sk != nullptr && *sk == SpecialKey::Down);
    std::println("PASS\n");
}

void test_input_right_arrow() {
    std::println("--- test_input_right_arrow ---");
    InputParser p;
    auto events = p.feed("\x1b[C");
    assert(!events.empty());
    const auto* sk = std::get_if<SpecialKey>(&get_key(events[0]).key);
    assert(sk != nullptr && *sk == SpecialKey::Right);
    std::println("PASS\n");
}

void test_input_left_arrow() {
    std::println("--- test_input_left_arrow ---");
    InputParser p;
    auto events = p.feed("\x1b[D");
    assert(!events.empty());
    const auto* sk = std::get_if<SpecialKey>(&get_key(events[0]).key);
    assert(sk != nullptr && *sk == SpecialKey::Left);
    std::println("PASS\n");
}

void test_input_home_key() {
    std::println("--- test_input_home_key ---");
    InputParser p;
    auto events = p.feed("\x1b[H");
    assert(!events.empty());
    const auto* sk = std::get_if<SpecialKey>(&get_key(events[0]).key);
    assert(sk != nullptr && *sk == SpecialKey::Home);
    std::println("PASS\n");
}

void test_input_end_key() {
    std::println("--- test_input_end_key ---");
    InputParser p;
    auto events = p.feed("\x1b[F");
    assert(!events.empty());
    const auto* sk = std::get_if<SpecialKey>(&get_key(events[0]).key);
    assert(sk != nullptr && *sk == SpecialKey::End);
    std::println("PASS\n");
}

void test_input_enter_key() {
    std::println("--- test_input_enter_key ---");
    InputParser p;
    auto events = p.feed("\r");
    assert(!events.empty());
    const auto* sk = std::get_if<SpecialKey>(&get_key(events[0]).key);
    assert(sk != nullptr && *sk == SpecialKey::Enter);
    std::println("PASS\n");
}

void test_input_backspace() {
    std::println("--- test_input_backspace ---");
    InputParser p;
    auto events = p.feed("\x7f");
    assert(!events.empty());
    const auto* sk = std::get_if<SpecialKey>(&get_key(events[0]).key);
    assert(sk != nullptr && *sk == SpecialKey::Backspace);
    std::println("PASS\n");
}

void test_input_tab() {
    std::println("--- test_input_tab ---");
    InputParser p;
    auto events = p.feed("\t");
    assert(!events.empty());
    const auto* sk = std::get_if<SpecialKey>(&get_key(events[0]).key);
    assert(sk != nullptr && *sk == SpecialKey::Tab);
    std::println("PASS\n");
}

void test_input_ctrl_c() {
    std::println("--- test_input_ctrl_c ---");
    InputParser p;
    auto events = p.feed("\x03"); // ETX = Ctrl+C
    assert(!events.empty());
    const KeyEvent& ke = get_key(events[0]);
    const auto* ck = std::get_if<CharKey>(&ke.key);
    assert(ck != nullptr && ck->codepoint == 'c');
    assert(ke.mods.ctrl == true);
    std::println("PASS\n");
}

void test_input_ctrl_a() {
    std::println("--- test_input_ctrl_a ---");
    InputParser p;
    auto events = p.feed("\x01"); // SOH = Ctrl+A
    assert(!events.empty());
    const KeyEvent& ke = get_key(events[0]);
    const auto* ck = std::get_if<CharKey>(&ke.key);
    assert(ck != nullptr && ck->codepoint == 'a');
    assert(ke.mods.ctrl == true);
    std::println("PASS\n");
}

void test_input_utf8_two_byte() {
    std::println("--- test_input_utf8_two_byte ---");
    InputParser p;
    // U+00E9 'é' = C3 A9
    auto events = p.feed("\xc3\xa9");
    assert(!events.empty());
    const auto* ck = std::get_if<CharKey>(&get_key(events[0]).key);
    assert(ck != nullptr && ck->codepoint == 0x00E9);
    std::println("PASS\n");
}

void test_input_utf8_three_byte() {
    std::println("--- test_input_utf8_three_byte ---");
    InputParser p;
    // U+2603 snowman ☃ = E2 98 83
    auto events = p.feed("\xe2\x98\x83");
    assert(!events.empty());
    const auto* ck = std::get_if<CharKey>(&get_key(events[0]).key);
    assert(ck != nullptr && ck->codepoint == 0x2603);
    std::println("PASS\n");
}

void test_input_utf8_four_byte() {
    std::println("--- test_input_utf8_four_byte ---");
    InputParser p;
    // U+1F600 grinning face = F0 9F 98 80
    auto events = p.feed("\xf0\x9f\x98\x80");
    assert(!events.empty());
    const auto* ck = std::get_if<CharKey>(&get_key(events[0]).key);
    assert(ck != nullptr && ck->codepoint == 0x1F600);
    std::println("PASS\n");
}

void test_input_mouse_left_press() {
    std::println("--- test_input_mouse_left_press ---");
    InputParser p;
    // SGR mouse: ESC[<0;5;3M = left button press at col=5, row=3
    auto events = p.feed("\x1b[<0;5;3M");
    assert(!events.empty());
    const MouseEvent& me = get_mouse(events[0]);
    assert(me.button == MouseButton::Left);
    assert(me.kind   == MouseEventKind::Press);
    assert(me.x.value == 5);
    assert(me.y.value == 3);
    std::println("PASS\n");
}

void test_input_mouse_left_release() {
    std::println("--- test_input_mouse_left_release ---");
    InputParser p;
    // SGR mouse release: trailing 'm' instead of 'M'
    auto events = p.feed("\x1b[<0;5;3m");
    assert(!events.empty());
    const MouseEvent& me = get_mouse(events[0]);
    assert(me.kind == MouseEventKind::Release);
    std::println("PASS\n");
}

void test_input_mouse_right_button() {
    std::println("--- test_input_mouse_right_button ---");
    InputParser p;
    // Button 2 = right mouse button
    auto events = p.feed("\x1b[<2;1;1M");
    assert(!events.empty());
    const MouseEvent& me = get_mouse(events[0]);
    assert(me.button == MouseButton::Right);
    std::println("PASS\n");
}

void test_input_mouse_scroll_up() {
    std::println("--- test_input_mouse_scroll_up ---");
    InputParser p;
    // Button 64 = scroll up
    auto events = p.feed("\x1b[<64;1;1M");
    assert(!events.empty());
    const MouseEvent& me = get_mouse(events[0]);
    assert(me.button == MouseButton::ScrollUp);
    std::println("PASS\n");
}

void test_input_focus_gained() {
    std::println("--- test_input_focus_gained ---");
    InputParser p;
    // Focus gained: ESC[I
    auto events = p.feed("\x1b[I");
    assert(!events.empty());
    const auto* fe = std::get_if<FocusEvent>(&events[0]);
    assert(fe != nullptr && fe->focused == true);
    std::println("PASS\n");
}

void test_input_focus_lost() {
    std::println("--- test_input_focus_lost ---");
    InputParser p;
    // Focus lost: ESC[O
    auto events = p.feed("\x1b[O");
    assert(!events.empty());
    const auto* fe = std::get_if<FocusEvent>(&events[0]);
    assert(fe != nullptr && fe->focused == false);
    std::println("PASS\n");
}

void test_input_bracketed_paste() {
    std::println("--- test_input_bracketed_paste ---");
    InputParser p;
    // Bracketed paste: ESC[200~ content ESC[201~
    auto events = p.feed("\x1b[200~hello paste\x1b[201~");
    assert(!events.empty());
    const auto* pe = std::get_if<PasteEvent>(&events[0]);
    assert(pe != nullptr);
    assert(pe->content == "hello paste");
    std::println("PASS\n");
}

void test_input_page_up() {
    std::println("--- test_input_page_up ---");
    InputParser p;
    auto events = p.feed("\x1b[5~");
    assert(!events.empty());
    const auto* sk = std::get_if<SpecialKey>(&get_key(events[0]).key);
    assert(sk != nullptr && *sk == SpecialKey::PageUp);
    std::println("PASS\n");
}

void test_input_page_down() {
    std::println("--- test_input_page_down ---");
    InputParser p;
    auto events = p.feed("\x1b[6~");
    assert(!events.empty());
    const auto* sk = std::get_if<SpecialKey>(&get_key(events[0]).key);
    assert(sk != nullptr && *sk == SpecialKey::PageDown);
    std::println("PASS\n");
}

void test_input_delete_key() {
    std::println("--- test_input_delete_key ---");
    InputParser p;
    auto events = p.feed("\x1b[3~");
    assert(!events.empty());
    const auto* sk = std::get_if<SpecialKey>(&get_key(events[0]).key);
    assert(sk != nullptr && *sk == SpecialKey::Delete);
    std::println("PASS\n");
}

void test_input_f1_key() {
    std::println("--- test_input_f1_key ---");
    InputParser p;
    // F1 via SS3: ESC O P
    auto events = p.feed("\x1bOP");
    assert(!events.empty());
    const auto* sk = std::get_if<SpecialKey>(&get_key(events[0]).key);
    assert(sk != nullptr && *sk == SpecialKey::F1);
    std::println("PASS\n");
}

void test_input_mixed_sequence() {
    std::println("--- test_input_mixed_sequence ---");
    InputParser p;
    // ASCII char + arrow key + ASCII char
    auto events = p.feed("a\x1b[Cb");
    assert(events.size() == 3);
    assert(std::get<CharKey>(get_key(events[0]).key).codepoint == U'a');
    assert(*std::get_if<SpecialKey>(&get_key(events[1]).key) == SpecialKey::Right);
    assert(std::get<CharKey>(get_key(events[2]).key).codepoint == U'b');
    std::println("PASS\n");
}

void test_input_has_pending_false_after_complete() {
    std::println("--- test_input_has_pending_false_after_complete ---");
    InputParser p;
    (void)p.feed("a");
    assert(!p.has_pending()); // complete sequence consumed
    std::println("PASS\n");
}

void test_input_reset_clears_state() {
    std::println("--- test_input_reset_clears_state ---");
    InputParser p;
    p.reset();
    assert(!p.has_pending());
    auto events = p.feed("z");
    assert(!events.empty());
    std::println("PASS\n");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_input_ascii_char();
    test_input_multiple_ascii_chars();
    test_input_space_char();
    test_input_up_arrow();
    test_input_down_arrow();
    test_input_right_arrow();
    test_input_left_arrow();
    test_input_home_key();
    test_input_end_key();
    test_input_enter_key();
    test_input_backspace();
    test_input_tab();
    test_input_ctrl_c();
    test_input_ctrl_a();
    test_input_utf8_two_byte();
    test_input_utf8_three_byte();
    test_input_utf8_four_byte();
    test_input_mouse_left_press();
    test_input_mouse_left_release();
    test_input_mouse_right_button();
    test_input_mouse_scroll_up();
    test_input_focus_gained();
    test_input_focus_lost();
    test_input_bracketed_paste();
    test_input_page_up();
    test_input_page_down();
    test_input_delete_key();
    test_input_f1_key();
    test_input_mixed_sequence();
    test_input_has_pending_false_after_complete();
    test_input_reset_clears_state();
    std::println("=== ALL 31 TESTS PASSED ===");
}
