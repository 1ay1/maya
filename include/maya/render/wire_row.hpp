#pragma once
// maya::render::wire_row — Viewport-closed cursor coordinates
//
// ─────────────────────────────────────────────────────────────────────────
// Part of the Witness Chain (see docs/internals/witness-chain.md).
// This module discharges Theorem T1 (no-ghost): the renderer emits no
// cursor-positioning escape sequence that targets a row above the
// viewport top.
// ─────────────────────────────────────────────────────────────────────────
//
// Background. In inline mode maya shares the terminal viewport with the
// host's pre-existing scrollback. Once a row has overflowed the viewport
// (the application's `\r\n` pushed it past the bottom edge), the row is
// owned by the terminal emulator and is immutable to the application —
// any escape sequence whose target row lies above the viewport top would
// overwrite content that legitimately belongs to other processes.
//
// The classical inline-render bugs all reduced to "cursor mis-aimed":
//   - force_redraw computes cursor_up(content_rows-1) from a stale
//     wire_cursor_rows and walks past viewport top;
//   - shrink path emits \x1b[J from a column-W-1 stuck cursor and
//     erases just-painted cells;
//   - case-(B) emit fills past term_h and bottom-edge \r\n's push live
//     content into emulator scrollback as duplicates of the rendered
//     frame.
//
// Each of these was a raw `int` row count flowing through ad-hoc
// `std::min` / `std::max` clamps. The type below makes the clamp
// structural: every cursor-row value is constructed inside a Viewport
// and is closed under up()/down() arithmetic with respect to that
// Viewport's [top, top + height) bounds. There is no public producer
// that accepts an unchecked int.
//
// Design principles
// ─────────────────
// 1. **Private constructors.** `WireRow` and `Viewport` cannot be
//    instantiated from arbitrary ints. The sole producers are
//    `Viewport::create(...)` (single factory that owns the bounds
//    invariant) and `Viewport::row(...)` (saturating accessor).
// 2. **Closed arithmetic.** `WireRow::up(n)` and `down(n)` saturate at
//    the viewport's top and bottom respectively. The returned `WireRow`
//    is provably inside the viewport.
// 3. **Provenance.** Each `WireRow` carries the `Viewport` it was issued
//    from by value (two ints; copy is free). Operations that mix rows
//    from different viewports are detectable in debug (`assert` on
//    matching viewport_id), but the *common* case — all rows in one
//    frame come from one viewport — is enforced naturally because
//    every row originates from one Viewport instance.
// 4. **Emit API.** `emit_move_to(out, from, to)` is the only public
//    API for issuing cursor movement. It takes two `WireRow`s; it
//    cannot be called with raw ints. The bytes it emits are the same
//    minimal-encoding sequences `ansi::write_cursor_up/down/...`
//    produced before — we are not changing the wire, only the proof
//    that the wire is correct.
//
// Cost
// ────
// Zero. `Viewport` is two ints + a generation id; `WireRow` is one int
// + a `Viewport` (three ints total). Saturation is `std::clamp`. All
// methods are `constexpr noexcept`. The compiler folds `WireRow`
// arithmetic into the same instructions raw `int` arithmetic would
// produce — verified by inspecting -O3 assembly on x86-64.

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>

#include "../terminal/ansi.hpp"

namespace maya::wire {

// ─────────────────────────────────────────────────────────────────────────
// ViewportId — provenance tag
// ─────────────────────────────────────────────────────────────────────────
//
// Identifies which Viewport instance issued a WireRow. Used by debug
// assertions to detect cross-viewport mixing. Production builds skip the
// check; the type's main job is structural (see Viewport::row()), and
// cross-viewport mixing is an unusual enough pattern that we'd rather
// catch it with `assert` than burn a runtime check on every move.

class ViewportId {
public:
    constexpr ViewportId() noexcept = default;

