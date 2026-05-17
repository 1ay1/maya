#pragma once
// maya::render::inline_frame — Tagged type-state for the inline renderer
//
// ─────────────────────────────────────────────────────────────────────────
// Part of the Witness Chain (see docs/internals/witness-chain.md). This
// module is the apex of the chain: every legal state of the inline
// renderer is a distinct type; every legal transition is a function
// whose signature only accepts the predecessor tag; every illegal
// transition (compose-after-finalize, render-without-verify, etc.) is
// a compile error.
// ─────────────────────────────────────────────────────────────────────────
//
// The five tags
// ─────────────
//   InlineFrame<Empty>   — freshly constructed, no frame ever rendered.
//                          Only legal next move: into Fresh via `seed()`.
//   InlineFrame<Fresh>   — about to render the very first frame.
//                          Compose is allowed; case (A) path inside
//                          compose_inline_frame fires.
//   InlineFrame<Synced>  — last frame committed; shadow matches wire.
//                          Compose requires a ShadowWitness obtained
//                          from `verify()`.
//   InlineFrame<Stale>   — last attempt failed (write error, force_redraw,
//                          short-write detected, shadow hash poisoned).
//                          Compose enters case (B) soft-redraw.
//   InlineFrame<Sealed>  — `finalize()` consumed the state. Compose
//                          is no longer possible; render-after-finalize
//                          is a compile error.
//
// The legal transitions
// ─────────────────────
//                       seed()
//   InlineFrame<Empty>  ────────▶  InlineFrame<Fresh>
//   InlineFrame<Fresh>  ─render─▶  InlineFrame<Synced>          (case A)
//                              \─▶  InlineFrame<Stale>
//   InlineFrame<Synced> ─verify─▶  std::optional<ShadowWitness>
//                       ─render─▶  InlineFrame<Synced>          (Synced + witness)
//                              \─▶  InlineFrame<Stale>          (write fail)
//   InlineFrame<Synced> ─demote_to_stale()─▶ InlineFrame<Stale>
//   InlineFrame<Stale>  ─render─▶  InlineFrame<Synced>          (case B)
//                              \─▶  InlineFrame<Stale>
//   InlineFrame<*>      ─finalize(out)─▶ InlineFrame<Sealed>
//
// Each transition is a free function or member that consumes the
// predecessor by value-move and returns the successor by value. The
// underlying InlineFrameState (carrying prev_cells, prev_rows, etc.) is
// moved through every transition; AlignedBuffer's pointer-swap move
// makes this cheap.
//
// Render entry point
// ──────────────────
// To render against a Synced state, callers must follow this exact
// shape:
//
//   InlineFrame<Synced> s = ...;
//   auto wit = s.verify();
//   if (!wit) { s = std::move(s).demote_to_stale(); /* render Stale */ }
//   else {
//       auto outcome = std::move(s).render(canvas, term_h, pool,
//                                          *std::move(wit),
//                                          synchronized_output);
//       // outcome is std::variant<Synced, Stale> — must visit
//   }
//
// Skipping verify is uncompilable: `Synced::render` takes a
// ShadowWitness&& by value. Constructing one without going through
// verify() is impossible because ShadowWitness's constructor is
// friended only to verify_shadow.
//
// Holding two views of the state simultaneously is impossible:
// transitions consume by move; the source is moved-from and using it
// is detected by compilers under -Wmoved-from.
//
// Cost
// ────
// Zero. The tags are empty types; the wrappers carry the same
// InlineFrameState by value as today. Move semantics ensure no
// copying. The variant return type compiles to a tagged union with
// no allocations.
//
// Compatibility
// ─────────────
// This is purely ADDITIVE. The raw `InlineFrameState`,
// `compose_inline_frame`, and `verify_shadow_hash` remain available
// for tests and gradual migration. The Runtime will migrate to
// InlineFrame<Tag> incrementally; until it does, the type system's
// safety claims hold for code that opted in.

#include <cassert>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "frame_bytes.hpp"
#include "serialize.hpp"

