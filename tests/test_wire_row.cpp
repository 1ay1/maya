// Tests for the Witness Chain — Layer 1: WireRow + Viewport.
//
// These tests are the proof witnesses for Theorem T1 (no-ghost): every
// cursor-positioning emit terminates at a row inside the Viewport. The
// type system enforces this by construction; the tests below verify the
// representable values are exactly the safe ones.

#include "agtest.hpp"

#include "maya/render/wire_row.hpp"
#include "check.hpp"

#include <print>

// Local 1-arg shim: tests/check.hpp's MAYA_TEST_CHECK takes a message;
// for this file every check is self-explanatory from the condition so
// we wrap with a no-op message.
using namespace maya::wire;

// ─────────────────────────────────────────────────────────────────────────
// 1. Saturation: up() clamps at viewport top.
// ─────────────────────────────────────────────────────────────────────────
//
// Asking for a row 1000 above the bottom of a height-24 viewport must
// produce viewport top, not a negative absolute row. This is the
// structural witness for "cursor never points above the visible region."
TEST_CASE("up saturates at top") {
    std::println("--- test_up_saturates_at_top ---");
    Viewport vp = Viewport::for_fresh_frame(24);
    WireRow bottom = vp.bottom_row();
    CHECK(bottom.y() == 23);

    WireRow way_up = bottom.up(1000);
    CHECK(way_up.y() == 0);   // saturated, not -977
    CHECK(way_up.viewport() == vp.id());
}

// ─────────────────────────────────────────────────────────────────────────
// 2. Saturation: down() clamps at viewport bottom.
// ─────────────────────────────────────────────────────────────────────────
//
// Symmetric to the above — down() cannot escape the viewport bottom.
// Frames that try to grow past term_h via raw arithmetic used to push
// host scrollback content into emulator scrollback; closed arithmetic
// makes that path uncompilable.
TEST_CASE("down saturates at bottom") {
    std::println("--- test_down_saturates_at_bottom ---");
    Viewport vp = Viewport::for_fresh_frame(24);
    WireRow top = vp.top_row();
    CHECK(top.y() == 0);

    WireRow way_down = top.down(1000);
    CHECK(way_down.y() == 23);   // saturated at viewport bottom
}

// ─────────────────────────────────────────────────────────────────────────
// 3. Row factory clamps absolute requests.
// ─────────────────────────────────────────────────────────────────────────
//
// `Viewport::row(y)` is the only way to mint a WireRow from an absolute
// index. It clamps. A caller that has computed a row from layout (e.g.
// "new frame top = cursor_row - (emit_rows - 1)") cannot accidentally
// mint a negative row; the type swallows the OOB and yields the top.
TEST_CASE("row factory clamps") {
    std::println("--- test_row_factory_clamps ---");
    Viewport vp = Viewport::for_fresh_frame(10);
    CHECK(vp.row(-5).y() == 0);
    CHECK(vp.row(0).y()  == 0);
    CHECK(vp.row(5).y()  == 5);
    CHECK(vp.row(9).y()  == 9);
    CHECK(vp.row(10).y() == 9);
    CHECK(vp.row(99).y() == 9);
}

// ─────────────────────────────────────────────────────────────────────────
// 4. up().down() round-trip preserves position when both stay inside.
// ─────────────────────────────────────────────────────────────────────────
//
// Algebraic sanity: when neither operation hits a boundary, they are
// each other's inverse. This is what lets the renderer compose moves
// freely without worrying about hidden state.
TEST_CASE("up down inverse inside viewport") {
    std::println("--- test_up_down_inverse_inside_viewport ---");
    Viewport vp = Viewport::for_fresh_frame(24);
    WireRow start = vp.row(12);
    WireRow up5 = start.up(5);
    WireRow back = up5.down(5);
    CHECK(up5.y()  == 7);
    CHECK(back.y() == 12);
}

// ─────────────────────────────────────────────────────────────────────────
// 5. ViewportId differs between separately-created Viewports.
// ─────────────────────────────────────────────────────────────────────────
//
// Two Viewports created in sequence have distinct ids. Debug builds use
// this to assert WireRow arithmetic stays inside one Viewport.
TEST_CASE("viewport id unique") {
    std::println("--- test_viewport_id_unique ---");
    Viewport a = Viewport::for_fresh_frame(24);
    Viewport b = Viewport::for_fresh_frame(24);
    CHECK(!(a.id() == b.id()));

    WireRow ar = a.top_row();
    WireRow br = b.top_row();
    CHECK(!(ar.viewport() == br.viewport()));
}

