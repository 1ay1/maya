#pragma once
// maya::render::serialize - Canvas serialization for inline rendering
//
// Converts canvas cells into an ANSI byte stream for terminal output.
// Used by both the one-shot `serialize()` (full canvas dump) and the
// stateful `compose_inline_frame()` (differential row update for
// Claude-Code-style inline progress output).
//
// The inline renderer keeps a cached copy of the last-rendered cells
// in `InlineFrameState`. Each frame, it compares the new canvas row-by-
// row using `simd::bulk_eq` (exact 64-bit-packed cell comparison — no
// hash collisions) to find the first row that actually changed, then
// rewrites only that row and everything below it. Rows still on-screen
// are overwritten in place; rows that scrolled into history stay put.

#include <cstdint>
#include <optional>
#include <string>

#include "canvas.hpp"

namespace maya {

/// Find the last non-empty row in a canvas (1-based height).
/// Returns the number of rows that contain visible content.
int content_height(const Canvas& canvas) noexcept;

// ============================================================================
// ContentRows — derivation witness for the row count passed to compose
// ============================================================================
//
// Part of the Witness Chain (see docs/internals/witness-chain.md).
//
// Theorem T2 (no-corruption) requires that compose_inline_frame's
// per-row emit loop walks exactly the rows the canvas painted — no
// fewer (missed cells), no more (serialising stale cells from prior
// frames into the live frame). The runtime check
// `content_rows == canvas.max_content_row() + 1` enforces this only
// when MAYA_DEBUG_SHADOW_VERIFY is defined; otherwise the precondition
// is a comment.
//
// ContentRows lifts the precondition into the type system. It is a
// thin wrapper around `int` with a private constructor; the only
// producer is `content_rows(canvas)` below. A future overload of
// `compose_inline_frame` taking ContentRows instead of `int` will
// reject every call site that derives the row count from anywhere
// other than the canvas — layout-computed heights, hard-coded
// constants, off-by-one arithmetic on prev_rows, etc.
//
// The bound canvas pointer carries provenance for debug builds: if a
// caller computes ContentRows from canvas A and passes it to compose
// alongside canvas B, the runtime `assert` catches the mix-up. The
// pointer is intentionally not part of the type's identity (no
// templating on canvas address); doing so would force every call site
// to be templated for no real safety benefit — the *intent* of the
// type is to make "derive from the wrong source" uncompilable, and
// the single-producer rule is what delivers that.

class ContentRows {
public:
    [[nodiscard]] constexpr int value() const noexcept { return rows_; }

    /// Debug-only provenance check. compose_inline_frame asserts this
    /// matches the canvas it was passed; mismatch is a programming
    /// error (computed rows from a different canvas than rendered).
    [[nodiscard]] constexpr const Canvas* source_canvas() const noexcept {
        return source_;
    }

private:
    constexpr ContentRows(int r, const Canvas* src) noexcept
        : rows_(r), source_(src) {}
    friend ContentRows content_rows(const Canvas&) noexcept;

    int            rows_;
    const Canvas*  source_;
};

/// The sole producer of ContentRows. Derives the row count from the
/// canvas's painted region (canvas.max_content_row() + 1) and binds
/// the result to that canvas for downstream provenance verification.
///
/// Use this in place of the raw `content_height(canvas)` call site
/// whenever the result will be passed to compose_inline_frame, so the
/// type system witnesses that the row count came from the canvas the
/// frame will paint.
[[nodiscard]] inline ContentRows content_rows(const Canvas& canvas) noexcept {
    return ContentRows{content_height(canvas), &canvas};
}

/// Serialize `rows` rows of the canvas starting at `start_row`
/// (or all rows if rows <= 0).
void serialize(const Canvas& canvas, const StylePool& pool,
               std::string& out, int rows = 0, int start_row = 0);

// ============================================================================
// Inline frame composition
// ============================================================================

struct InlineFrameState;  // forward

/// Opaque token representing "commit this many rows of the current
/// frame's prev_cells to scrollback." Constructed only via
/// `InlineFrameState::scrollback_marker(rows)`, which clamps `rows` to
/// the state's actual `prev_rows` at the time of the call. Consuming
/// the marker via `InlineFrameState::commit(marker)` is the only way
/// to advance the state's scrollback boundary through the typed path.
///
/// The marker carries no state pointer / generation — single-threaded
/// inline rendering means the marker is created and consumed in close
/// temporal proximity within the same Runtime tick. The safety the
/// type provides is "you can't construct a row count from thin air" —
/// the application has to obtain a marker from a live state, which
/// forces them to read the current `prev_rows` rather than carrying a
/// stale int around.
class ScrollbackMarker {
public:
    /// Empty marker — committing it is a no-op. Useful as a default
    /// when no scrollback advance is wanted this tick.
    constexpr ScrollbackMarker() noexcept = default;