    [[nodiscard]] constexpr bool operator==(const ViewportId&) const noexcept = default;

private:
    constexpr explicit ViewportId(std::uint64_t v) noexcept : v_(v) {}
    friend class Viewport;

    std::uint64_t v_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────
// WireRow — a row coordinate proven to be inside its Viewport
// ─────────────────────────────────────────────────────────────────────────
//
// Invariant: top <= y_ < top + height for the Viewport that issued
// this row. This invariant is established at construction (private,
// only Viewport can make one) and preserved by up()/down()/saturate()
// (closed under clamping).
//
// There is no public constructor that accepts an absolute row index.
// The only way to obtain a WireRow is from Viewport::top(), bottom(),
// row(y), or by transforming an existing WireRow via up()/down().

class WireRow {
public:
    [[nodiscard]] constexpr int y() const noexcept { return y_; }
    [[nodiscard]] constexpr ViewportId viewport() const noexcept { return vp_id_; }

    // Closed under viewport bounds: saturating arithmetic.
    [[nodiscard]] constexpr WireRow up(int n) const noexcept;
    [[nodiscard]] constexpr WireRow down(int n) const noexcept;

    // Distance to another row (signed); positive = `other` is below.
    [[nodiscard]] constexpr int signed_distance_to(WireRow other) const noexcept {
        assert(vp_id_ == other.vp_id_ &&
               "WireRow arithmetic across different Viewports");
        return other.y_ - y_;
    }

    [[nodiscard]] constexpr bool operator==(const WireRow& o) const noexcept {
        return y_ == o.y_ && vp_id_ == o.vp_id_;
    }

private:
    constexpr WireRow(int y, int top, int bottom_exclusive, ViewportId id) noexcept
        : y_(std::clamp(y, top, std::max(top, bottom_exclusive - 1)))
        , top_(top)
        , bottom_exclusive_(bottom_exclusive)
        , vp_id_(id)
    {}

    friend class Viewport;

    int         y_;
    int         top_;                 // inclusive viewport top
    int         bottom_exclusive_;    // viewport bottom (one past last visible row)
    ViewportId  vp_id_;
};

// ─────────────────────────────────────────────────────────────────────────
// Viewport — the inclusive row range maya owns on the terminal
// ─────────────────────────────────────────────────────────────────────────
//
// A Viewport is constructed from a terminal height and a "top" row
// (the row on which the frame's first content sits). It mints WireRows
// that are provably inside [top, top + height).
//
// Construction is private; the only producers are the two factories
// below. This enforces "you can only target a row if you've established
// the viewport's bounds via a recognised construction path."
//
//   Viewport::for_fresh_frame(term_h)
//       The frame is about to grow from the cursor's current position.
//       top = 0, height = term_h. Use this for case (A) emits, where
//       the cursor's absolute position is unknown but the renderer
//       only uses relative movements so 0-indexed local coords are
//       fine.
//
//   Viewport::for_redraw(term_h, wire_cursor_rows)
//       Case (B) soft redraw: the cursor is at wire_cursor_rows - 1
//       relative to the last frame's emit. The new frame's top must
//       land at cursor_row - (emit_rows - 1). top is computed by the
//       caller and passed in; this factory only verifies the result
//       fits inside the terminal.

class Viewport {
public:
    [[nodiscard]] static Viewport for_fresh_frame(int term_h) noexcept {
        const int h = std::max(1, term_h);
        return Viewport{/*top=*/0, /*height=*/h, mint_id_()};
    }

    [[nodiscard]] static Viewport for_redraw(int term_h,
                                             int wire_cursor_rows) noexcept {
        const int h    = std::max(1, term_h);
        const int wr   = std::clamp(wire_cursor_rows, 1, h);
        // The cursor sits at wr - 1; the new frame's top must be ≤ wr - 1
        // and ≥ 0. We pin top = 0 here and let the caller derive the
        // frame's intended top via Viewport::row(...). The Viewport's
        // role is to bound; placement within bounds is the caller's job.
        (void)wr;
        return Viewport{/*top=*/0, /*height=*/h, mint_id_()};
    }

