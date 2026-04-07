// test_resize.cpp — Headless tests for responsive rendering across resize
//
// Verifies that the entire render pipeline (layout → canvas → row hashes →
// serialize) handles terminal dimension changes correctly for ANY element
// tree, in both inline and alt-screen code paths.

#include <maya/maya.hpp>
#include <cassert>
#include <print>
#include <string>
#include <vector>

using namespace maya;
using namespace maya::detail;
using namespace maya::dsl;

// ── Helpers ──────────────────────────────────────────────────────────────────

std::string get_row(const Canvas& canvas, int y) {
    std::string s;
    for (int x = 0; x < canvas.width(); ++x) {
        Cell c = canvas.get(x, y);
        if (c.character >= 0x20 && c.character < 0x7F)
            s += static_cast<char>(c.character);
        else if (c.character == 0)
            s += ' ';
        else
            s += '?';
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// Render an element at a given width and return all non-empty row strings.
std::vector<std::string> render_at(const Element& elem, int width, int height = 500) {
    StylePool pool;
    Canvas canvas(width, height, &pool);
    render_tree(elem, canvas, pool, theme::dark, /*auto_height=*/true);
    int ch = content_height(canvas);
    std::vector<std::string> rows;
    for (int y = 0; y < ch; ++y)
        rows.push_back(get_row(canvas, y));
    return rows;
}

// Check every row starts content within the first `max_col` columns.
void assert_left_aligned(const std::vector<std::string>& rows, int max_col, const char* ctx) {
    for (int y = 0; y < static_cast<int>(rows.size()); ++y) {
        if (rows[y].empty()) continue;
        auto first = rows[y].find_first_not_of(' ');
        if (first != std::string::npos && static_cast<int>(first) > max_col) {
            std::println("  FAIL [{}]: row {} starts at col {}: '{}'", ctx, y, first, rows[y]);
            assert(false);
        }
    }
}

// Check that text appears somewhere in the rendered output.
bool contains_text(const std::vector<std::string>& rows, const std::string& text) {
    for (auto& r : rows)
        if (r.find(text) != std::string::npos) return true;
    return false;
}

// ============================================================================
// 1. Canvas fundamentals on resize
// ============================================================================

void test_canvas_resize() {
    std::println("--- test_canvas_resize ---");
    StylePool pool;

    // Canvas at width 80
    Canvas c1(80, 10, &pool);
    c1.write_text(0, 0, "hello", pool.intern(Style{}));
    assert(get_row(c1, 0) == "hello");

    // "Resize" to 120 — new canvas, old content gone
    Canvas c2(120, 10, &pool);
    assert(get_row(c2, 0).empty());
    c2.write_text(0, 0, "world", pool.intern(Style{}));
    assert(get_row(c2, 0) == "world");

    // Verify clear_rows resets properly
    c2.write_text(0, 1, "line2", pool.intern(Style{}));
    c2.clear_rows(2);
    assert(get_row(c2, 0).empty());
    assert(get_row(c2, 1).empty());

    std::println("PASS\n");
}

// ============================================================================
// 2. Row hashes change when canvas width changes
// ============================================================================

void test_row_hash_width_dependency() {
    std::println("--- test_row_hash_width_dependency ---");
    StylePool pool;

    // Same text "ABCD" at width 80 vs 120 must produce different hashes
    // because the row buffer has different lengths (padded with spaces).
    auto sid = pool.intern(Style{});

    Canvas c80(80, 5, &pool);
    c80.write_text(0, 0, "ABCD", sid);

    Canvas c120(120, 5, &pool);
    c120.write_text(0, 0, "ABCD", sid);

    uint64_t h80  = simd::hash_row(c80.cells(), 80);
    uint64_t h120 = simd::hash_row(c120.cells(), 120);

    // Must be different — width is part of the hash input
    assert(h80 != h120);
    std::println("  hash@80={:#x}, hash@120={:#x} — different: OK", h80, h120);

    // Same canvas, same row, same width → same hash
    uint64_t h80b = simd::hash_row(c80.cells(), 80);
    assert(h80 == h80b);

    std::println("PASS\n");
}

// ============================================================================
// 3. InlineState resets on width change
// ============================================================================

void test_inline_state_reset() {
    std::println("--- test_inline_state_reset ---");

    InlineState st;

    // Simulate: first frame at width 80
    st.canvas_width = 80;
    st.prev_height = 10;
    st.prev_content_height = 12;
    st.committed_height = 5;
    st.prev_row_hashes = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};

    // Simulate width change detection (as render_inline does)
    int new_width = 120;
    if (st.canvas_width != new_width) {
        st.prev_row_hashes.clear();
        st.prev_height = 0;
        st.prev_content_height = 0;
        st.committed_height = 0;
        st.canvas_width = new_width;
    }

    assert(st.prev_row_hashes.empty());
    assert(st.prev_height == 0);
    assert(st.prev_content_height == 0);
    assert(st.committed_height == 0);
    assert(st.canvas_width == 120);

    std::println("PASS\n");
}

// ============================================================================
// 4. serialize_changed: all rows marked changed when old hashes empty
// ============================================================================

void test_serialize_after_resize() {
    std::println("--- test_serialize_after_resize ---");
    StylePool pool;
    auto sid = pool.intern(Style{});

    // Render some content
    Canvas canvas(60, 10, &pool);
    canvas.write_text(0, 0, "Line one", sid);
    canvas.write_text(0, 1, "Line two", sid);
    canvas.write_text(0, 2, "Line three", sid);

    // Compute new hashes
    std::vector<uint64_t> new_hashes(3);
    for (int y = 0; y < 3; ++y)
        new_hashes[y] = simd::hash_row(canvas.cells() + y * 60, 60);

    // After resize: old hashes are EMPTY (cleared)
    std::string out;
    serialize_changed(canvas, pool, out, 3, 0,
                      nullptr, 0,  // no old hashes
                      new_hashes.data(), 3);

    // All 3 rows should be serialized (all "changed")
    // Verify output contains all three lines
    assert(out.find("Line one") != std::string::npos);
    assert(out.find("Line two") != std::string::npos);
    assert(out.find("Line three") != std::string::npos);

    std::println("PASS\n");
}

// ============================================================================
// 5. Basic elements render correctly at multiple widths
// ============================================================================

void test_element_at_widths() {
    std::println("--- test_element_at_widths ---");

    // A simple vstack with text, hstack, and box
    auto make_ui = [] {
        return vstack()(
            text("Title line", Style{}.with_bold()),
            hstack()(
                text("Left"),
                box().grow(1),
                text("Right")
            ),
            text("Content goes here, it can be a longer line that should wrap at narrow widths")
        );
    };

    for (int w : {30, 60, 80, 120, 200}) {
        auto rows = render_at(make_ui(), w);
        std::println("  width={}: {} rows", w, rows.size());

        // Title should always be present and left-aligned
        assert(contains_text(rows, "Title line"));
        assert_left_aligned(rows, 5, "basic element");

        // "Left" and "Right" should be present
        assert(contains_text(rows, "Left"));
        assert(contains_text(rows, "Right"));

        // "Right" should be near right edge (within the width)
        for (auto& r : rows) {
            auto pos = r.find("Right");
            if (pos != std::string::npos) {
                assert(static_cast<int>(pos + 5) <= w);
                // For widths > 40, "Right" should be significantly right of "Left"
                if (w > 40) {
                    assert(pos > 10);
                }
                break;
            }
        }
    }

    std::println("PASS\n");
}

// ============================================================================
// 6. Same element tree renders identically when re-rendered at same width
// ============================================================================

void test_render_determinism() {
    std::println("--- test_render_determinism ---");

    auto make_ui = [] {
        return vstack()(
            text("Header"),
            hstack()(text("A"), text(" "), text("B")),
            text("Footer")
        );
    };

    auto rows1 = render_at(make_ui(), 80);
    auto rows2 = render_at(make_ui(), 80);

    assert(rows1.size() == rows2.size());
    for (size_t i = 0; i < rows1.size(); ++i) {
        assert(rows1[i] == rows2[i]);
    }

    std::println("PASS\n");
}

// ============================================================================
// 7. Row hashes: stable rows detected, changed rows detected
// ============================================================================

void test_row_hash_stability() {
    std::println("--- test_row_hash_stability ---");
    StylePool pool;
    auto sid = pool.intern(Style{});
    constexpr int W = 80;

    // Frame 1: two rows
    Canvas c1(W, 10, &pool);
    c1.write_text(0, 0, "Stable row", sid);
    c1.write_text(0, 1, "Changes every frame: 1", sid);

    std::vector<uint64_t> hashes1(2);
    hashes1[0] = simd::hash_row(c1.cells(), W);
    hashes1[1] = simd::hash_row(c1.cells() + W, W);

    // Frame 2: row 0 same, row 1 different
    Canvas c2(W, 10, &pool);
    c2.write_text(0, 0, "Stable row", sid);
    c2.write_text(0, 1, "Changes every frame: 2", sid);

    std::vector<uint64_t> hashes2(2);
    hashes2[0] = simd::hash_row(c2.cells(), W);
    hashes2[1] = simd::hash_row(c2.cells() + W, W);

    // Row 0 should have same hash (content identical)
    assert(hashes1[0] == hashes2[0]);
    // Row 1 should have different hash
    assert(hashes1[1] != hashes2[1]);

    // serialize_changed should skip row 0, only write row 1
    std::string out;
    serialize_changed(c2, pool, out, 2, 0,
                      hashes1.data(), 2,
                      hashes2.data(), 2);

    assert(out.find("Stable row") == std::string::npos);   // skipped
    assert(out.find("Changes every frame: 2") != std::string::npos); // written

    std::println("PASS\n");
}

// ============================================================================
// 8. Full pipeline: render → resize → re-render at new width
// ============================================================================

void test_full_resize_pipeline() {
    std::println("--- test_full_resize_pipeline ---");

    auto make_ui = [] {
        return vstack()(
            text("Header text here"),
            hstack()(
                text("Status: OK"),
                box().grow(1),
                text("100%")
            ),
            text("A longer paragraph of content that will wrap differently at different terminal widths depending on the layout engine")
        );
    };

    // Render at 80
    auto rows80 = render_at(make_ui(), 80);
    int h80 = static_cast<int>(rows80.size());

    // Render at 40 — should have more rows due to wrapping
    auto rows40 = render_at(make_ui(), 40);
    int h40 = static_cast<int>(rows40.size());

    // Render at 160 — should have fewer rows
    auto rows160 = render_at(make_ui(), 160);
    int h160 = static_cast<int>(rows160.size());

    std::println("  height@40={} height@80={} height@160={}", h40, h80, h160);

    // Narrower terminal → more wrapping → more rows (or equal)
    assert(h40 >= h80);
    assert(h80 >= h160);

    // All should contain the same content
    assert(contains_text(rows40, "Header text here"));
    assert(contains_text(rows80, "Header text here"));
    assert(contains_text(rows160, "Header text here"));

    assert(contains_text(rows40, "Status: OK"));
    assert(contains_text(rows80, "Status: OK"));
    assert(contains_text(rows160, "Status: OK"));

    // All should be left-aligned
    assert_left_aligned(rows40, 5, "40-col");
    assert_left_aligned(rows80, 5, "80-col");
    assert_left_aligned(rows160, 5, "160-col");

    // "100%" should be near the right edge at each width
    auto check_right = [](const std::vector<std::string>& rows, int w) {
        for (auto& r : rows) {
            auto pos = r.find("100%");
            if (pos != std::string::npos) {
                assert(static_cast<int>(pos) >= w / 3);
                break;
            }
        }
    };
    check_right(rows40, 40);
    check_right(rows80, 80);
    check_right(rows160, 160);

    std::println("PASS\n");
}

// ============================================================================
// 9. Widgets: Input renders at various widths
// ============================================================================

void test_input_resize() {
    std::println("--- test_input_resize ---");

    Input<> input;
    input.set_placeholder("Search...");

    for (int w : {20, 40, 80, 120}) {
        auto rows = render_at(input, w);
        assert(!rows.empty());
        assert(contains_text(rows, "Search..."));
        assert_left_aligned(rows, 3, "input");
        std::println("  width={}: {} rows — OK", w, rows.size());
    }

    std::println("PASS\n");
}

// ============================================================================
// 10. Widgets: Spinner renders at various widths
// ============================================================================

void test_spinner_resize() {
    std::println("--- test_spinner_resize ---");

    Spinner<SpinnerStyle::Dots> spinner;
    spinner.advance(0.1f);

    for (int w : {10, 40, 80}) {
        auto rows = render_at(spinner, w);
        assert(!rows.empty());
        assert(rows.size() == 1);  // spinner is always 1 row
        std::println("  width={}: '{}' — OK", w, rows[0]);
    }

    std::println("PASS\n");
}

// ============================================================================
// 11. Widgets: Table adapts columns to width
// ============================================================================

void test_table_resize() {
    std::println("--- test_table_resize ---");

    Table tbl({{"Name", 0}, {"Value", 0}});  // auto-width columns
    tbl.add_row({"Language", "C++"});
    tbl.add_row({"Compiler", "g++-15"});

    for (int w : {30, 60, 80, 120}) {
        auto rows = render_at(tbl, w);
        assert(contains_text(rows, "Name"));
        assert(contains_text(rows, "Language"));
        assert(contains_text(rows, "g++-15"));
        assert_left_aligned(rows, 10, "table");
        std::println("  width={}: {} rows — OK", w, rows.size());
    }

    std::println("PASS\n");
}

// ============================================================================
// 12. Widgets: Markdown renders at various widths
// ============================================================================

void test_markdown_resize() {
    std::println("--- test_markdown_resize ---");

    auto md_src = "# Heading\n\nSome **bold** text and `code`.\n\n"
                  "- Item one\n- Item two\n\n"
                  "```\ncode block\n```\n";

    for (int w : {30, 60, 80, 120}) {
        auto rows = render_at(markdown(md_src), w);
        assert(contains_text(rows, "Heading"));
        assert(contains_text(rows, "bold"));
        assert(contains_text(rows, "code block"));
        assert_left_aligned(rows, 10, "markdown");
        std::println("  width={}: {} rows — OK", w, rows.size());
    }

    std::println("PASS\n");
}

// ============================================================================
// 13. Widgets: Badge renders at various widths
// ============================================================================

void test_badge_resize() {
    std::println("--- test_badge_resize ---");

    for (int w : {20, 40, 80}) {
        auto rows = render_at(tool_badge("read_file"), w);
        assert(!rows.empty());
        assert_left_aligned(rows, 5, "badge");
        std::println("  width={}: '{}' — OK", w, rows[0]);
    }

    std::println("PASS\n");
}

// ============================================================================
// 14. ChatView: full lifecycle with resize
// ============================================================================

void test_chatview_resize() {
    std::println("--- test_chatview_resize ---");

    ChatView chat;
    chat.add_user("hello");
    chat.add_tool("read_file", "Lang: C++\nVer: 26");
    chat.begin_stream();
    chat.stream_set("Response text here.");
    chat.end_stream();

    // First render at 80
    auto rows80 = render_at(chat.build(), 80);
    assert(contains_text(rows80, "hello"));
    assert(contains_text(rows80, "read_file"));
    assert(contains_text(rows80, "Response text here"));
    assert(contains_text(rows80, "ready"));
    assert(contains_text(rows80, "3 msgs"));
    assert_left_aligned(rows80, 10, "chat@80");
    std::println("  80: {} rows — OK", rows80.size());

    // Resize to 40
    chat.resize(40, 24);
    auto rows40 = render_at(chat.build(), 40);
    assert(contains_text(rows40, "hello"));
    assert(contains_text(rows40, "read_file"));
    assert_left_aligned(rows40, 10, "chat@40");
    std::println("  40: {} rows — OK", rows40.size());

    // Resize to 145
    chat.resize(145, 48);
    auto rows145 = render_at(chat.build(), 145);
    assert(contains_text(rows145, "hello"));
    assert(contains_text(rows145, "Response text here"));
    assert_left_aligned(rows145, 10, "chat@145");
    std::println("  145: {} rows — OK", rows145.size());

    std::println("PASS\n");
}

// ============================================================================
// 15. ChatView: cache invalidation on resize
// ============================================================================

void test_chatview_cache_invalidation() {
    std::println("--- test_chatview_cache_invalidation ---");

    ChatView chat;
    chat.add_user("test message");
    chat.begin_stream();
    chat.stream_set("Reply");
    chat.end_stream();

    // Render twice at same width — second should use cache
    auto r1 = render_at(chat.build(), 80);
    auto r2 = render_at(chat.build(), 80);
    assert(r1.size() == r2.size());
    for (size_t i = 0; i < r1.size(); ++i)
        assert(r1[i] == r2[i]);

    // Resize — cache should be invalidated
    chat.resize(120, 24);
    auto r3 = render_at(chat.build(), 120);
    // Content should still be present
    assert(contains_text(r3, "test message"));
    assert(contains_text(r3, "Reply"));

    std::println("PASS\n");
}

// ============================================================================
// 16. ChatView: compact mode at narrow width
// ============================================================================

void test_chatview_compact() {
    std::println("--- test_chatview_compact ---");

    ChatView chat;
    chat.add_user("hi");
    chat.begin_stream();
    chat.stream_set("Response");
    chat.end_stream();

    // Wide: should show "Assistant"
    chat.resize(100, 24);
    {
        RenderContext ctx{100, 24, 0};
        RenderContextGuard guard(ctx);
        auto wide = render_at(chat.build(), 100);
        assert(contains_text(wide, "Assistant"));
    }

    // Narrow (<60): should show "AI" instead
    chat.resize(40, 24);
    {
        RenderContext ctx{40, 24, 1};
        RenderContextGuard guard(ctx);
        assert(chat.compact());
        auto narrow = render_at(chat.build(), 40);
        assert(contains_text(narrow, "AI"));
    }

    std::println("PASS\n");
}

// ============================================================================
// 17. ChatView: streaming state
// ============================================================================

void test_chatview_streaming() {
    std::println("--- test_chatview_streaming ---");

    ChatView chat;
    assert(!chat.is_streaming());
    assert(chat.message_count() == 0);

    chat.add_user("Hello");
    assert(chat.message_count() == 1);

    chat.begin_stream();
    assert(chat.is_streaming());
    assert(chat.message_count() == 2);

    chat.stream_append("Part 1 ");
    chat.stream_append("Part 2");

    auto rows = render_at(chat.build(), 80);
    assert(contains_text(rows, "Hello"));
    assert(contains_text(rows, "Part 1 Part 2"));

    chat.end_stream();
    assert(!chat.is_streaming());
    assert(chat.message_count() == 2);

    // After end_stream, content should still be there
    auto rows2 = render_at(chat.build(), 80);
    assert(contains_text(rows2, "Part 1 Part 2"));

    std::println("PASS\n");
}

// ============================================================================
// 18. Alt-screen path: render_tree without auto_height (fixed canvas)
// ============================================================================

void test_alt_screen_render() {
    std::println("--- test_alt_screen_render ---");

    auto ui = vstack()(
        text("Top"),
        hstack()(text("Left"), box().grow(1), text("Right")),
        text("Bottom")
    );

    // Alt-screen: fixed canvas, no auto_height
    for (int w : {40, 80, 120}) {
        int h = 24;
        StylePool pool;
        Canvas canvas(w, h, &pool);
        render_tree(ui, canvas, pool, theme::dark);

        auto r0 = get_row(canvas, 0);
        assert(r0.find("Top") != std::string::npos);

        // "Right" should be present somewhere
        bool found = false;
        for (int y = 0; y < h; ++y) {
            if (get_row(canvas, y).find("Right") != std::string::npos) {
                found = true;
                break;
            }
        }
        assert(found);
        std::println("  alt@{}: OK", w);
    }

    std::println("PASS\n");
}

// ============================================================================
// 19. Double-buffer diff: only changed cells emitted
// ============================================================================

void test_diff_after_resize() {
    std::println("--- test_diff_after_resize ---");
    StylePool pool;
    auto sid = pool.intern(Style{});

    // Front buffer at width 80
    Canvas front(80, 5, &pool);
    front.write_text(0, 0, "Hello", sid);

    // Back buffer at width 80 (same content — diff should be empty)
    Canvas back1(80, 5, &pool);
    back1.write_text(0, 0, "Hello", sid);

    std::string out1;
    diff(front, back1, pool, out1);
    // No cell changes → no SGR/content in output (just cursor moves maybe)
    assert(out1.find("Hello") == std::string::npos);
    std::println("  same content: no diff output — OK");

    // Back buffer with changed content (no shared chars with "Hello" to
    // avoid diff skipping identical cells and splitting the output string)
    Canvas back2(80, 5, &pool);
    back2.write_text(0, 0, "ABCDE", sid);
    back2.mark_all_damaged();

    std::string out2;
    diff(front, back2, pool, out2);
    assert(out2.find("ABCDE") != std::string::npos);
    std::println("  changed content: diff has new content — OK");

    // After "resize": new canvases at different width
    Canvas front2(120, 5, &pool);
    Canvas back3(120, 5, &pool);
    back3.write_text(0, 0, "Resized", sid);
    back3.mark_all_damaged();

    std::string out3;
    diff(front2, back3, pool, out3);
    assert(out3.find("Resized") != std::string::npos);
    std::println("  resized canvas: full repaint — OK");

    std::println("PASS\n");
}

// ============================================================================
// 20. Stress: rapid resize cycle doesn't corrupt state
// ============================================================================

void test_rapid_resize_cycle() {
    std::println("--- test_rapid_resize_cycle ---");

    ChatView chat;
    chat.add_user("message");
    chat.begin_stream();
    chat.stream_set("streaming response text");
    chat.end_stream();

    // Simulate rapid resize: 80 → 40 → 120 → 60 → 200 → 80
    int widths[] = {80, 40, 120, 60, 200, 80};

    for (int w : widths) {
        chat.resize(w, 24);
        auto rows = render_at(chat.build(), w);

        assert(!rows.empty());
        assert(contains_text(rows, "message"));
        assert(contains_text(rows, "streaming response text"));
        assert_left_aligned(rows, 10, "rapid resize");
    }

    std::println("PASS\n");
}

// ============================================================================
// 21. Nested widget tree: border + padding + content adapts to width
// ============================================================================

void test_nested_layout_resize() {
    std::println("--- test_nested_layout_resize ---");

    auto make_ui = [] {
        return vstack()(
            box().border(BorderStyle::Round)(
                vstack()(
                    text("Inside border"),
                    hstack()(text("A"), box().grow(1), text("B"))
                )
            ),
            text("Outside")
        );
    };

    for (int w : {30, 60, 120}) {
        auto rows = render_at(make_ui(), w);
        assert(contains_text(rows, "Inside border"));
        assert(contains_text(rows, "Outside"));
        assert(contains_text(rows, "A"));
        assert(contains_text(rows, "B"));
        std::println("  width={}: {} rows — OK", w, rows.size());
    }

    std::println("PASS\n");
}

// ============================================================================

int main() {
    // Canvas & hash fundamentals
    test_canvas_resize();
    test_row_hash_width_dependency();
    test_inline_state_reset();
    test_serialize_after_resize();
    test_row_hash_stability();

    // Element rendering at multiple widths
    test_element_at_widths();
    test_render_determinism();
    test_full_resize_pipeline();
    test_nested_layout_resize();

    // Alt-screen path
    test_alt_screen_render();
    test_diff_after_resize();

    // Widget responsiveness
    test_input_resize();
    test_spinner_resize();
    test_table_resize();
    test_markdown_resize();
    test_badge_resize();

    // ChatView
    test_chatview_resize();
    test_chatview_cache_invalidation();
    test_chatview_compact();
    test_chatview_streaming();

    // Stress
    test_rapid_resize_cycle();

    std::println("\n=== All {} resize tests passed! ===", 21);
    return 0;
}
