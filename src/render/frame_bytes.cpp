// maya::render::frame_bytes — Implementation
//
// See include/maya/render/frame_bytes.hpp for the type-system contract.
// This file implements the wrapper around the existing
// `compose_inline_frame` that lifts the call into the Witness Chain:
// every state mutation is funneled through a return value rather than
// an out-parameter, and byte delivery is fused with state advance via
// FrameBytes::commit_to.

#include "maya/render/frame_bytes.hpp"

#include <cassert>
#include <cstdlib>
#include <utility>

namespace maya {

// ─────────────────────────────────────────────────────────────────────────
// compose_inline_frame_v2
// ─────────────────────────────────────────────────────────────────────────
//
// Wraps the legacy in-place `compose_inline_frame` with the
// witness-chain calling convention:
//
//   1. Consume the witness. Re-verify the hash against the state we
//      were handed (closes the window between verify_shadow returning
//      and this call running). A mismatch here implies the state was
//      mutated after the witness was issued but before compose
//      consumed it — fundamentally a thread-safety / memory-corruption
//      bug, since the renderer is single-threaded and the witness was
//      taken immediately upstream. std::terminate is the correct
//      response: continuing would emit bytes against a shadow that is
//      provably out of sync with the wire.
//
//   2. Verify ContentRows came from the canvas. Provenance check; in
//      debug builds asserts the source canvas matches. In release we
//      skip the check (no perf cost on hot path) but the type system
//      has already done its job: the only producer of ContentRows is
//      `content_rows(canvas)`, so unless the caller deliberately
//      lied (e.g. derived from a stale canvas reference), the check
//      is tautological.
//
//   3. Run the legacy compose with the moved-in state. The state's
//      buffers are reused; the byte buffer is freshly allocated
//      inside this function (cheap — `std::string` doubles by default,
//      and we're going to move it into FrameBytes anyway).
//
//   4. Wrap the result. The mutated state becomes the SUCCESSOR. The
//      bytes plus the successor become a FrameBytes — the linear
//      capsule the caller must consume via commit_to.

FrameBytes compose_inline_frame_v2(
    const Canvas& canvas,
    ContentRows content_rows,
    int term_h,
    const StylePool& pool,
    InlineFrameState&& state,
    ShadowWitness&& witness,
    bool synchronized_output)
{
    // (1) Witness provenance: the witness must have been issued
    //     against THIS state value. We compare by address; the caller
    //     moves the state in, so its address may differ from where the
    //     witness was minted only if the caller deliberately shuffled
    //     states between issuance and consumption. That is the bug
    //     this check is designed to catch.
    //
    //     Note: the witness bound to a `const InlineFrameState*`; the
    //     moved state is at a different address. We instead re-hash
    //     the moved state and compare with witness.hash_at_issue().
    //     If the cells survived the move (they will — AlignedBuffer's
    //     move is a pointer-swap) the hash will match.
    assert(witness.valid() && "ShadowWitness was already consumed");

    {
        // Re-hash the now-moved state's prev_cells; compare with the
        // value the witness recorded. A mismatch implies the cell
        // buffer changed between the moments verify_shadow returned
        // and compose entered — a fundamental violation that is
        // unrecoverable.
        std::uint64_t h = 14695981039346656037ULL;
        if (state.prev_width > 0 && state.prev_rows > 0 &&
            state.shadow_hash != static_cast<std::uint64_t>(-1)) {
            const std::size_t W = static_cast<std::size_t>(state.prev_width);
            const std::size_t n = static_cast<std::size_t>(state.prev_rows) * W;
            if (n <= state.prev_cells.size()) {
                const std::uint64_t* p = state.prev_cells.data();
                for (std::size_t i = 0; i < n; ++i) {
                    h ^= p[i];
                    h *= 1099511628211ULL;
                }
                if (h != witness.hash_at_issue()) {
                    // Shadow corruption between verify and consume.
                    // Cannot emit bytes safely; abort the process.
                    std::abort();
                }
            }
        }
    }

    // (2) ContentRows provenance: in debug, verify the row count was
    //     derived from THIS canvas. In release the check is a no-op
    //     because the type system already enforced single-producer
    //     discipline at the call site.
    assert((content_rows.source_canvas() == &canvas ||
            content_rows.source_canvas() == nullptr) &&
           "ContentRows was derived from a different canvas");

    // (3) Move the witness out — its lifetime is logically ended
    //     once we've consumed it. The move clears its bound pointer
    //     so any accidental reuse fires the valid() assertion.
    (void)ShadowWitness{std::move(witness)};

    // (4) Run the legacy compose. It mutates `state` in place;
    //     after this call `state` IS the successor.
    std::string out;
    out.reserve(1024);   // typical inline frame; grows as needed.
    compose_inline_frame(canvas, content_rows.value(), term_h, pool,
                         state, out, synchronized_output);

    // (5) Capsule. FrameBytes::commit_to consumes both arms.
    return FrameBytes{std::move(out), std::move(state)};
}

// ─────────────────────────────────────────────────────────────────────────
// FrameBytes::commit_to — typed atomic write
// ─────────────────────────────────────────────────────────────────────────
//
// Writer::write_or_buffer already provides the byte-level atomicity
// (residue path for partial accept, hard-error surface for I/O
// failure). Our job is to fuse that result with the state advance:
//
//   - empty FrameBytes (compose produced no diff) → trivially Synced
//     with the moved successor.
//   - non-empty + write succeeds → Synced with the moved successor.
//   - non-empty + write fails → Stale with the recovery shape.

CommitOutcome FrameBytes::commit_to(Writer& writer) && noexcept {
    if (bytes_.empty()) {
        // Compose decided no bytes need to be sent (no diff vs prev).
        // Successor state is unchanged from what compose handed us.
        return commit::Synced{std::move(successor_)};
    }

    Status st = writer.write_or_buffer(bytes_);
    if (st) {
        return commit::Synced{std::move(successor_)};
    }

    // Hard I/O error. Build the recovery state:
    //
    //   - ghost_rows_above = successor's wire_cursor_rows (the rows
    //     the failed frame was supposed to occupy; the next render's
    //     case-(B) emit will erase them).
    //   - prev_rows zeroed so the next compose enters case (B).
    //   - cursor_hidden / decawm_off cleared so terminal-mode escapes
    //     are re-emitted after the failure (host shell may have
    //     restored modes during the gap).
    //   - shadow_hash sentinel so verify_shadow on the next render
    //     treats this state as fresh (no false positives).
    //
    // The successor we built was already loaded with wire_cursor_rows
    // by compose_inline_frame — we read that value before resetting.
    InlineFrameState recovery = std::move(successor_);
    const int prior_wire_rows = recovery.wire_cursor_rows;
    recovery.ghost_rows_above = prior_wire_rows;
    recovery.prev_rows        = 0;
    recovery.cursor_hidden    = false;
    recovery.decawm_off       = false;
    recovery.shadow_hash      = static_cast<std::uint64_t>(-1);
    // Tell the writer to flush any residue + restore terminal modes
    // before the next render runs. Best-effort; ignore the result.
    writer.discard_residue();
    return commit::Stale{std::move(recovery), st};
}

// abandon: discard bytes without writing. The bytes never reached the
// wire, so the wire still reflects the PRIOR frame, not the successor
// state compose computed. The recovery shape is therefore based on
// the successor's wire_cursor_rows (which equals min(prev_rows_at_
// successor, term_h), and since we never wrote, the wire actually
// shows what the previous frame painted — but we cannot tell whether
// the previous frame fully landed, so we conservatively demote to
// Stale).
//
// In practice abandon() is called when the runtime decides between
// compose and commit_to that the frame should not ship (e.g. quit
// arrived). The next render — if any — routes through case (B) and
// repaints in place.

commit::Stale FrameBytes::abandon() && noexcept {
    InlineFrameState recovery = std::move(successor_);
    const int prior_wire_rows = recovery.wire_cursor_rows;
    recovery.ghost_rows_above = prior_wire_rows;
    recovery.prev_rows        = 0;
    recovery.cursor_hidden    = false;
    recovery.decawm_off       = false;
    recovery.shadow_hash      = static_cast<std::uint64_t>(-1);
    return commit::Stale{std::move(recovery), ok()};
}

} // namespace maya
