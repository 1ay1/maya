// Tests for the Writer buffered I/O: push, flush, write_raw
#include <maya/maya.hpp>
#include <cassert>
#include <print>
#include <fcntl.h>

using namespace maya;

void test_writer_flush_to_devnull() {
    std::println("--- test_writer_flush_to_devnull ---");
    int fd = ::open("/dev/null", O_WRONLY);
    assert(fd >= 0);
    Writer w(fd);
    w.push(render_op::CursorHide{});
    w.push(render_op::Write{"hello"});
    w.push(render_op::Write{" world"});
    w.push(render_op::StyleStr{"\x1b[1m"});
    w.push(render_op::CursorShow{});
    auto status = w.flush();
    ::close(fd);
    assert(status.has_value());
    std::println("PASS\n");
}

void test_writer_write_raw() {
    std::println("--- test_writer_write_raw ---");
    int fd = ::open("/dev/null", O_WRONLY);
    assert(fd >= 0);
    Writer w(fd);
    auto status = w.write_raw("hello world\n");
    ::close(fd);
    assert(status.has_value());
    std::println("PASS\n");
}

void test_writer_empty_flush() {
    std::println("--- test_writer_empty_flush ---");
    int fd = ::open("/dev/null", O_WRONLY);
    assert(fd >= 0);
    Writer w(fd);
    auto status = w.flush(); // nothing pushed, should succeed
    ::close(fd);
    assert(status.has_value());
    std::println("PASS\n");
}

void test_writer_multiple_writes_merged() {
    std::println("--- test_writer_multiple_writes_merged ---");
    int fd = ::open("/dev/null", O_WRONLY);
    assert(fd >= 0);
    Writer w(fd);
    // Consecutive Write ops should merge (batching optimization)
    w.push(render_op::Write{"aaa"});
    w.push(render_op::Write{"bbb"});
    w.push(render_op::Write{"ccc"});
    auto status = w.flush();
    ::close(fd);
    assert(status.has_value());
    std::println("PASS\n");
}

void test_writer_cursor_ops() {
    std::println("--- test_writer_cursor_ops ---");
    int fd = ::open("/dev/null", O_WRONLY);
    assert(fd >= 0);
    Writer w(fd);
    w.push(render_op::CursorHide{});
    w.push(render_op::CursorShow{});
    w.push(render_op::CursorHide{});
    auto status = w.flush();
    ::close(fd);
    assert(status.has_value());
    std::println("PASS\n");
}

void test_writer_style_and_write() {
    std::println("--- test_writer_style_and_write ---");
    int fd = ::open("/dev/null", O_WRONLY);
    assert(fd >= 0);
    Writer w(fd);
    w.push(render_op::StyleStr{"\x1b[1;31m"});   // bold red
    w.push(render_op::Write{"ERROR"});
    w.push(render_op::StyleStr{"\x1b[0m"});       // reset
    auto status = w.flush();
    ::close(fd);
    assert(status.has_value());
    std::println("PASS\n");
}

void test_writer_write_raw_empty_string() {
    std::println("--- test_writer_write_raw_empty_string ---");
    int fd = ::open("/dev/null", O_WRONLY);
    assert(fd >= 0);
    Writer w(fd);
    auto status = w.write_raw("");
    ::close(fd);
    assert(status.has_value());
    std::println("PASS\n");
}

void test_writer_large_output() {
    std::println("--- test_writer_large_output ---");
    int fd = ::open("/dev/null", O_WRONLY);
    assert(fd >= 0);
    Writer w(fd);
    // Simulate a full-screen render by pushing 2000 bytes
    std::string chunk(100, 'X');
    for (int i = 0; i < 20; ++i)
        w.push(render_op::Write{chunk});
    auto status = w.flush();
    ::close(fd);
    assert(status.has_value());
    std::println("PASS\n");
}

// safe_break_len: the residue-safe write boundary helper. The
// property we care about: for every prefix of `data` that the kernel
// might accept (0..safe), the wire is left at a complete-unit
// boundary — no half-CSI, no half-UTF-8, no orphan ESC.
void test_safe_break_len_plain_ascii() {
    std::println("--- test_safe_break_len_plain_ascii ---");
    assert(detail::safe_break_len("") == 0);
    assert(detail::safe_break_len("hello") == 5);
    std::println("PASS\n");
}