    /// Number of rows the marker will commit when consumed.
    [[nodiscard]] constexpr int rows() const noexcept { return rows_; }

    [[nodiscard]] constexpr bool empty() const noexcept { return rows_ <= 0; }

private:
    int rows_ = 0;
    constexpr explicit ScrollbackMarker(int r) noexcept : rows_(r) {}
    friend struct InlineFrameState;
};

/// Persistent state for the inline (row-diff) renderer.
///
/// Holds a copy of the last-rendered cell buffer so successive frames can
/// be compared exactly via `simd::bulk_eq` instead of hashed (no collisions).
/// Carry the same instance across frames; call `reset()` to invalidate
/// after a write failure, `\x1b[2J` clear, or any other event that
/// desynchronises the terminal from the cached state.
struct InlineFrameState {
    AlignedBuffer prev_cells;
    int           prev_width = 0;
    int           prev_rows  = 0;  // content_rows from the last composed frame

    /// Terminal-mode flags persisted across frames. compose_inline_frame
    /// flips `decawm_off`/`cursor_hidden` to true the first time it
    /// emits the corresponding sequence; subsequent frames skip the
    /// per-frame re-emission and the bracket exit. reset() clears them
    /// so the next composition re-establishes the modes from scratch
    /// (cursor position is also unknown after reset). Owners call
    /// `finalize(out)` on shutdown to restore the terminal.
    bool decawm_off    = false;
    bool cursor_hidden = false;

    /// Rows of the prior frame still on screen that case-(B) soft
    /// redraw must erase ABOVE the new frame, not just below.
    ///
    /// Set by `Runtime::force_redraw()` (to `wire_cursor_rows`)
    /// *before* it zeroes `prev_rows`; consumed by
    /// `compose_inline_frame`'s case-(B) emit, which uses it to size
    /// the upward erase so the previous (possibly larger) frame's top
    /// is wiped, not left visible above the freshly-painted shorter
    /// frame. Cleared back to 0 by case-(B) after the erase emits.
    ///
    /// Zero in steady state. Non-zero only between force_redraw and
    /// the next compose. Width-change `reset()` also clears it because
    /// the row counts are no longer meaningful at the new width.
    int ghost_rows_above = 0;

    /// Viewport rows occupied by the prior frame as the terminal sees
    /// it — i.e. `min(prev_rows_at_last_emit, term_h)`. Set at the end
    /// of every successful compose; intentionally NOT mutated by
    /// `commit_prefix` (commits move rows into native scrollback in
    /// the renderer's bookkeeping but do not change the terminal
    /// cursor row, so the on-screen offset from the prior emit is
    /// invariant under commits). `force_redraw()` snapshots this into
    /// `ghost_rows_above` so the case-(B) upward erase walks the
    /// correct number of rows even when commits happened between
    /// the last emit and the redraw.
    int wire_cursor_rows = 0;

    /// Production shadow-of-wire verifier. After each successful
    /// compose, `shadow_hash` is set to a deterministic 64-bit hash
    /// over the visible-viewport portion of `prev_cells`. Before the
    /// next compose runs, the runtime can re-hash the same range and
    /// compare; a mismatch means somebody mutated prev_cells outside
    /// the compose path (or memory corruption) — the shadow is no
    /// longer trustworthy and the next render must go through the
    /// Divergent recovery path rather than emit a diff against a
    /// poisoned reference. UINT64_MAX is the "not yet computed"
    /// sentinel set by reset() and by the constructor. The cost is
    /// O(visible_rows × W) of u64 mixing per frame — ~1µs on a
    /// 200×80 viewport, which the debug verifier (MAYA_DEBUG_
    /// SHADOW_VERIFY) already pays unconditionally; production carries
    /// the same overhead but with the failure routed to graceful
    /// recovery instead of abort().
    uint64_t shadow_hash = static_cast<uint64_t>(-1);

    void reset() noexcept {
        prev_width        = 0;
        prev_rows         = 0;
        decawm_off        = false;
        cursor_hidden     = false;
        shadow_hash       = static_cast<uint64_t>(-1);
        ghost_rows_above  = 0;
        wire_cursor_rows  = 0;
    }

    /// Append the bytes needed to restore terminal state owned by this
    /// frame (cursor visibility, DECAWM). Clears the flags so further
    /// calls are no-ops. Safe to call multiple times.
    void finalize(std::string& out) {
        if (cursor_hidden) {
            out.append("\x1b[?25h");
            cursor_hidden = false;
        }
        if (decawm_off) {
            out.append("\x1b[?7h");
            decawm_off = false;
        }
    }

