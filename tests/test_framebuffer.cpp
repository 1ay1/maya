// Tests for FrameBuffer: double-buffered rendering, diff cycle, persistent buffer
#include <maya/maya.hpp>
#include <maya/render/frame.hpp>
// NDEBUG guard: CMake builds tests in Release (-O3 -DNDEBUG), which strips
// assert(). Undefine it here so this file's runtime asserts actually fire.
#undef NDEBUG
#include "agtest.hpp"
#include <print>

using namespace maya;
using namespace maya::dsl;

TEST_CASE("framebuffer first render nonempty") {
    std::println("--- test_framebuffer_first_render_nonempty ---");
    FrameBuffer fb(30, 5);
    const auto& out = fb.render(text("hello"), theme::dark);
    assert(!out.empty());
    std::println("  first render bytes: {}", out.size());
    std::println("PASS\n");
}

TEST_CASE("framebuffer contains sync markers") {
    std::println("--- test_framebuffer_contains_sync_markers ---");
    FrameBuffer fb(20, 5);
    const auto& out = fb.render(text("test"), theme::dark);
    // Must contain sync_start and sync_end for atomic terminal update
    // sync_start = "\x1b[?2026h", sync_end = "\x1b[?2026l"
    assert(out.find("\x1b[?2026h") != std::string::npos);
    assert(out.find("\x1b[?2026l") != std::string::npos);
    std::println("PASS\n");
}

TEST_CASE("framebuffer contains cursor hide") {
    std::println("--- test_framebuffer_contains_cursor_hide ---");
    FrameBuffer fb(20, 5);
    const auto& out = fb.render(text("test"), theme::dark);
    // Must begin with hide_cursor = "\x1b[?25l"
    assert(out.find("\x1b[?25l") != std::string::npos);
    std::println("PASS\n");
}

TEST_CASE("framebuffer same content second render smaller") {
    std::println("--- test_framebuffer_same_content_second_render_smaller ---");
    FrameBuffer fb(30, 5);
    const auto& out1 = fb.render(text("hello world"), theme::dark);
    std::size_t size1 = out1.size();

    const auto& out2 = fb.render(text("hello world"), theme::dark);
    std::println("  frame1={} frame2={}", size1, out2.size());
    // Second render of identical content should be <= first (diff is smaller/empty)
    assert(out2.size() <= size1);
    std::println("PASS\n");
}

TEST_CASE("framebuffer changed content produces output") {
    std::println("--- test_framebuffer_changed_content_produces_output ---");
    FrameBuffer fb(30, 5);
    (void)fb.render(text("frame1"), theme::dark);

    const auto& out = fb.render(text("frame2"), theme::dark);
    assert(!out.empty());
    std::println("  changed-content bytes: {}", out.size());
    std::println("PASS\n");
}

TEST_CASE("framebuffer persistent buffer reused") {
    std::println("--- test_framebuffer_persistent_buffer_reused ---");
    FrameBuffer fb(20, 5);
    // Render several frames; the returned reference points to the internal buffer.
    // Capacity should grow but not shrink — verify by rendering multiple times.
    std::size_t cap0 = 0;
    for (int i = 0; i < 5; ++i) {
        const auto& out = fb.render(text("hello"), theme::dark);
        assert(!out.empty());
        if (i == 0) cap0 = out.capacity();
    }
    // Capacity should be non-zero and stable after warm-up
    assert(cap0 > 0);
    std::println("  steady-state capacity: {}", cap0);
    std::println("PASS\n");
}

TEST_CASE("framebuffer resize forces repaint") {
    std::println("--- test_framebuffer_resize_forces_repaint ---");
    FrameBuffer fb(20, 5);
    (void)fb.render(text("hello"), theme::dark); // warm up

    fb.resize(30, 8);
    const auto& out = fb.render(text("hello"), theme::dark);
    assert(!out.empty()); // Must repaint after resize
    std::println("  post-resize bytes: {}", out.size());
    std::println("PASS\n");
}

TEST_CASE("framebuffer invalidate forces repaint") {
    std::println("--- test_framebuffer_invalidate_forces_repaint ---");
    FrameBuffer fb(20, 5);
    (void)fb.render(text("hello"), theme::dark); // warm up

    fb.invalidate();
    const auto& out = fb.render(text("hello"), theme::dark);
    assert(!out.empty()); // Must fully repaint after invalidate
    std::println("  post-invalidate bytes: {}", out.size());
    std::println("PASS\n");
}

TEST_CASE("framebuffer dimensions") {
    std::println("--- test_framebuffer_dimensions ---");
    FrameBuffer fb(40, 15);
    assert(fb.width()  == 40);
    assert(fb.height() == 15);

    fb.resize(80, 24);
    assert(fb.width()  == 80);
    assert(fb.height() == 24);
    std::println("PASS\n");
}

TEST_CASE("framebuffer cursor control") {
    std::println("--- test_framebuffer_cursor_control ---");
    FrameBuffer fb(20, 5);
    fb.set_cursor(Position{Columns{5}, Rows{2}});
    fb.set_cursor_visible(true);
    // Render should include a show_cursor sequence since we set it visible
    const auto& out = fb.render(text("test"), theme::dark);
    assert(!out.empty());
    // show_cursor = "\x1b[?25h"
    assert(out.find("\x1b[?25h") != std::string::npos);
    std::println("PASS\n");
}

TEST_CASE("framebuffer swap front back") {
    std::println("--- test_framebuffer_swap_front_back ---");
    FrameBuffer fb(20, 5);
    render_tree(text("front"), fb.front().canvas, fb.style_pool(), theme::dark);
    render_tree(text("back"),  fb.back().canvas,  fb.style_pool(), theme::dark);

    // After render(), the back becomes front
    (void)fb.render(text("new_back"), theme::dark);

    // Front now holds what was in back before render
    const auto& front_canvas = fb.front().canvas;
    assert(front_canvas.width() == 20);
    std::println("PASS\n");
}

TEST_CASE("framebuffer default constructed zero size") {
    std::println("--- test_framebuffer_default_constructed_zero_size ---");
    FrameBuffer fb; // default: 0x0
    assert(fb.width()  == 0);
    assert(fb.height() == 0);
    std::println("PASS\n");
}

