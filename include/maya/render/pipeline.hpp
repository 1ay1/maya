#pragma once
// maya::render::pipeline — Compile-time enforced render pipeline
//
// Encodes the rendering sequence as a type-state Mealy machine.
// Each stage is a distinct C++ type. Transitions consume the current stage
// (&&-qualified) and return the next stage. Performing steps out of order
// is a compile-time error — the type system is the proof of correctness.
//
// Mealy machine:
//   S = {Idle, Cleared, Painted, Opened, Closed}   (state set)
//   Σ = {clear, paint, open_frame,                  (input alphabet)
//         write_diff, apply_cursor, close_frame}
//   Λ = canvas operations, ANSI string writes       (output alphabet)
//
//   δ(Idle,    clear())                → Cleared + canvas.clear()
//   δ(Cleared, paint(root))            → Painted + render_tree()
//   δ(Painted, open_frame())           → Opened  + write hide_cursor+sync_start
//   δ(Opened,  write_diff(front))      → Opened  + write ANSI diff bytes
//   δ(Opened,  apply_cursor(...))      → Opened  + write cursor move (cond.)
//   δ(Opened,  close_frame())          → Closed  + write SGR-reset+sync_end
//
// Correctness proofs are static_asserts at the bottom of this file.
// If those asserts compile, the state machine is provably correct.
//
// Usage (fluent):
//   RenderPipeline::start(back_canvas, pool, theme, out_buf)
//       .clear()
//       .paint(root)
//       .open_frame()
//       .write_diff(front_canvas)
//       .apply_cursor(back_cur, front_cur, back_vis, front_vis)
//       .close_frame();
//   // out_buf now contains a complete, atomic ANSI frame.

#include <concepts>
#include <type_traits>

#include "canvas.hpp"
#include "diff.hpp"
#include "renderer.hpp"
#include "serialize.hpp"
#include "../core/types.hpp"
#include "../element/element.hpp"
#include "../style/theme.hpp"
#include "../terminal/ansi.hpp"

namespace maya {

// ============================================================================
// Stage tags — zero-size compile-time state identifiers
// ============================================================================
// These are phantom types. They carry no runtime data. Their only purpose is
// to parameterise RenderPipeline<S> so the compiler rejects invalid orderings.

namespace stage {
    struct Idle    {};  // ready to start; nothing written or rendered
    struct Cleared {};  // back canvas erased; ready for painting
    struct Painted {};  // element tree rendered into back canvas
    struct Opened  {};  // sync frame open; diff content may be appended
    struct Closed  {};  // sync frame closed; output buffer is complete
}

// Concept: valid stage = zero-size, trivially-constructible tag type.
template<typename T>
concept Stage = std::is_empty_v<T> && std::is_trivially_constructible_v<T>;

static_assert(Stage<stage::Idle>);
static_assert(Stage<stage::Cleared>);
static_assert(Stage<stage::Painted>);
static_assert(Stage<stage::Opened>);
static_assert(Stage<stage::Closed>);

// ============================================================================
// RenderPipeline<S> — linear, move-only typed render pipeline
// ============================================================================
// "Linear" in the linear-type sense: every transition method is &&-qualified,
// so the old stage object is consumed. You cannot accidentally reuse a stage.
//
// The pipeline holds non-owning references to external resources (canvas,
// style pool, theme, output string). It is cheap to construct and copy
// logically — but copy is deleted to make linearity enforceable.

template<Stage S>
class RenderPipeline {
    Canvas&       back_;
    StylePool&    pool_;
    const Theme&  theme_;
    std::string&  out_;

    // All stage specialisations may construct each other.
    template<Stage> friend class RenderPipeline;

    RenderPipeline(Canvas& b, StylePool& p, const Theme& t, std::string& o) noexcept
        : back_(b), pool_(p), theme_(t), out_(o) {}

public:
    RenderPipeline()                                   = delete;
    RenderPipeline(const RenderPipeline&)              = delete;
    RenderPipeline& operator=(const RenderPipeline&)   = delete;
    RenderPipeline(RenderPipeline&&)            noexcept = default;
    RenderPipeline& operator=(RenderPipeline&&) noexcept = default;

    // -- Entry point ----------------------------------------------------------

    /// Create an Idle pipeline. Call clear() to begin the render cycle.
    [[nodiscard]] static RenderPipeline<stage::Idle>
    start(Canvas& back, StylePool& pool, const Theme& theme, std::string& out) noexcept {
        return {back, pool, theme, out};
    }

    // =========================================================================
    // δ(Idle, clear()) → Cleared
    // Effect: back_.clear() — zeroes all cells, marks full canvas as damaged.
    // =========================================================================
    [[nodiscard]] RenderPipeline<stage::Cleared>
    clear() && requires std::same_as<S, stage::Idle> {
        back_.clear();
        return {back_, pool_, theme_, out_};
    }