// ─────────────────────────────────────────────────────────────────────────
// 6. signed_distance_to is positive when other is below.
// ─────────────────────────────────────────────────────────────────────────
//
// emit_move uses this; positive distance ⇒ CSI B (cursor down).
TEST_CASE("signed distance") {
    std::println("--- test_signed_distance ---");
    Viewport vp = Viewport::for_fresh_frame(24);
    WireRow at5  = vp.row(5);
    WireRow at10 = vp.row(10);
    CHECK(at5.signed_distance_to(at10) ==  5);
    CHECK(at10.signed_distance_to(at5) == -5);
    CHECK(at5.signed_distance_to(at5)  ==  0);
}

// ─────────────────────────────────────────────────────────────────────────
// 7. emit_move produces correct minimal byte sequences.
// ─────────────────────────────────────────────────────────────────────────
//
// The bytes emitted must match the historical ansi::write_cursor_up/down
// shape, since the wire is the eventual proof of the witness chain.
TEST_CASE("emit move bytes") {
    std::println("--- test_emit_move_bytes ---");
    Viewport vp = Viewport::for_fresh_frame(24);

    // Same row → no bytes.
    {
        std::string out;
        emit_move(out, vp.row(5), vp.row(5));
        CHECK(out.empty());
    }

    // Move up 3 rows.
    {
        std::string out;
        emit_move(out, vp.row(10), vp.row(7));
        CHECK(out == "\x1b[3A");
    }

    // Move down 4 rows.
    {
        std::string out;
        emit_move(out, vp.row(2), vp.row(6));
        CHECK(out == "\x1b[4B");
    }

    // Move up 1 row (CSI A with explicit 1, not bare CSI A — matches
    // ansi::write_cursor_up which always writes the count).
    {
        std::string out;
        emit_move(out, vp.row(5), vp.row(4));
        CHECK(out == "\x1b[1A");
    }
}

// ─────────────────────────────────────────────────────────────────────────
// 8. emit_move where target saturates at top emits only the safe delta.
// ─────────────────────────────────────────────────────────────────────────
//
// This is the central anti-ghost guarantee: a caller asking to move up
// further than viewport top cannot emit a CSI sequence that targets a
// row above the viewport. The saturation happens at WireRow construction,
// so the delta emit_move sees is already the safe one.
TEST_CASE("emit move cannot escape viewport top") {
    std::println("--- test_emit_move_cannot_escape_viewport_top ---");
    Viewport vp = Viewport::for_fresh_frame(24);
    WireRow from = vp.row(5);
    WireRow to   = from.up(100);    // saturates at top (row 0)
    CHECK(to.y() == 0);

    std::string out;
    emit_move(out, from, to);
    // Delta is -5 (from row 5 up 5), not -100. CSI 5 A.
    CHECK(out == "\x1b[5A");
}

// ─────────────────────────────────────────────────────────────────────────
// 9. emit_move_to_col0 emits vertical move + \r.
// ─────────────────────────────────────────────────────────────────────────
TEST_CASE("emit move to col0") {
    std::println("--- test_emit_move_to_col0 ---");
    Viewport vp = Viewport::for_fresh_frame(24);

    {
        std::string out;
        emit_move_to_col0(out, vp.row(10), vp.row(8));
        CHECK(out == "\x1b[2A\r");
    }

    {
        std::string out;
        emit_move_to_col0(out, vp.row(3), vp.row(3));
        // Same row: no vertical move, but still \r.
        CHECK(out == "\r");
    }
}

// ─────────────────────────────────────────────────────────────────────────
// 10. Defensive: zero/negative height collapses to height 1.
// ─────────────────────────────────────────────────────────────────────────
//
// Terminal resize during a render could in theory supply term_h == 0.
// The viewport must still be a legal value the renderer can target.
// Height 1 lets the renderer emit a single row at row 0, which is the
// minimum useful viewport.
TEST_CASE("zero height collapses to one") {
    std::println("--- test_zero_height_collapses_to_one ---");
    Viewport vp = Viewport::for_fresh_frame(0);
    CHECK(vp.height() == 1);
    CHECK(vp.top_row().y() == 0);
    CHECK(vp.bottom_row().y() == 0);
}

// ─────────────────────────────────────────────────────────────────────────
// Static asserts: properties the compiler proves for us.
// ─────────────────────────────────────────────────────────────────────────
//
// WireRow and Viewport are tiny value types; the compiler can prove
// triviality and reasonable size at compile time.
static_assert(sizeof(WireRow) <= 32, "WireRow should remain a small value type");
static_assert(sizeof(Viewport) <= 32, "Viewport should remain a small value type");

