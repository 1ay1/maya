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

void test_input_osc52_clipboard_text_st() {
    std::println("--- test_input_osc52_clipboard_text_st ---");
    InputParser p;
    // OSC 52 ; c ; base64("hi") ST  -> "hi" = aGk=
    auto events = p.feed("\x1b]52;c;aGk=\x1b\\");
    assert(events.size() == 1);
    const auto* pe = std::get_if<PasteEvent>(&events[0]);
    assert(pe != nullptr);
    assert(pe->content == "hi");
    std::println("PASS\n");
}

void test_input_osc52_clipboard_text_bel() {
    std::println("--- test_input_osc52_clipboard_text_bel ---");
    InputParser p;
    // BEL-terminated form. base64("maya") = bWF5YQ==
    auto events = p.feed("\x1b]52;c;bWF5YQ==\x07");
    assert(events.size() == 1);
    const auto* pe = std::get_if<PasteEvent>(&events[0]);
    assert(pe != nullptr);
    assert(pe->content == "maya");
    std::println("PASS\n");
}

void test_input_osc52_clipboard_binary() {
    std::println("--- test_input_osc52_clipboard_binary ---");
    InputParser p;
    // The PNG magic prefix 89 50 4E 47 0D 0A 1A 0A base64s to
    // iVBORw0KGgo= — the exact bytes agentty's image sniff needs.
    auto events = p.feed("\x1b]52;c;iVBORw0KGgo=\x1b\\");
    assert(events.size() == 1);
    const auto* pe = std::get_if<PasteEvent>(&events[0]);
    assert(pe != nullptr);
    const std::string& b = pe->content;
    assert(b.size() == 8);
    assert(static_cast<unsigned char>(b[0]) == 0x89);
    assert(static_cast<unsigned char>(b[1]) == 0x50);
    assert(static_cast<unsigned char>(b[2]) == 0x4E);
    assert(static_cast<unsigned char>(b[3]) == 0x47);
    std::println("PASS\n");
}

void test_input_osc52_empty_payload_ignored() {
    std::println("--- test_input_osc52_empty_payload_ignored ---");
    InputParser p;
    // Empty payload (clipboard empty) and the "?" refusal both yield
    // NO event — never an empty paste that could loop the host.
    auto e1 = p.feed("\x1b]52;c;\x1b\\");
    assert(e1.empty());
    auto e2 = p.feed("\x1b]52;c;?\x1b\\");
    assert(e2.empty());
    std::println("PASS\n");
}

void test_input_osc_non52_discarded() {
    std::println("--- test_input_osc_non52_discarded ---");
    InputParser p;
    // OSC 0 (set title) and OSC 11 (bg color report) must stay
    // unhandled — no spurious paste.
    auto e1 = p.feed("\x1b]0;my title\x07");
    assert(e1.empty());
    auto e2 = p.feed("\x1b]11;rgb:0000/0000/0000\x1b\\");
    assert(e2.empty());
    std::println("PASS\n");
}

void test_input_osc52_split_feed() {
    std::println("--- test_input_osc52_split_feed ---");
    InputParser p;
    // The reply can arrive across multiple read() chunks (SSH / slow
    // pty). The FSM must accumulate across feed() calls. base64("ok")
    // = b2s=.
    auto e0 = p.feed("\x1b]52;c;b2");
    assert(e0.empty());
    auto e1 = p.feed("s=\x1b\\");
    assert(e1.size() == 1);
    const auto* pe = std::get_if<PasteEvent>(&e1[0]);
    assert(pe != nullptr && pe->content == "ok");
    std::println("PASS\n");
}

// ── OSC 5522 (kitty clipboard protocol) read replies ──────────────────

void test_input_osc5522_image_reassembly() {
    std::println("--- test_input_osc5522_image_reassembly ---");
    InputParser p;
    // OK → two PNG chunks → trailing text/plain (must be skipped, the
    // image outranks it) → DONE. mime b64: image/png = aW1hZ2UvcG5n,
    // text/plain = dGV4dC9wbGFpbg==. PNG magic prefix iVBORw0KGgo=
    // split as iVBORw0K (89 50 4E 47 0D 0A) + Ggo= (1A 0A).
    std::string s;
    s += "\x1b]5522;type=read:status=OK\x1b\\";
    s += "\x1b]5522;type=read:status=DATA:mime=aW1hZ2UvcG5n;iVBORw0K\x1b\\";
    s += "\x1b]5522;type=read:status=DATA:mime=aW1hZ2UvcG5n;Ggo=\x1b\\";
    s += "\x1b]5522;type=read:status=DATA:mime=dGV4dC9wbGFpbg==;aGVsbG8=\x1b\\";
    s += "\x1b]5522;type=read:status=DONE\x1b\\";
    auto events = p.feed(s);
    assert(events.size() == 1);
    const auto* pe = std::get_if<PasteEvent>(&events[0]);
    assert(pe != nullptr);
    const std::string& b = pe->content;
    assert(b.size() == 8);
    assert(static_cast<unsigned char>(b[0]) == 0x89);
    assert(static_cast<unsigned char>(b[1]) == 0x50);
    assert(static_cast<unsigned char>(b[6]) == 0x1A);
    assert(static_cast<unsigned char>(b[7]) == 0x0A);
    std::println("PASS\n");
}

