// maya::render::inline_frame — Implementation of the tagged transitions
//
// See include/maya/render/inline_frame.hpp for the type-system contract.
//
// This file is small by design: every method is either a value-move
// transition (cheap; AlignedBuffer's move is pointer-swap) or a wrapper
// around the existing compose_inline_frame_v2 / FrameBytes::commit_to
// chain. The witness chain's machinery lives upstream; this layer is
// the type-state glue that makes the chain's preconditions structural.

#include "maya/render/inline_frame.hpp"

#include <cassert>
#include <utility>

namespace maya::inline_frame {

// ─────────────────────────────────────────────────────────────────────────
// Empty → Fresh / Sealed
// ─────────────────────────────────────────────────────────────────────────

InlineFrame<Fresh> InlineFrame<Empty>::seed() && noexcept {
    // Fresh starts with a default-constructed InlineFrameState.
    // prev_width == 0 routes compose into case (A) on first render.
    return InlineFrame<Fresh>{};
}

InlineFrame<Sealed> InlineFrame<Empty>::finalize(std::string&) && noexcept {
    return InlineFrame<Sealed>{};
}

// ─────────────────────────────────────────────────────────────────────────
// Fresh → Synced / Stale (first render)
// ─────────────────────────────────────────────────────────────────────────
//
// The first render emits via compose's case (A): serialize from the
// cursor's current position, growing the frame downward via row
// separators. The state's prev_width == 0 routes compose down this
// path automatically.

RenderOutcome InlineFrame<Fresh>::render(
    const Canvas& canvas,
    ContentRows rows,
    int term_h,
    const StylePool& pool,
    Writer& writer,
    bool synchronized_output) &&
{
    // Synthesize a witness that trivially passes — the Fresh state has
    // no shadow to verify against (shadow_hash is the sentinel). The
    // verify_shadow function returns a populated optional for fresh
    // states; we extract and consume it.
    auto wit = verify_shadow(state_);
    assert(wit.has_value() && "verify_shadow must accept a Fresh state");

    FrameBytes capsule = compose_inline_frame_v2(
        canvas, rows, term_h, pool,
        std::move(state_), *std::move(wit),
        synchronized_output);

    auto outcome = std::move(capsule).commit_to(writer);
    return std::visit([](auto&& arm) -> RenderOutcome {
        using T = std::decay_t<decltype(arm)>;
        if constexpr (std::is_same_v<T, maya::commit::Synced>) {
            return InlineFrame<Synced>{std::move(arm.state)};
        } else {
            return InlineFrame<Stale>{std::move(arm.state)};
        }
    }, std::move(outcome));
}

InlineFrame<Sealed> InlineFrame<Fresh>::finalize(std::string& out) && noexcept {
    state_.finalize(out);
    return InlineFrame<Sealed>{};
}

// ─────────────────────────────────────────────────────────────────────────
// Synced → Synced / Stale (incremental render)
// ─────────────────────────────────────────────────────────────────────────
//
// The witness is required by signature. The caller obtained it from
// verify() and is consuming it here. compose_inline_frame_v2's
// internal re-verification closes the issuance-to-consumption window.

RenderOutcome InlineFrame<Synced>::render(
    const Canvas& canvas,
    ContentRows rows,
    int term_h,
    const StylePool& pool,
    Writer& writer,
    ShadowWitness&& witness,
    bool synchronized_output) &&
{
    FrameBytes capsule = compose_inline_frame_v2(
        canvas, rows, term_h, pool,
        std::move(state_), std::move(witness),
        synchronized_output);

    auto outcome = std::move(capsule).commit_to(writer);
    return std::visit([](auto&& arm) -> RenderOutcome {
        using T = std::decay_t<decltype(arm)>;
        if constexpr (std::is_same_v<T, maya::commit::Synced>) {
            return InlineFrame<Synced>{std::move(arm.state)};
        } else {
            return InlineFrame<Stale>{std::move(arm.state)};
        }
    }, std::move(outcome));
}

InlineFrame<Stale> InlineFrame<Synced>::demote_to_stale() && noexcept {
    // Move into Stale, applying the force-redraw recovery shape:
    // ghost_rows_above captures wire_cursor_rows so the next case-(B)
    // render erases the prior frame's territory before painting the
    // new frame.
    InlineFrameState s = std::move(state_);
    s.ghost_rows_above = s.wire_cursor_rows;
    s.prev_rows        = 0;
    s.cursor_hidden    = false;
    s.decawm_off       = false;
    s.shadow_hash      = static_cast<std::uint64_t>(-1);
    return InlineFrame<Stale>{std::move(s)};
}

InlineFrame<Sealed> InlineFrame<Synced>::finalize(std::string& out) && noexcept {
    state_.finalize(out);
    return InlineFrame<Sealed>{};
}

// ─────────────────────────────────────────────────────────────────────────
// Stale → Synced / Stale (recovery render)
// ─────────────────────────────────────────────────────────────────────────
//
// The state's prev_rows == 0 and ghost_rows_above > 0 route compose
// down case (B). No witness is required because there's nothing to
// verify — the shadow was already declared poisoned by entry into
// Stale.

RenderOutcome InlineFrame<Stale>::render(
    const Canvas& canvas,
    ContentRows rows,
    int term_h,
    const StylePool& pool,
    Writer& writer,
    bool synchronized_output) &&
{
    // Synthesize a fresh witness — for a state with shadow_hash = -1
    // verify_shadow returns a populated optional unconditionally.
    auto wit = verify_shadow(state_);
    assert(wit.has_value() && "verify_shadow must accept a Stale state");

    FrameBytes capsule = compose_inline_frame_v2(
        canvas, rows, term_h, pool,
        std::move(state_), *std::move(wit),
        synchronized_output);

    auto outcome = std::move(capsule).commit_to(writer);
    return std::visit([](auto&& arm) -> RenderOutcome {
        using T = std::decay_t<decltype(arm)>;
        if constexpr (std::is_same_v<T, maya::commit::Synced>) {
            return InlineFrame<Synced>{std::move(arm.state)};
        } else {
            return InlineFrame<Stale>{std::move(arm.state)};
        }
    }, std::move(outcome));
}

InlineFrame<Sealed> InlineFrame<Stale>::finalize(std::string& out) && noexcept {
    state_.finalize(out);
    return InlineFrame<Sealed>{};
}

} // namespace maya::inline_frame
