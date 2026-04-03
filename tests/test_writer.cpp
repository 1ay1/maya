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
    std::println("=== ALL 8 TESTS PASSED ===");
}