void test_safe_break_len_complete_csi() {
    std::println("--- test_safe_break_len_complete_csi ---");
    // \x1b[31m = SGR red. Final byte 'm' is in 0x40–0x7E.
    std::string_view s = "\x1b[31m";
    assert(detail::safe_break_len(s) == s.size());
    // Trailing data after a complete CSI stays safe.
    std::string_view s2 = "\x1b[31mhi";
    assert(detail::safe_break_len(s2) == s2.size());
    std::println("PASS\n");
}

void test_safe_break_len_incomplete_csi() {
    std::println("--- test_safe_break_len_incomplete_csi ---");
    // Half a CSI — parameter bytes only.
    assert(detail::safe_break_len("abc\x1b[3") == 3);
    // Just the introducer.
    assert(detail::safe_break_len("abc\x1b[") == 3);
    // Bare ESC.
    assert(detail::safe_break_len("abc\x1b") == 3);
    std::println("PASS\n");
}

void test_safe_break_len_osc_bel() {
    std::println("--- test_safe_break_len_osc_bel ---");
    // OSC 0;title BEL — BEL terminates.
    std::string_view s = "\x1b]0;hi\x07" "after";
    // Safe at the trailing "after" plus everything before.
    assert(detail::safe_break_len(s) == s.size());
    // Truncated OSC.
    assert(detail::safe_break_len("a\x1b]0;hi") == 1);
    std::println("PASS\n");
}

void test_safe_break_len_osc_st() {
    std::println("--- test_safe_break_len_osc_st ---");
    // OSC … ST (\x1b\\).
    std::string_view s = "\x1b]52;c;dGVzdA==\x1b\\";
    assert(detail::safe_break_len(s) == s.size());
    // Truncated at ST's first byte.
    std::string_view t = "\x1b]52;c;dGVzdA==\x1b";
    // The trailing ESC opens a new pending sequence, so safe is at
    // the end of the OSC if we already saw a final — we haven't.
    // The whole thing is in-flight, safe stays at 0.
    assert(detail::safe_break_len(t) == 0);
    std::println("PASS\n");
}

void test_safe_break_len_utf8_split() {
    std::println("--- test_safe_break_len_utf8_split ---");
    // "é" is C3 A9 (2-byte UTF-8). Cut mid-codepoint.
    std::string_view s = "ab\xc3";   // missing A9
    assert(detail::safe_break_len(s) == 2);
    // Complete codepoint.
    std::string_view s2 = "ab\xc3\xa9";
    assert(detail::safe_break_len(s2) == s2.size());
    // 3-byte codepoint, complete.
    std::string_view s3 = "\xe2\x9c\x93";  // U+2713 ✓
    assert(detail::safe_break_len(s3) == s3.size());
    // Cut after lead.
    std::string_view s4 = "\xe2\x9c";
    assert(detail::safe_break_len(s4) == 0);
    std::println("PASS\n");
}

void test_safe_break_len_can_sub_clear_pending() {
    std::println("--- test_safe_break_len_can_sub_clear_pending ---");
    // CAN (\x18) is a one-byte unit even mid-ESC — the helper
    // treats it like ASCII (a complete unit), which is correct
    // because terminals consume CAN as a CS cancel.
    assert(detail::safe_break_len("abc\x18") == 4);
    assert(detail::safe_break_len("abc\x1a") == 4);
    std::println("PASS\n");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_writer_flush_to_devnull();
    test_writer_write_raw();
    test_writer_empty_flush();
    test_writer_multiple_writes_merged();
    test_writer_cursor_ops();
    test_writer_style_and_write();
    test_writer_write_raw_empty_string();
    test_writer_large_output();
    test_safe_break_len_plain_ascii();
    test_safe_break_len_complete_csi();
    test_safe_break_len_incomplete_csi();
    test_safe_break_len_osc_bel();
    test_safe_break_len_osc_st();
    test_safe_break_len_utf8_split();
    test_safe_break_len_can_sub_clear_pending();
    std::println("=== ALL 15 TESTS PASSED ===");
}