namespace maya::inline_frame {

// ─────────────────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────

template <class Tag> class InlineFrame;

struct Empty  {};
struct Fresh  {};
struct Synced {};
struct Stale  {};
struct Sealed {};

// ─────────────────────────────────────────────────────────────────────────
// RenderOutcome — variant of (Synced, Stale) returned by render()
// ─────────────────────────────────────────────────────────────────────────
//
// The two arms encode the entire space of post-render states. Sealed
// is not a possible outcome of render; rendering a Sealed state is a
// compile error (no render method exists on InlineFrame<Sealed>).
// Empty / Fresh are not outcomes either — the only paths INTO Synced
// or Stale are through render.

using RenderOutcome = std::variant<InlineFrame<Synced>, InlineFrame<Stale>>;

// ─────────────────────────────────────────────────────────────────────────
// InlineFrame<Empty>
// ─────────────────────────────────────────────────────────────────────────
//
// The starting state. Holds no useful state. The only thing you can
// do with it is `seed()` it into Fresh, ready for the first compose.

template <>
class InlineFrame<Empty> {
public:
    InlineFrame() noexcept = default;
    InlineFrame(const InlineFrame&)            = delete;
    InlineFrame& operator=(const InlineFrame&) = delete;
    InlineFrame(InlineFrame&&) noexcept            = default;
    InlineFrame& operator=(InlineFrame&&) noexcept = default;

    /// Transition into Fresh, ready for the first render.
    [[nodiscard]] InlineFrame<Fresh> seed() && noexcept;

    /// Transition directly into Sealed without ever rendering. Used
    /// when a Runtime is constructed but immediately destroyed.
    [[nodiscard]] InlineFrame<Sealed> finalize(std::string& /*out*/) && noexcept;
};

// ─────────────────────────────────────────────────────────────────────────
// InlineFrame<Fresh>
// ─────────────────────────────────────────────────────────────────────────
//
// First-ever render. Compose's case (A) handles this: emit from
// cursor's current position via serialize(). No diff happens because
// there's nothing to diff against.

template <>
class InlineFrame<Fresh> {
public:
    InlineFrame(const InlineFrame&)            = delete;
    InlineFrame& operator=(const InlineFrame&) = delete;
    InlineFrame(InlineFrame&&) noexcept            = default;
    InlineFrame& operator=(InlineFrame&&) noexcept = default;

    /// Run the first-ever render. Returns a RenderOutcome.
    [[nodiscard]] RenderOutcome render(
        const Canvas& canvas,
        ContentRows rows,
        int term_h,
        const StylePool& pool,
        Writer& writer,
        bool synchronized_output = true) &&;

    /// Finalize without rendering. Restores terminal state owned by
    /// the inline frame (DECAWM, cursor visibility) into `out`.
    [[nodiscard]] InlineFrame<Sealed> finalize(std::string& out) && noexcept;

private:
    InlineFrame() noexcept = default;
    explicit InlineFrame(InlineFrameState s) noexcept : state_(std::move(s)) {}
    friend class InlineFrame<Empty>;
    friend class InlineFrame<Synced>;
    friend class InlineFrame<Stale>;

    InlineFrameState state_;
};

// ─────────────────────────────────────────────────────────────────────────
// InlineFrame<Synced>
// ─────────────────────────────────────────────────────────────────────────
//
// Last render succeeded; the shadow matches what the terminal shows.
// To render against this state, you must first call `verify()` to
// obtain a ShadowWitness, then pass that witness to `render()`. The
// witness is move-only and consumed by render, so it cannot be reused.

template <>
class InlineFrame<Synced> {
public:
    InlineFrame(const InlineFrame&)            = delete;
    InlineFrame& operator=(const InlineFrame&) = delete;
    InlineFrame(InlineFrame&&) noexcept            = default;
    InlineFrame& operator=(InlineFrame&&) noexcept = default;

    /// Recompute the shadow hash and check it against the recorded
    /// value. Returns std::nullopt iff the shadow has been mutated
    /// outside the renderer (host bug, memory corruption, …).
    [[nodiscard]] std::optional<ShadowWitness> verify() const noexcept {
        return verify_shadow(state_);
    }

    /// Render a new frame. Consumes the witness; returns Synced on
    /// successful write, Stale on hard I/O error. The variant must
    /// be visited.
    [[nodiscard]] RenderOutcome render(
        const Canvas& canvas,
        ContentRows rows,
        int term_h,
        const StylePool& pool,
        Writer& writer,
        ShadowWitness&& witness,
        bool synchronized_output = true) &&;

    /// Explicit demotion to Stale without rendering. Used by the
    /// runtime when an external event (resize, force_redraw, write
    /// failure detected elsewhere) invalidates the shadow.
    [[nodiscard]] InlineFrame<Stale> demote_to_stale() && noexcept;

