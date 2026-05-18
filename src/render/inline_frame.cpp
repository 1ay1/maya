// maya::render::inline_frame — Implementation of the tagged transitions
//
// See include/maya/render/inline_frame.hpp for the type-system contract.
//
// Most methods are value-move transitions or wrappers around
// `compose_inline_frame` + `FrameBytes::commit_to`. The HardReset
// path is the only one that emits a non-compose sequence directly:
// the `\x1b[2J\x1b[3J\x1b[H` wipe followed by a fresh paint.

#include "maya/render/inline_frame.hpp"
#include "maya/terminal/writer.hpp"

#include <cassert>
#include <utility>

namespace maya::inline_frame {

namespace detail {

// Common helper: dispatch a CommitOutcome (Synced/Stale/HardReset) into
// the RenderOutcome variant. Shared by every render() implementation.
RenderOutcome lift_commit_outcome(CommitOutcome outcome) noexcept {
    return std::visit([](auto&& arm) -> RenderOutcome {
        using T = std::decay_t<decltype(arm)>;
        if constexpr (std::is_same_v<T, commit::Synced>) {
            return InlineFrame<Synced>{std::move(arm.state)};
        } else if constexpr (std::is_same_v<T, commit::Stale>) {
            return InlineFrame<Stale>{std::move(arm.state)};
        } else {
            static_assert(std::is_same_v<T, commit::HardReset>);
            return InlineFrame<HardReset>{};
        }
    }, std::move(outcome));
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────
// Empty → Fresh / Sealed
// ─────────────────────────────────────────────────────────────────────────

InlineFrame<Fresh> InlineFrame<Empty>::seed() && noexcept {
    return InlineFrame<Fresh>{};
}

InlineFrame<Sealed> InlineFrame<Empty>::finalize(std::string&) && noexcept {
    return InlineFrame<Sealed>{};
}

// ─────────────────────────────────────────────────────────────────────────
// Fresh → Synced / Stale / HardReset (first render)
// ─────────────────────────────────────────────────────────────────────────
//
// State's prev_width == 0 routes compose into case (A): emit from
// cursor's current position via serialize(). Host content above the
// cursor stays visible.

RenderOutcome InlineFrame<Fresh>::render(
    const Canvas& canvas,
    ContentRows rows,
    int term_h,
    const StylePool& pool,
    Writer& writer,
    bool synchronized_output) &&
{
    auto wit = verify_shadow(state_);
    assert(wit.has_value() && "verify_shadow must accept a Fresh state");

    FrameBytes capsule = compose_inline_frame(
        canvas, rows, term_h, pool,
        std::move(state_), *std::move(wit),
        synchronized_output);

    return detail::lift_commit_outcome(std::move(capsule).commit_to(writer));
}

InlineFrame<Sealed> InlineFrame<Fresh>::finalize(std::string& out) && noexcept {
    auto fin = std::move(state_).finalize();
    out.append(fin.restore_bytes);
    return InlineFrame<Sealed>{};
}

// ─────────────────────────────────────────────────────────────────────────
// Synced → Synced / Stale / HardReset (incremental render)
// ─────────────────────────────────────────────────────────────────────────

RenderOutcome InlineFrame<Synced>::render(
    const Canvas& canvas,
    ContentRows rows,
    int term_h,
    const StylePool& pool,
    Writer& writer,
    ShadowWitness&& witness,
    bool synchronized_output) &&
{
    FrameBytes capsule = compose_inline_frame(
        canvas, rows, term_h, pool,
        std::move(state_), std::move(witness),
        synchronized_output);

    return detail::lift_commit_outcome(std::move(capsule).commit_to(writer));
}

InlineFrame<Stale> InlineFrame<Synced>::demote_to_stale() && noexcept {
    // Soft demotion: prepare for case-(B) recovery. The wire's
    // content is plausibly the last frame we painted; the next
    // render walks the cursor up and re-paints in place, erasing
    // below. The shape transformation is a pure factory on the
    // state — no aliases survive across this call.
    return InlineFrame<Stale>{
        std::move(state_).abandoned_for_recovery()
    };
}

InlineFrame<HardReset> InlineFrame<Synced>::demote_to_hard_reset() && noexcept {
    // Hard demotion: the wire's content is unknown. The next render
    // emits a full wipe and starts fresh; we drop our state entirely
    // because none of its invariants can be trusted.
    return InlineFrame<HardReset>{};
}

InlineFrame<Sealed> InlineFrame<Synced>::finalize(std::string& out) && noexcept {
    auto fin = std::move(state_).finalize();
    out.append(fin.restore_bytes);
    return InlineFrame<Sealed>{};
}

// ─────────────────────────────────────────────────────────────────────────
// Stale → Synced / Stale / HardReset (soft recovery render)
// ─────────────────────────────────────────────────────────────────────────
//
// State's prev_rows == 0 and ghost_rows_above > 0 route compose into
// case (B): walk cursor up, paint in place, erase below. No
// scrollback wipe.

RenderOutcome InlineFrame<Stale>::render(
    const Canvas& canvas,
    ContentRows rows,
    int term_h,
    const StylePool& pool,
    Writer& writer,
    bool synchronized_output) &&
{
    auto wit = verify_shadow(state_);
    assert(wit.has_value() && "verify_shadow must accept a Stale state");

    FrameBytes capsule = compose_inline_frame(
        canvas, rows, term_h, pool,
        std::move(state_), *std::move(wit),
        synchronized_output);

    return detail::lift_commit_outcome(std::move(capsule).commit_to(writer));
}

InlineFrame<HardReset> InlineFrame<Stale>::escalate_to_hard_reset() && noexcept {
    // The Stale render failed too. Drop the state; emit a wipe next.
    return InlineFrame<HardReset>{};
}

InlineFrame<Sealed> InlineFrame<Stale>::finalize(std::string& out) && noexcept {
    auto fin = std::move(state_).finalize();
    out.append(fin.restore_bytes);
    return InlineFrame<Sealed>{};
}

// ─────────────────────────────────────────────────────────────────────────
// HardReset → Synced / Stale / HardReset (full reset render)
// ─────────────────────────────────────────────────────────────────────────
//
// Emit `\x1b[2J\x1b[3J\x1b[H` to wipe the viewport AND saved-lines,
// homing the cursor. Then run a fresh case-(A) paint by routing
// through Fresh. This path is destructive to host scrollback above
// the viewport — appropriate only when the wire is genuinely in an
// unknown state (post-resize, post-write-failure).

RenderOutcome InlineFrame<HardReset>::render(
    const Canvas& canvas,
    ContentRows rows,
    int term_h,
    const StylePool& pool,
    Writer& writer,
    bool synchronized_output) &&
{
    // 1. Wipe sequence. If this write fails, we stay in HardReset and
    //    the next render retries.
    {
        Status st = writer.write_or_buffer("\x1b[2J\x1b[3J\x1b[H");
        if (!st) {
            writer.discard_residue();
            return InlineFrame<HardReset>{};
        }
    }

    // 2. Fresh-state paint (case A). Build a Fresh and run its render.
    InlineFrame<Fresh> fresh{};
    return std::move(fresh).render(canvas, rows, term_h, pool, writer,
                                   synchronized_output);
}

InlineFrame<Sealed> InlineFrame<HardReset>::finalize(std::string&) && noexcept {
    return InlineFrame<Sealed>{};
}

} // namespace maya::inline_frame