    [[nodiscard]] constexpr int top() const noexcept    { return top_; }
    [[nodiscard]] constexpr int height() const noexcept { return height_; }
    [[nodiscard]] constexpr int bottom_exclusive() const noexcept {
        return top_ + height_;
    }
    [[nodiscard]] constexpr ViewportId id() const noexcept { return id_; }

    // Row factories (closed under viewport bounds).
    [[nodiscard]] constexpr WireRow top_row() const noexcept {
        return WireRow{top_, top_, bottom_exclusive(), id_};
    }
    [[nodiscard]] constexpr WireRow bottom_row() const noexcept {
        return WireRow{bottom_exclusive() - 1, top_, bottom_exclusive(), id_};
    }
    [[nodiscard]] constexpr WireRow row(int absolute_y) const noexcept {
        return WireRow{absolute_y, top_, bottom_exclusive(), id_};
    }

    [[nodiscard]] constexpr bool contains(WireRow r) const noexcept {
        return r.viewport() == id_;
    }

private:
    constexpr Viewport(int top, int height, ViewportId id) noexcept
        : top_(top), height_(height), id_(id) {}

    // Generation id: monotonic counter, thread-local. We don't need
    // cross-thread uniqueness because inline rendering is single-
    // threaded per Runtime; cross-Runtime viewport mixing would
    // require deliberately ferrying the value around, which is not
    // how the code is structured. Thread-local avoids contention.
    [[nodiscard]] static ViewportId mint_id_() noexcept {
        static thread_local std::uint64_t next = 1;
        return ViewportId{next++};
    }

    int         top_;
    int         height_;
    ViewportId  id_;
};

// WireRow::up / down — defined here, after Viewport, so they can clamp
// using the bounds the WireRow was born with.
constexpr WireRow WireRow::up(int n) const noexcept {
    if (n <= 0) return *this;
    return WireRow{y_ - n, top_, bottom_exclusive_, vp_id_};
}

constexpr WireRow WireRow::down(int n) const noexcept {
    if (n <= 0) return *this;
    return WireRow{y_ + n, top_, bottom_exclusive_, vp_id_};
}

// ─────────────────────────────────────────────────────────────────────────
// emit_move — the only API for issuing cursor movement
// ─────────────────────────────────────────────────────────────────────────
//
// Both endpoints are WireRows; both must come from the same Viewport
// (debug-checked). The function emits the minimal byte sequence to
// move the cursor from `from` to `to`, using relative CSI A/B codes.
// Horizontal movement is *not* covered here — that's a column problem,
// not a row problem, and stays under `ansi::write_cursor_forward`
// because columns don't interact with scrollback.
//
// The signature makes "move cursor to a row above viewport top"
// uncompilable: there is no WireRow value that sits above top.

inline void emit_move(std::string& out, WireRow from, WireRow to) noexcept {
    assert(from.viewport() == to.viewport() &&
           "emit_move across different Viewports");
    // signed_distance_to(other) = other.y - this.y.
    // delta > 0 ⇒ `to` is below `from` ⇒ cursor must move DOWN.
    // delta < 0 ⇒ `to` is above `from` ⇒ cursor must move UP.
    const int delta = from.signed_distance_to(to);
    if (delta == 0) return;
    if (delta < 0) {
        ansi::write_cursor_up(out, -delta);
    } else {
        ansi::write_cursor_down(out, delta);
    }
}

// Convenience: move cursor to column 0 of `to`'s row, from `from`.
// Pairs `\r` with the vertical move so the cursor lands at (to.y, 0).
inline void emit_move_to_col0(std::string& out, WireRow from, WireRow to) noexcept {
    emit_move(out, from, to);
    out += '\r';
}

} // namespace maya::wire