    /// Commit `rows` rows of scrollback. Shifts prev_cells up and
    /// decrements prev_rows. Returns a new Synced with the shifted
    /// state; the old state is consumed. Over-commit (rows >=
    /// prev_rows) yields an Empty-equivalent Synced (prev_rows = 0).
    [[nodiscard]] InlineFrame<Synced> commit(ScrollbackMarker marker) && noexcept {
        state_.commit(marker);
        return InlineFrame<Synced>{std::move(state_)};
    }

    /// Issue a marker for `rows` rows of the current frame, clamped
    /// to [0, prev_rows]. Read-only on the state; the marker is the
    /// linear token consumed by commit().
    [[nodiscard]] ScrollbackMarker scrollback_marker(int rows) const noexcept {
        return state_.scrollback_marker(rows);
    }

    /// Read-only access to row/width for diagnostics and host code
    /// that needs to know the frame's dimensions (e.g. to decide
    /// whether to virtualize old content).
    [[nodiscard]] int rows()  const noexcept { return state_.prev_rows; }
    [[nodiscard]] int width() const noexcept { return state_.prev_width; }
    [[nodiscard]] int wire_cursor_rows() const noexcept {
        return state_.wire_cursor_rows;
    }

    /// Restore terminal state and transition to Sealed.
    [[nodiscard]] InlineFrame<Sealed> finalize(std::string& out) && noexcept;

private:
    InlineFrame() noexcept = default;
    explicit InlineFrame(InlineFrameState s) noexcept : state_(std::move(s)) {}
    friend class InlineFrame<Empty>;
    friend class InlineFrame<Fresh>;
    friend class InlineFrame<Stale>;

    InlineFrameState state_;
};

// ─────────────────────────────────────────────────────────────────────────
// InlineFrame<Stale>
// ─────────────────────────────────────────────────────────────────────────
//
// Last operation left the wire in an unknown state. The next render
// goes through compose's case (B) soft-redraw: it walks the cursor
// to the new frame's top, paints in place, erases below. Recovers to
// Synced on success.

template <>
class InlineFrame<Stale> {
public:
    InlineFrame(const InlineFrame&)            = delete;
    InlineFrame& operator=(const InlineFrame&) = delete;
    InlineFrame(InlineFrame&&) noexcept            = default;
    InlineFrame& operator=(InlineFrame&&) noexcept = default;

    /// Re-render from a stale state. Compose's case (B) handles this:
    /// soft redraw in place using ghost_rows_above to size the
    /// upward erase. Returns Synced on success, Stale on repeated
    /// failure.
    [[nodiscard]] RenderOutcome render(
        const Canvas& canvas,
        ContentRows rows,
        int term_h,
        const StylePool& pool,
        Writer& writer,
        bool synchronized_output = true) &&;

    [[nodiscard]] int rows()  const noexcept { return state_.prev_rows; }
    [[nodiscard]] int width() const noexcept { return state_.prev_width; }

    [[nodiscard]] InlineFrame<Sealed> finalize(std::string& out) && noexcept;

private:
    InlineFrame() noexcept = default;
    explicit InlineFrame(InlineFrameState s) noexcept : state_(std::move(s)) {}
    friend class InlineFrame<Empty>;
    friend class InlineFrame<Fresh>;
    friend class InlineFrame<Synced>;
    friend FrameBytes;

    InlineFrameState state_;
};

// ─────────────────────────────────────────────────────────────────────────
// InlineFrame<Sealed>
// ─────────────────────────────────────────────────────────────────────────
//
// Terminal restoration emitted. Cannot be rendered against. The
// destructor is the only valid operation; there is no transition out
// of Sealed.

template <>
class InlineFrame<Sealed> {
public:
    InlineFrame(InlineFrame&&) noexcept            = default;
    InlineFrame& operator=(InlineFrame&&) noexcept = default;
    InlineFrame(const InlineFrame&)            = delete;
    InlineFrame& operator=(const InlineFrame&) = delete;

private:
    InlineFrame() noexcept = default;
    friend class InlineFrame<Empty>;
    friend class InlineFrame<Fresh>;
    friend class InlineFrame<Synced>;
    friend class InlineFrame<Stale>;
};

} // namespace maya::inline_frame
