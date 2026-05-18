#pragma once
// maya::render::frame_bytes — Atomic seal between compose and writer
//
// ─────────────────────────────────────────────────────────────────────────
// Part of the Witness Chain (see docs/internals/witness-chain.md).
// This module discharges the atomicity half of Theorem T2: the state's
// advance (prev_cells, prev_rows, shadow_hash, wire_cursor_rows) is
// inseparable from the byte delivery — either both happen or neither.
// ─────────────────────────────────────────────────────────────────────────
//
// Background. Previously the inline render path was three uncorrelated
// steps:
//
//   compose_into_buf(canvas, ..., state, /*out*/ buf, ...);
//   auto status = writer.write_or_buffer(buf);
//   if (!status) { state.shadow_hash = -1; state.prev_rows = 0; ... }
//
// The compose mutated `state` in place to reflect the bytes it just
// produced; `write_or_buffer` then attempted to ship those bytes to
// the kernel. If the kernel returned a hard error, the state had
// already advanced to "we just rendered frame N" while the wire was
// still at frame N-1 (or worse, halfway between them). The recovery
// code in `app.cpp` patched this by manually resetting state fields
// on write failure, but the *type* of the state never indicated
// "this advancement is provisional until the bytes ship."
//
// FrameBytes inverts the relationship. `compose_inline_frame` no
// longer mutates the state in place; it returns a FrameBytes value
// carrying:
//
//   - the byte buffer to write
//   - the SUCCESSOR state value (what `state` should become if the
//     bytes ship intact)
//
// `FrameBytes::commit_to(writer)` is the *only* way to extract the
// bytes. It writes them atomically (drained residue + fresh bytes +
// any new residue is encapsulated by Writer::write_or_buffer) and
// returns:
//
//   - the successor state on full delivery, OR
//   - a Stale state (with prev_rows = 0, shadow_hash = sentinel,
//     ghost_rows_above set from the prior wire_cursor_rows) on
//     hard I/O failure.
//
// The return type is `std::variant<InlineFrameState, InlineFrameState>`
// tagged via a `Outcome` strong enum, OR a `CommitOutcome` aggregate
// that the caller destructures. We use the aggregate form (rather than
// a variant of the same type) so that the *intent* of each arm is
// readable at call sites: `outcome.synced_state` vs
// `outcome.stale_state` makes the success vs failure intent explicit
// without requiring a discriminator field on InlineFrameState.
//
// Linear discipline
// ─────────────────
// FrameBytes is move-only. `commit_to(writer) &&` rvalue-qualifies on
// the consumer; once consumed, the FrameBytes is moved-from and
// inaccessible. There is no copy constructor, no `bytes()` accessor
// that returns a public reference, no way to "look at" the bytes
// without consuming them.
//
// `abandon()` is the explicit drop-path: discards the bytes, returns a
// Stale state. Used when the caller decides at the last moment not to
// commit (e.g. quit signal arrived between compose and write).
//
// Cost
// ────
// Zero on the hot path. The FrameBytes carries an `std::string` (moved
// from compose's internal buffer) and an `InlineFrameState` (moved by
// value — AlignedBuffer is already move-only and cheap to move). No
// new allocations beyond what the prior path already paid; the
// `out_` string is allocated once on the Runtime and reused via move.

#include <string>
#include <utility>
#include <variant>

#include "../core/expected.hpp"
#include "../terminal/writer.hpp"
#include "serialize.hpp"

namespace maya {

// ─────────────────────────────────────────────────────────────────────────
// commit::Synced / commit::Stale — typed result arms of FrameBytes::commit_to
// ─────────────────────────────────────────────────────────────────────────
//
// One of two states is alive at any time. The variant index encodes
// which arm the caller's std::visit must handle:
//
//   - index 0 (Synced): full byte delivery; carries the SUCCESSOR
//     InlineFrameState (prev_cells/rows updated, fresh shadow_hash).
//   - index 1 (Stale): hard I/O error during write; carries the
//     RECOVERY InlineFrameState (prev_rows zeroed, ghost_rows_above
//     set to the prior wire_cursor_rows so case-(B) redraw walks the
//     correct upward erase distance).
//
// std::variant forces the caller to handle both via visit; structured
// bindings on a variant are a compile error, which is the point — a
// "happy-path-only" call site is uncompilable. The recovery state
// always provides a valid starting point for the next render's
// case-(B) emit, so there is no third "give up" branch.
//
// Named in the `commit::` sub-namespace so they don't collide with
// the `inline_frame::Synced` / `inline_frame::Stale` phantom tags
// used by the InlineFrame<Tag> type-state in inline_frame.hpp.

namespace commit {
    struct Synced     { InlineFrameState state; };
    struct Stale      { InlineFrameState state; Status reason; };
    struct HardReset  { Status reason; };
}

using CommitOutcome = std::variant<commit::Synced,
                                   commit::Stale,
                                   commit::HardReset>;

// ─────────────────────────────────────────────────────────────────────────
// FrameBytes — pre-committed bytes + their successor state
// ─────────────────────────────────────────────────────────────────────────
//
// Constructed only by `compose_inline_frame` (friend). Consumed only
// by `commit_to(writer) &&` or `abandon() &&`. The byte buffer is
// private; there is no public way to inspect or copy it.

class [[nodiscard("FrameBytes is a linear capsule — dropping it without commit_to() or abandon() ships nothing while logically advancing the renderer's state, desynchronising the shadow from the wire")]] FrameBytes {
public:
    FrameBytes(const FrameBytes&)            = delete;
    FrameBytes& operator=(const FrameBytes&) = delete;
    FrameBytes(FrameBytes&&) noexcept            = default;
    FrameBytes& operator=(FrameBytes&&) noexcept = default;