void test_input_osc5522_text_only() {
    std::println("--- test_input_osc5522_text_only ---");
    InputParser p;
    std::string s;
    s += "\x1b]5522;type=read:status=OK\x1b\\";
    s += "\x1b]5522;type=read:status=DATA:mime=dGV4dC9wbGFpbg==;aGVsbG8=\x1b\\";
    s += "\x1b]5522;type=read:status=DONE\x1b\\";
    auto events = p.feed(s);
    assert(events.size() == 1);
    const auto* pe = std::get_if<PasteEvent>(&events[0]);
    assert(pe != nullptr && pe->content == "hello");
    std::println("PASS\n");
}

void test_input_osc5522_image_outranks_earlier_text() {
    std::println("--- test_input_osc5522_image_outranks_earlier_text ---");
    InputParser p;
    // text arrives FIRST — a terminal that ignores our request order.
    // The later image/png must replace the accumulated text.
    std::string s;
    s += "\x1b]5522;type=read:status=OK\x1b\\";
    s += "\x1b]5522;type=read:status=DATA:mime=dGV4dC9wbGFpbg==;aGVsbG8=\x1b\\";
    s += "\x1b]5522;type=read:status=DATA:mime=aW1hZ2UvcG5n;iVBORw0KGgo=\x1b\\";
    s += "\x1b]5522;type=read:status=DONE\x1b\\";
    auto events = p.feed(s);
    assert(events.size() == 1);
    const auto* pe = std::get_if<PasteEvent>(&events[0]);
    assert(pe != nullptr && pe->content.size() == 8);
    assert(static_cast<unsigned char>(pe->content[0]) == 0x89);
    std::println("PASS\n");
}

void test_input_osc5522_error_aborts_then_recovers() {
    std::println("--- test_input_osc5522_error_aborts_then_recovers ---");
    InputParser p;
    // EPERM mid-transfer → no event, and a following clean transfer
    // must succeed (state fully reset).
    std::string s;
    s += "\x1b]5522;type=read:status=OK\x1b\\";
    s += "\x1b]5522;type=read:status=DATA:mime=aW1hZ2UvcG5n;iVBORw0K\x1b\\";
    s += "\x1b]5522;type=read:status=EPERM\x1b\\";
    auto e1 = p.feed(s);
    assert(e1.empty());
    std::string s2;
    s2 += "\x1b]5522;type=read:status=OK\x1b\\";
    s2 += "\x1b]5522;type=read:status=DATA:mime=dGV4dC9wbGFpbg==;b2s=\x1b\\";
    s2 += "\x1b]5522;type=read:status=DONE\x1b\\";
    auto e2 = p.feed(s2);
    assert(e2.size() == 1);
    const auto* pe = std::get_if<PasteEvent>(&e2[0]);
    assert(pe != nullptr && pe->content == "ok");
    std::println("PASS\n");
}

void test_input_osc5522_stray_packets_dropped() {
    std::println("--- test_input_osc5522_stray_packets_dropped ---");
    InputParser p;
    // DATA/DONE with no preceding OK (another client's leak through a
    // multiplexer) must yield nothing.
    auto e1 = p.feed("\x1b]5522;type=read:status=DATA:mime=dGV4dC9wbGFpbg==;b2s=\x1b\\");
    assert(e1.empty());
    auto e2 = p.feed("\x1b]5522;type=read:status=DONE\x1b\\");
    assert(e2.empty());
    // Write acks are not read replies — ignored even mid-ground.
    auto e3 = p.feed("\x1b]5522;type=write:status=DONE\x1b\\");
    assert(e3.empty());
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
    test_input_osc52_clipboard_text_st();
    test_input_osc52_clipboard_text_bel();
    test_input_osc52_clipboard_binary();
    test_input_osc52_empty_payload_ignored();
    test_input_osc_non52_discarded();
    test_input_osc52_split_feed();
    test_input_osc5522_image_reassembly();
    test_input_osc5522_text_only();
    test_input_osc5522_image_outranks_earlier_text();
    test_input_osc5522_error_aborts_then_recovers();
    test_input_osc5522_stray_packets_dropped();
    test_input_page_up();
    test_input_page_down();
    test_input_delete_key();
    test_input_f1_key();
    test_input_mixed_sequence();
    test_input_has_pending_false_after_complete();
    test_input_reset_clears_state();
    std::println("=== ALL 42 TESTS PASSED ===");
}
