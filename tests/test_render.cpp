// Headless render test - verifies layout + paint produce correct canvas output
#include <maya/maya.hpp>
#include <maya/render/frame.hpp>
#include <cassert>
#include <print>
#include <string>

using namespace maya;
using namespace maya::detail;
using namespace maya::dsl;

std::string get_row(const Canvas& canvas, int y) {
    std::string s;
    for (int x = 0; x < canvas.width(); ++x) {
        Cell c = canvas.get(x, y);
        if (c.character >= 0x20 && c.character < 0x7F)
            s += static_cast<char>(c.character);
        else if (c.character >= 0x2500)
            s += '#';
        else
            s += ' ';
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

void dump(const Canvas& canvas, int rows = -1) {
    if (rows < 0) rows = canvas.height();
    for (int y = 0; y < rows && y < canvas.height(); ++y) {
        std::println("  {:2}|{}|", y, get_row(canvas, y));
    }
}

// ============================================================================
// Test 1: bare text element fills root
// ============================================================================
void test_bare_text() {
    std::println("--- test_bare_text ---");
    StylePool pool;
    Canvas canvas(30, 3, &pool);
    render_tree(text("hello"), canvas, pool, theme::dark);
    dump(canvas);
    assert(get_row(canvas, 0).starts_with("hello"));
    std::println("PASS\n");
}

// ============================================================================
// Test 2: column box with padding
// ============================================================================
void test_column_padding() {
    std::println("--- test_column_padding ---");
    StylePool pool;
    Canvas canvas(30, 8, &pool);
    render_tree(
        box().direction(Column).padding(1)(
            text("AAA"),
            text("BBB")
        ), canvas, pool, theme::dark);
    dump(canvas);
    // padding=1 → content starts at (1,1)
    assert(get_row(canvas, 0).empty()); // top padding row
    assert(get_row(canvas, 1).find("AAA") != std::string::npos);
    assert(get_row(canvas, 2).find("BBB") != std::string::npos);
    std::println("PASS\n");
}

// ============================================================================
// Test 3: row layout places children horizontally
// ============================================================================
void test_row_layout() {
    std::println("--- test_row_layout ---");
    StylePool pool;
    Canvas canvas(30, 3, &pool);
    render_tree(
        box().direction(Row)(
            text("LEFT"),
            text("RIGHT")
        ), canvas, pool, theme::dark);
    dump(canvas);
    std::string r = get_row(canvas, 0);
    auto lpos = r.find("LEFT");
    auto rpos = r.find("RIGHT");
    std::println("  LEFT@{} RIGHT@{}", lpos == std::string::npos ? -1 : (int)lpos,
                 rpos == std::string::npos ? -1 : (int)rpos);
    assert(lpos != std::string::npos);
    assert(rpos != std::string::npos);
    assert(rpos > lpos);
    std::println("PASS\n");
}

// ============================================================================
// Test 4: flex-grow spacer pushes content to bottom
// ============================================================================
void test_spacer() {
    std::println("--- test_spacer ---");
    StylePool pool;
    Canvas canvas(20, 10, &pool);
    render_tree(
        box().direction(Column)(
            text("TOP"),
            spacer(),
            text("BOT")
        ), canvas, pool, theme::dark);
    dump(canvas);
    assert(get_row(canvas, 0).find("TOP") != std::string::npos);
    assert(get_row(canvas, 9).find("BOT") != std::string::npos);
    std::println("PASS\n");
}

// ============================================================================
// Test 5: border draws box-drawing characters
// ============================================================================
void test_border() {
    std::println("--- test_border ---");
    StylePool pool;
    Canvas canvas(20, 5, &pool);
    render_tree(
        box().direction(Column).border(BorderStyle::Single)(
            text("hi")
        ), canvas, pool, theme::dark);
    dump(canvas);
    Cell tl = canvas.get(0, 0);
    std::println("  corner U+{:04X}", (uint32_t)tl.character);
    assert(tl.character != U' '); // should be a box-drawing char
    // "hi" should be inside the border at (1,1)
    assert(get_row(canvas, 1).find("hi") != std::string::npos);
    std::println("PASS\n");
}

// ============================================================================
// Test 6: nested boxes
// ============================================================================
void test_nested() {
    std::println("--- test_nested ---");
    StylePool pool;
    Canvas canvas(40, 10, &pool);
    render_tree(
        box().direction(Column).padding(1)(
            text("title"),
            box().direction(Row).gap(1)(
                text("A"),
                text("B")
            )
        ), canvas, pool, theme::dark);
    dump(canvas, 5);
    assert(get_row(canvas, 1).find("title") != std::string::npos);
    std::string r2 = get_row(canvas, 2);
    assert(r2.find("A") != std::string::npos);
    assert(r2.find("B") != std::string::npos);
    std::println("PASS\n");
}

// ============================================================================
// Test 7: counter example element tree
// ============================================================================
void test_counter_tree() {
    std::println("--- test_counter_tree ---");
    StylePool pool;
    Canvas canvas(40, 10, &pool);
    render_tree(
        box().direction(Column).padding(1)(
            text("Counter: 0", bold_style),
            text("[+/-] change  [q] quit", dim_style)
        ), canvas, pool, theme::dark);
    dump(canvas, 5);
    assert(get_row(canvas, 1).find("Counter: 0") != std::string::npos);
    assert(get_row(canvas, 2).find("[+/-]") != std::string::npos);
    std::println("PASS\n");
}

// ============================================================================
// Test 8: diff produces ops for changed cells
// ============================================================================
void test_diff() {
    std::println("--- test_diff ---");
    StylePool pool;
    Canvas old_canvas(20, 3, &pool);
    Canvas new_canvas(20, 3, &pool);

    // Write different content to each
    render_tree(text("before"), old_canvas, pool, theme::dark);
    render_tree(text("after"), new_canvas, pool, theme::dark);

    // Verify cells are actually different
    std::println("  old[0]={:c} new[0]={:c}",
        (char)old_canvas.get(0, 0).character,
        (char)new_canvas.get(0, 0).character);
    std::println("  old packed={:#x} new packed={:#x}",
        old_canvas.get_packed(0, 0), new_canvas.get_packed(0, 0));
    auto dmg = new_canvas.damage();
    std::println("  damage: ({},{}) {}x{}",
        dmg.pos.x.value, dmg.pos.y.value,
        dmg.size.width.value, dmg.size.height.value);

    std::string result;
    diff(old_canvas, new_canvas, pool, result);
    std::println("  diff bytes: {}", result.size());
    assert(!result.empty());
    std::println("PASS\n");
}

// ============================================================================
// Test 9: FrameBuffer render + diff cycle
// ============================================================================
void test_framebuffer() {
    std::println("--- test_framebuffer ---");
    FrameBuffer fb(30, 5);

    // First frame: full repaint
    const auto& out1 = fb.render(text("frame1"), theme::dark);
    std::println("  frame1 bytes: {}", out1.size());
    assert(!out1.empty());

    // Second frame: same content → should produce fewer bytes
    const auto& out2 = fb.render(text("frame1"), theme::dark);
    std::println("  frame2 (same) bytes: {}", out2.size());

    // Third frame: different content → should produce output
    const auto& out3 = fb.render(text("frame2"), theme::dark);
    std::println("  frame3 (changed) bytes: {}", out3.size());
    assert(!out3.empty());
    std::println("PASS\n");
}

// ============================================================================
// Test 10: input parser basics
// ============================================================================
void test_input_parser() {
    std::println("--- test_input_parser ---");
    InputParser parser;

    // Regular character
    auto events = parser.feed("a");
    std::println("  'a' -> {} events", events.size());
    assert(!events.empty());
    auto* ke = std::get_if<KeyEvent>(&events[0]);
    assert(ke != nullptr);
    auto* ck = std::get_if<CharKey>(&ke->key);
    assert(ck != nullptr && ck->codepoint == 'a');

    // Escape sequence for arrow up: ESC [ A
    events = parser.feed("\x1b[A");
    std::println("  ESC[A -> {} events", events.size());
    if (!events.empty()) {
        ke = std::get_if<KeyEvent>(&events[0]);
        if (ke) {
            auto* sk = std::get_if<SpecialKey>(&ke->key);
            if (sk) std::println("  special key: {}", static_cast<int>(*sk));
        }
    }

    std::println("PASS\n");
}

// ============================================================================
// Test 11: signal reactivity
// ============================================================================
void test_signal() {
    std::println("--- test_signal ---");
    Signal<int> count{0};
    assert(count.get() == 0);

    count.set(5);
    assert(count.get() == 5);

    count.update([](int& n) { n += 10; });
    assert(count.get() == 15);

    // Computed
    auto doubled = computed([&] { return count.get() * 2; });
    assert(doubled.get() == 30);

    count.set(3);
    assert(doubled.get() == 6);

    std::println("  count={} doubled={}", count.get(), doubled.get());
    std::println("PASS\n");
}

// ============================================================================
// Test 12: style SGR generation
// ============================================================================
void test_style_sgr() {
    std::println("--- test_style_sgr ---");
    auto s = Style{}.with_bold().with_fg(Color::red());
    std::string sgr = s.to_sgr();
    std::println("  bold+red: '{}'", sgr);
    assert(sgr.find("1") != std::string::npos);  // bold = SGR 1
    assert(sgr.find("31") != std::string::npos);  // red fg = SGR 31

    auto empty_sgr = Style{}.to_sgr();
    assert(empty_sgr.empty());

    std::println("PASS\n");
}

// ============================================================================
// Test 13: StyleApplier transition
// ============================================================================
void test_style_transition() {
    std::println("--- test_style_transition ---");
    Style a = Style{}.with_bold();
    Style b = Style{}.with_bold().with_fg(Color::green());
    std::string t = ansi::StyleApplier::transition(a, b);
    std::println("  bold -> bold+green: '{}'", t);
    assert(!t.empty()); // should emit green fg
    assert(t.find("32") != std::string::npos); // green = 32

    // Same style -> empty
    std::string same = ansi::StyleApplier::transition(a, a);
    assert(same.empty());
    std::println("PASS\n");
}

// ============================================================================
// Test 14: writer batches and flushes
// ============================================================================
void test_writer() {
    std::println("--- test_writer ---");
    // Write to /dev/null to test the pipeline doesn't crash
    int fd = ::open("/dev/null", O_WRONLY);
    assert(fd >= 0);
    Writer w(fd);
    w.push(render_op::CursorHide{});
    w.push(render_op::Write{"hello"});
    w.push(render_op::Write{" world"});  // should merge with previous
    w.push(render_op::StyleStr{"\x1b[1m"});
    w.push(render_op::CursorShow{});
    auto status = w.flush();
    ::close(fd);
    assert(status.has_value());
    std::println("PASS\n");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_bare_text();
    test_column_padding();
    test_row_layout();
    test_spacer();
    test_border();
    test_nested();
    test_counter_tree();
    test_diff();
    test_framebuffer();
    test_input_parser();
    test_signal();
    test_style_sgr();
    test_style_transition();
    test_writer();

    std::println("=== ALL {} TESTS PASSED ===", 14);
}
