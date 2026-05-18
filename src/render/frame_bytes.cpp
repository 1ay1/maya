// maya::render::frame_bytes — Implementation
//
// See include/maya/render/frame_bytes.hpp for the type-system contract.
// This file implements the witness-chain `compose_inline_frame`: every
// state mutation is funneled through a return value rather than an
// out-parameter, and byte delivery is fused with state advance via
// FrameBytes::commit_to.

#include "maya/render/frame_bytes.hpp"

#include <cassert>
#include <cstdlib>
#include <utility>

namespace maya {

// Internal byte-emitter, defined in serialize.cpp. The witness-chain
// `compose_inline_frame` below is the only caller; it drives this
// helper after consuming a ShadowWitness so the in-place mutation it
// performs (on its own moved-in local) is safe under the chain's
// invariants. There is no public header for this symbol — callers
// outside this TU cannot reach it.
std::pair<std::string, InlineFrameState>
compose_inline_frame_impl(const Canvas& canvas,
                          int content_rows,
                          int term_h,
                          const StylePool& pool,
                          InlineFrameState&& state_in,
                          bool synchronized_output);

// ─────────────────────────────────────────────────────────────────────────
// compose_inline_frame
// ─────────────────────────────────────────────────────────────────────────
//
// Drives the internal byte-emitter with the witness-chain calling
// convention:
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

FrameBytes compose_inline_frame(
    const Canvas& canvas,
    ContentRows content_rows,
    TermRows term_h,
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
        if (state.prev_width_ > 0 && state.prev_rows_ > 0 &&
            state.shadow_hash_ != static_cast<std::uint64_t>(-1)) {
            const std::size_t W = static_cast<std::size_t>(state.prev_width_);
            const std::size_t n = static_cast<std::size_t>(state.prev_rows_) * W;
            if (n <= state.prev_cells_.size()) {
                const std::uint64_t* p = state.prev_cells_.data();
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

    // (4) Drive the internal byte-emitter. It takes the state by
    //     &&-move, mutates a function-local owned copy, and returns
    //     `(bytes, successor)` — a pure transformation in the type
    //     system's eyes. We have no observable state mutation to
    //     reason about; the byte buffer and the new state value are
    //     simply the result of a function call.
    auto [out, successor] = compose_inline_frame_impl(
        canvas, content_rows.value(), term_h.value(), pool,
        std::move(state), synchronized_output);

    // (5) Capsule. FrameBytes::commit_to consumes both arms.
    return FrameBytes{std::move(out), std::move(successor)};
}

// ─────────────────────────────────────────────────────────────────────────
// FrameBytes::commit_to — typed atomic write
// ─────────────────────────────────────────────────────────────────────────
//
// Writer::write_or_buffer provides byte-level atomicity: a residue
// path for partial writes, and a hard-error surface for genuine I/O
// failure. Our job is to fuse that result with the state advance:
//
//   - empty FrameBytes (compose produced no diff) → trivially Synced
//     with the moved successor.
//   - non-empty + write succeeds → Synced with the moved successor.
//   - non-empty + write fails hard → HardReset (no carried state).
//     The wire is in an unknown state after a mid-frame I/O failure;
//     the only safe next move is the \x1b[2J\x1b[3J\x1b[H wipe
//     emitted by InlineFrame<HardReset>::render. We DROP the
//     in-flight successor because none of its invariants hold any
//     more (prev_cells now reflects bytes the wire never received).

CommitOutcome FrameBytes::commit_to(Writer& writer) && noexcept {
    if (bytes_.empty()) {
        return commit::Synced{std::move(successor_)};
    }

    Status st = writer.write_or_buffer(bytes_);
    if (st) {
        return commit::Synced{std::move(successor_)};
    }

    // Hard I/O error. Drop the residue (it would re-send bytes the
    // wire might have received partially) and surface HardReset so
    // the next render emits the wipe sequence and starts fresh.
    writer.discard_residue();
    return commit::HardReset{st};
}

// abandon: discard bytes without writing. The bytes never reached the
// wire, so the wire still shows what the prior frame painted — but
// our model (`successor_`) was already updated to reflect a frame the
// wire never received. Demote to Stale: the next render goes through
// case (B) and soft-redraws in place. This is the right shape because
// the wire's content is plausibly the prior frame's; the recovery
// just needs to refresh the cells the canvas now describes.

commit::Stale FrameBytes::abandon() && noexcept {
    // Type-theoretic shape: we receive the successor by value-move
    // (no aliases survive), pass it through the recovery factory
    // (a pure `State -> State` operation that returns the post-
    // abandon shape), and wrap it in the typed arm. The outer
    // caller observes only `commit::Stale{new_state}` — no mutation
    // of any object they hold a reference to.
    return commit::Stale{
        std::move(successor_).abandoned_for_recovery(),
        ok(),
    };
}

// abandon_to_hard_reset: the runtime has decided that the wire is
// genuinely in an unknown state (e.g. it observed a write error from
// a parallel codepath) and the bytes we're holding are inappropriate
// to send. Drop them; surface HardReset.
commit::HardReset FrameBytes::abandon_to_hard_reset(Status reason) && noexcept {
    return commit::HardReset{reason};
}

} // namespace maya