    /// Number of bytes in this frame. Read-only; useful for logging
    /// and bandwidth profiling. Does not expose buffer contents.
    [[nodiscard]] std::size_t byte_count() const noexcept {
        return bytes_.size();
    }

    /// True iff the compose produced an empty frame (no diff vs prev).
    /// commit_to on an empty FrameBytes is a fast no-op that returns
    /// the successor state unchanged.
    [[nodiscard]] bool empty() const noexcept { return bytes_.empty(); }

    /// Consume the FrameBytes and ship the bytes via `writer`.
    /// Returns a CommitOutcome variant:
    ///   - Synced{...} on full byte delivery (whatever the writer
    ///     accepted into its residue queue is treated as delivered;
    ///     the residue path is part of the atomic semantics — see
    ///     Writer::write_or_buffer's contract).
    ///   - Stale{...} on hard I/O error. The carried state is the
    ///     recovery shape: prev_rows zeroed, ghost_rows_above set,
    ///     shadow_hash sentinel'd. The next render must route through
    ///     case (B) to soft-redraw in place.
    [[nodiscard]] CommitOutcome commit_to(Writer& writer) && noexcept;

    /// Consume the FrameBytes without writing. Used when the caller
    /// decides — after compose, before write — not to commit (e.g.
    /// shutdown signal in flight). Returns a Stale state so the next
    /// render forces a full redraw; the prior state's invariants are
    /// preserved on the wire because nothing was emitted.
    [[nodiscard]] commit::Stale abandon() && noexcept;

    /// Convenience: drop the bytes and synthesize a HardReset (e.g.
    /// when the runtime detected a write failure out-of-band and is
    /// abandoning this frame's bytes as part of the recovery).
    [[nodiscard]] commit::HardReset abandon_to_hard_reset(Status reason) && noexcept;

private:
    FrameBytes(std::string bytes, InlineFrameState successor) noexcept
        : bytes_(std::move(bytes)), successor_(std::move(successor)) {}

    friend FrameBytes compose_inline_frame(
        const Canvas&, ContentRows, TermRows, const StylePool&,
        InlineFrameState&&, ShadowWitness&&, bool);

    std::string      bytes_;
    InlineFrameState successor_;
};

// ─────────────────────────────────────────────────────────────────────────
// compose_inline_frame — the witness-chain compose
// ─────────────────────────────────────────────────────────────────────────
//
// New signature, witness-chain version:
//
//   - Canvas&           — the painted canvas.
//   - ContentRows       — row count derived from THIS canvas (typed
//                         witness; see serialize.hpp).
//   - TermRows          — terminal viewport height (typed witness;
//                         see serialize.hpp). The sole production
//                         producer is `query_term_rows(handle)` —
//                         a raw int from a stored cache is
//                         uncompilable.
//   - StylePool&        — interning pool (unchanged).
//   - InlineFrameState&& — consumed by value-move. The caller transfers
//                         ownership of the state to compose; what they
//                         get back via FrameBytes::commit_to is the
//                         successor.
//   - ShadowWitness&&   — consumed; proves the shadow was verified
//                         immediately before this call (see
//                         serialize.hpp).
//   - bool synchronized_output — DEC mode 2026 wrapping.
//
// Returns FrameBytes carrying the byte stream and the successor state.
// The caller invokes `.commit_to(writer)` on the result to ship the
// bytes and receive the typed outcome. This is the only public compose
// entrypoint — there is no in-place mutating form.
[[nodiscard]] FrameBytes compose_inline_frame(
    const Canvas& canvas,
    ContentRows content_rows,
    TermRows term_h,
    const StylePool& pool,
    InlineFrameState&& state,
    ShadowWitness&& witness,
    bool synchronized_output = true);

} // namespace maya