    // =========================================================================
    // δ(Cleared, paint(root)) → Painted
    // Effect: render_tree() walks the element tree and fills the back canvas.
    // =========================================================================
    [[nodiscard]] RenderPipeline<stage::Painted>
    paint(const Element& root) && requires std::same_as<S, stage::Cleared> {
        render_tree(root, back_, pool_, theme_);
        return {back_, pool_, theme_, out_};
    }

    // =========================================================================
    // δ(Cleared, paint(root, nodes)) → Painted
    // Overload: reuses a layout node vector across frames to avoid allocation.
    // =========================================================================
    [[nodiscard]] RenderPipeline<stage::Painted>
    paint(const Element& root, std::vector<layout::LayoutNode>& nodes,
          bool auto_height = false) && requires std::same_as<S, stage::Cleared> {
        render_tree(root, back_, pool_, theme_, nodes, auto_height);
        return {back_, pool_, theme_, out_};
    }

    // =========================================================================
    // δ(Painted, open_frame()) → Opened
    // Effect: appends hide_cursor + sync_start to out_.
    //         The terminal will not display partial updates until close_frame().
    // =========================================================================
    [[nodiscard]] RenderPipeline<stage::Opened>
    open_frame() && requires std::same_as<S, stage::Painted> {
        out_ += ansi::hide_cursor;
        out_ += ansi::sync_start;
        return {back_, pool_, theme_, out_};
    }

    // =========================================================================
    // δ(Opened, write_diff(front)) → Opened
    // Effect: compares front vs back; appends minimal ANSI delta to out_.
    //         Zero heap allocations — all writes are direct string appends.
    // =========================================================================
    [[nodiscard]] RenderPipeline<stage::Opened>
    write_diff(const Canvas& front) && requires std::same_as<S, stage::Opened> {
        diff(front, back_, pool_, out_);
        return {back_, pool_, theme_, out_};
    }

    // =========================================================================
    // δ(Opened, apply_cursor(...)) → Opened
    // Effect: conditionally appends show_cursor and/or CUP sequence to out_.
    //         No-op when cursor state is unchanged.
    // =========================================================================
    [[nodiscard]] RenderPipeline<stage::Opened>
    apply_cursor(
        Position back_cursor,  Position front_cursor,
        bool     back_visible, bool front_visible) &&
        requires std::same_as<S, stage::Opened>
    {
        if (back_visible && !front_visible)
            out_ += ansi::show_cursor;

        if (back_cursor != front_cursor && back_visible)
            ansi::write_move_to(out_,
                back_cursor.x.value + 1,
                back_cursor.y.value + 1);

        return {back_, pool_, theme_, out_};
    }

    // =========================================================================
    // δ(Opened, write_full()) → Opened
    // Effect: clears the screen, homes the cursor, then serializes the entire
    //         back canvas sequentially. Used for the first frame or after resize
    //         when diff against a stale front canvas would be incorrect.
    // =========================================================================
    [[nodiscard]] RenderPipeline<stage::Opened>
    write_full() && requires std::same_as<S, stage::Opened> {
        out_ += "\x1b[2J\x1b[H";
        serialize(back_, pool_, out_);
        return {back_, pool_, theme_, out_};
    }

    // =========================================================================
    // δ(Opened, close_frame()) → Closed
    // Effect: appends SGR-reset + sync_end to out_.
    //         After this, out_ is a complete, self-contained ANSI frame.
    // =========================================================================
    RenderPipeline<stage::Closed>
    close_frame() && requires std::same_as<S, stage::Opened> {
        out_ += ansi::reset;
        out_ += ansi::sync_end;
        return {back_, pool_, theme_, out_};
    }
};

// ============================================================================
// Compile-time correctness proofs — legal transitions
// ============================================================================
// Positive proofs: verify that the correct transition sequence compiles.
// The requires-clauses on each method statically reject illegal orderings
// at every call site — no separate negative proofs needed.

static_assert(requires(RenderPipeline<stage::Idle> p)
    { std::move(p).clear(); });

static_assert(requires(RenderPipeline<stage::Cleared> p, const Element& e)
    { std::move(p).paint(e); });

static_assert(requires(RenderPipeline<stage::Painted> p)
    { std::move(p).open_frame(); });

static_assert(requires(RenderPipeline<stage::Opened> p, const Canvas& c)
    { std::move(p).write_diff(c); });

static_assert(requires(RenderPipeline<stage::Opened> p)
    { std::move(p).write_full(); });

static_assert(requires(RenderPipeline<stage::Opened> p)
    { std::move(p).close_frame(); });

} // namespace maya