    /// Build a typed marker for committing `rows` rows of scrollback.
    /// Clamped to [0, prev_rows]; passing prev_rows means "commit
    /// everything that's currently considered prev frame." Returns an
    /// empty marker (no-op when consumed) when prev_rows == 0.
    [[nodiscard]] ScrollbackMarker scrollback_marker(int rows) const noexcept {
        if (rows <= 0 || prev_rows <= 0) return ScrollbackMarker{};
        return ScrollbackMarker{std::min(rows, prev_rows)};
    }

    /// Commit the marker. Shifts `prev_cells` up by marker.rows() rows
    /// and decrements `prev_rows`. A marker that targets all the rows
    /// (or more) clears the state cleanly via `reset()`.
    void commit(ScrollbackMarker marker) noexcept {
        commit_prefix(marker.rows());
    }

    /// Mark the top `rows` rows of the current prev frame as committed to
    /// terminal scrollback.  Shifts `prev_cells` up by `rows * prev_width`
    /// and decrements `prev_rows`.
    ///
    /// Use this when the caller knows the next frame's tree intentionally
    /// omits content that was at the top of the previous frame (e.g. a
    /// chat UI that slices old messages once they've scrolled above the
    /// viewport).  Without this call, `compose_inline_frame` would treat
    /// the shorter tree as "content removed from the bottom" and erase
    /// visible rows.
    ///
    /// New code should prefer `scrollback_marker(rows) + commit(marker)`
    /// — the typed path forces the caller to query the live state's
    /// prev_rows rather than carrying a stale int. This signature is
    /// retained for source compatibility with existing consumers.
    void commit_prefix(int rows) noexcept;
};

/// Compose the byte stream for one inline frame into `out`.
///
/// **DEPRECATED FOR APPLICATION CODE.** Use the Witness Chain instead:
///
///   - `compose_inline_frame_v2(...)` for the typed compose path that
///     requires `ContentRows` + `ShadowWitness` and returns
///     `FrameBytes`, or
///   - `inline_frame::InlineFrame<Tag>::render(...)` for the full
///     type-state-machine API (the Runtime and print/live use this).
///
/// This raw entry point is retained as an internal helper (the chain
/// composes onto it under friend access) and for the low-level
/// scrollback-correctness test suite which exercises the byte-emit
/// algorithm directly. NO PRODUCTION CODE PATH calls this any more.
///
/// Writes nothing (returns with `out` unchanged) when the current canvas
/// is byte-for-byte identical to the previous one. Otherwise emits the
/// minimal ANSI sequence that:
///   1. (optionally) wraps the update in DEC 2026 synchronized output,
///   2. hides the cursor,
///   3. moves to the first row that actually changed (never into
///      scrollback — rows that rolled off-screen are treated as
///      committed and skipped),
///   4. for each row in [first_changed, content_rows): emits only the
///      sub-span of cells that differ from the cached prev row (so an
///      unchanged middle row costs just \r\n, and a row whose only
///      change is one digit costs ~5 bytes — not the whole row),
///   5. erases any rows the frame shrank past (still on-screen only),
///   6. restores the cursor.
///
/// `content_rows` is the new frame's row count (typically
/// `content_height(canvas)`). `term_h` is the terminal height — used to
/// clamp cursor-up moves so we never try to "scroll back" into history.
/// `state` is updated with the new cell buffer on successful composition.
///
/// `synchronized_output` toggles the DEC 2026 wrapper. Default true for
/// backward compat; pass false on terminals that don't honor mode 2026.
void compose_inline_frame(const Canvas& canvas,
                          int content_rows,
                          int term_h,
                          const StylePool& pool,
                          InlineFrameState& state,
                          std::string& out,
                          bool synchronized_output = true);

/// Re-compute the shadow-of-wire hash over `state.prev_cells` and
/// compare it to the value `compose_inline_frame` stored at the end
/// of the previous render.
///
/// **DEPRECATED FOR APPLICATION CODE.** Use `verify_shadow(state)`
/// instead — it returns a typed `std::optional<ShadowWitness>` that
/// the Witness Chain consumes through `compose_inline_frame_v2`,
/// making "compose without verifying the shadow" a compile error.
/// This boolean form is retained for the low-level test surface that
/// wants to assert the hash invariant directly. NO PRODUCTION CODE
/// PATH calls this any more.
[[nodiscard]] bool verify_shadow_hash(const InlineFrameState& state) noexcept;

// ============================================================================
// ShadowWitness — proof that a state's shadow currently matches the wire
// ============================================================================
//
// Part of the Witness Chain (see docs/internals/witness-chain.md).
//
// Theorem T2 (no-corruption) hinges on `compose_inline_frame` diffing
// against a shadow that genuinely reflects what the terminal is
// displaying. Today the runtime calls `verify_shadow_hash(state)` and
// branches on the bool; forgetting the check or running compose
// against a state whose shadow has drifted is a runtime-only failure
// mode that has historically slipped past review (the FNV-1a hash
// catches it, but only because somebody remembered to call the
// function).
//
// ShadowWitness lifts the precondition into the type system. It is a
// move-only token whose constructor is private; the *only* producer is
// `verify_shadow(state)` below, which returns `std::optional<
// ShadowWitness>` — `nullopt` when the shadow is poisoned, a populated
// optional when the shadow is provably intact at the moment of the
// call. A future overload of `compose_inline_frame` will take
// `ShadowWitness&&` as a required argument, making "compose without
// verifying the shadow" a compile error.
//
// Provenance binding
// ——————————————————
// Each witness carries a pointer to the InlineFrameState it was
// issued against and the hash value at issue time. compose's
// consumption re-hashes (closing the brief window between issuance
// and consumption — if the cells were mutated during that span, the
// re-hash will not match) and asserts the pointer matches the state
// it was passed alongside. A witness from state A passed to compose
// with state B fires the assertion in debug; in release the
// re-hash will catch the mismatch (state B's cells produce a
// different hash than state A recorded).
//
// Re-hash failure inside compose is treated as memory corruption
// (the cells changed between consecutive function calls within a
// single Runtime tick) and triggers std::terminate. This is a
// stronger response than the Divergent recovery path, because the
// scenario is fundamentally outside the application's control — a
// concurrent write to private memory implies either a thread-safety
// bug or a hardware error, and continuing to run is worse than
// stopping.
//
// Move-only is load-bearing: it prevents a stale witness from being
// reused across frames. Once consumed by compose, the witness's
// storage is moved-from; a second consumption is a use-after-move
// and produces a witness-with-null-state that fails the consumption
// assertion. Compilers warn on use-after-move under -Wmoved-from.

class ShadowWitness {
public:
    ShadowWitness(ShadowWitness&& o) noexcept
        : state_(o.state_), hash_at_issue_(o.hash_at_issue_) {
        o.state_ = nullptr;
        o.hash_at_issue_ = 0;
    }
    ShadowWitness& operator=(ShadowWitness&& o) noexcept {
        if (this != &o) {
            state_         = o.state_;
            hash_at_issue_ = o.hash_at_issue_;
            o.state_         = nullptr;
            o.hash_at_issue_ = 0;
        }
        return *this;
    }
    ShadowWitness(const ShadowWitness&)            = delete;
    ShadowWitness& operator=(const ShadowWitness&) = delete;

    /// True for a freshly-constructed witness, false after move-from.
    /// Consumers (compose_inline_frame_v2) assert this before use.
    [[nodiscard]] bool valid() const noexcept { return state_ != nullptr; }

    /// Pointer-identity of the state the witness was issued against.
    /// Used by compose to detect cross-state misuse.
    [[nodiscard]] const InlineFrameState* bound_to() const noexcept {
        return state_;
    }

    /// Hash value recorded at issue. compose re-hashes the state's
    /// prev_cells on consumption and compares against this value.
    [[nodiscard]] std::uint64_t hash_at_issue() const noexcept {
        return hash_at_issue_;
    }

private:
    ShadowWitness(const InlineFrameState* s, std::uint64_t h) noexcept
        : state_(s), hash_at_issue_(h) {}
    friend std::optional<ShadowWitness> verify_shadow(const InlineFrameState&) noexcept;

    const InlineFrameState* state_;
    std::uint64_t           hash_at_issue_;
};

/// The sole producer of ShadowWitness. Re-hashes `state.prev_cells`
/// over the current `prev_rows × prev_width` range and compares
/// against `state.shadow_hash`:
///
///   - Match (or fresh state with no prior hash) → returns a populated
///     optional carrying the witness. The witness binds to `state` by
///     address; passing it to compose alongside a different state is
///     a programming error (debug assertion).
///
///   - Mismatch → returns std::nullopt. The runtime must NOT call
///     `compose_inline_frame_v2` on this state; the only safe response
///     is to demote to Divergent and re-paint from a clear viewport.
///
/// Cost: O(prev_rows × prev_width) of u64 FNV-1a folding, fully
/// vectorisable by the compiler. ~1µs on a 200×80 viewport. This is
/// the same hash `compose_inline_frame` already computes and stores;
/// `verify_shadow` is the symmetric reader.
[[nodiscard]] std::optional<ShadowWitness> verify_shadow(const InlineFrameState& state) noexcept;

} // namespace maya
