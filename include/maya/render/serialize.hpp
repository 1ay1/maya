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
#include "../platform/io.hpp"

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

class [[nodiscard("ContentRows is the typed witness that the row count came from the canvas about to be painted — dropping it forces another canvas walk to recompute")]] ContentRows {
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

// ============================================================================
// TermRows — derivation witness for the terminal-viewport height
// ============================================================================
//
// Part of the Witness Chain. Compose's case-(B) emit reads `term_h` to
// decide how many rows above the new frame to erase, and the per-row
// loop reads it to decide which rows will scroll off into native
// scrollback this frame. A stale `term_h` (caller cached it before a
// resize) causes the case-(B) erase to walk the wrong distance — the
// classic "ghost rows above the new frame" symptom.
//
// `TermRows` lifts the read-now-or-cache-and-pray decision into the
// type system. The sole producer is `query_term_rows(handle)` which
// goes straight to `platform::query_terminal_size` at the call site;
// you cannot construct a TermRows from a stored int, you cannot
// default-construct one, you cannot pass an old TermRows value to a
// later compose (the type is `[[nodiscard]]` and move-only-by-default
// for value semantics, but more importantly the *intent* of the type
// is that it gets re-queried per compose).
//
// The runtime is welcome to keep an `int` cache of last-known
// terminal height for layout decisions; compose just refuses to take
// it as input. The cost of querying per compose is one ioctl(2)
// (TIOCGWINSZ) — ~200ns on Linux — paid once per render tick, which
// is rounding error against the cost of even a tiny compose.
class [[nodiscard("TermRows is the typed witness for terminal viewport height — dropping it forces another platform query")]] TermRows {
public:
    [[nodiscard]] constexpr int value() const noexcept { return rows_; }

private:
    constexpr explicit TermRows(int r) noexcept : rows_(r) {}
    friend TermRows query_term_rows(platform::NativeHandle) noexcept;
    friend constexpr TermRows term_rows_for_test(int) noexcept;

    int rows_;
};

/// The sole production producer of `TermRows`. Calls
/// `platform::query_terminal_size(handle)` at the call site, returns
/// the height bound into the typed witness. Pass the result directly
/// to `compose_inline_frame` / `InlineFrame<Tag>::render` — carrying
/// it across more than one render tick defeats the purpose.
///
/// Returns a TermRows of value 24 if the platform query fails, so
/// the type can still flow through call paths that don't have a
/// real tty (e.g. when stdout is redirected to a file).
[[nodiscard]] TermRows query_term_rows(platform::NativeHandle handle) noexcept;

/// Test-only constructor for TermRows. Production code uses
/// `query_term_rows(handle)`; tests that don't have a real terminal
/// can synthesize a TermRows of any value via this constexpr factory.
/// Marked `_for_test` so the production-vs-test distinction is
/// visible at every call site.
[[nodiscard]] constexpr TermRows term_rows_for_test(int rows) noexcept {
    return TermRows{rows};
}

/// Serialize `rows` rows of the canvas starting at `start_row`
/// (or all rows if rows <= 0).
void serialize(const Canvas& canvas, const StylePool& pool,
               std::string& out, int rows = 0, int start_row = 0);

// ============================================================================
// Inline frame composition
// ============================================================================

class InlineFrameState;  // forward — defined below
struct FinalizeResult;   // forward — returned by InlineFrameState::finalize() &&

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
class [[nodiscard("ScrollbackMarker is a single-use commit token — dropping it loses the scrollback advance for this tick")]] ScrollbackMarker {
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
    friend class InlineFrameState;
};

// Forward declarations of the only types allowed to touch the state's
// private fields. These are the renderer internals — the byte-emitter,
// the witness-chain capsule, and the per-tag wrapper classes.
namespace inline_frame {
    struct Empty;
    struct Fresh;
    struct Synced;
    struct Stale;
    struct HardReset;
    struct Sealed;
    template <class Tag> class InlineFrame;
}
class  FrameBytes;
class  ShadowWitness;
struct StylePool;
class  Canvas;
[[nodiscard]] std::optional<ShadowWitness> verify_shadow(const InlineFrameState&) noexcept;

/// Persistent state for the inline (row-diff) renderer.
///
/// Holds a copy of the last-rendered cell buffer so successive frames
/// can be compared exactly via `simd::bulk_eq` instead of hashed (no
/// collisions). Carried inside `InlineFrame<Tag>` (the Witness Chain)
/// — never held directly by application code.
///
/// ─────────────────────────────────────────────────────────────────
/// Encapsulation (part of the Witness Chain's compile-time proof)
/// ─────────────────────────────────────────────────────────────────
///
/// All mutable fields are PRIVATE. The only code that can advance
/// them is the friend set below: the byte-emitter
/// `compose_inline_frame_impl`, the witness producer `verify_shadow`,
/// the FrameBytes capsule (commit / abandon successor handling), and
/// the six `InlineFrame<Tag>` classes (state-machine transitions).
///
/// Why: the on-wire scrollback invariant
///
///     prev_cells[0 .. prev_rows × prev_width) ≡ visible viewport
///
/// depends on prev_cells, prev_rows, prev_width, and shadow_hash all
/// advancing in lockstep with the bytes that actually shipped. An
/// outside writer that nudges any one of them desynchronises the
/// shadow from the wire — the next diff is computed against a fiction
/// and the terminal corrupts. Making the fields private moves "don't
/// touch these" from a comment into a compile error.
///
/// The state is non-copyable for the same reason: two parallel copies
/// of the same prev_cells would let two compose passes both think
/// they own the wire, which is exactly the bug class ShadowWitness
/// was invented to detect at runtime. With non-copyable + private
/// fields, the runtime detector becomes a belt over suspenders: the
/// compiler refuses to even produce the suspect program.
/// ─────────────────────────────────────────────────────────────────
/// Immutability (type-theoretic discipline)
/// ─────────────────────────────────────────────────────────────────
///
/// Logically, `InlineFrameState` is a value in an algebraic data type:
///
///     State = { prev_cells; prev_rows; prev_width; flags; shadow; … }
///
/// and every operation that "advances" the state is a pure function
/// `State → (Output, State)`. The implementation realises this with
/// move-only semantics: the only public mutators are rvalue-qualified
/// (`&&`), so they consume the current value and return a *new*
/// value. There is no `set_X(v)` method, no public assignment of any
/// field, no `void reset()` that re-uses the old name; the only way
/// to obtain a state with different invariants is to call a function
/// that returns one.
///
/// Internally (inside friends), the moved-in value is a function-
/// local owned object — the function can write into that local
/// freely before returning the result. The write is unobservable to
/// any outside code because the outside code has already given up
/// the state by value-move. From the type system's perspective the
/// operation is indistinguishable from a pure transformation.
///
/// Consequences:
///
///   1. Two parallel views of the same `prev_cells` cannot exist.
///      Move-only + private fields ensure it; the only way to obtain
///      a state is from a returning factory, and each returning
///      factory destroys its predecessor.
///
///   2. Reordering a sequence of state-advancing operations changes
///      the type of every intermediate, surfacing the wrong order as
///      a compile error. E.g. you cannot call `finalize` and then
///      try to `committed` the same state — finalize returned its
///      result by value, and the original state is gone.
///
///   3. Aliasing bugs ("someone snuck a pointer to state.shadow_hash
///      and bumped it") are impossible: there is no addressable
///      field, only accessors that return by value.
class [[nodiscard("InlineFrameState is the renderer's only handle to its shadow-of-wire — dropping the value loses the model of what the terminal is displaying")]] InlineFrameState {
public:
    /// The unique public constructor. Produces the initial empty
    /// value (prev_rows = 0, shadow_hash = sentinel, no terminal
    /// modes claimed). All further states descend from this one via
    /// the `&&`-qualified advancing operations below.
    InlineFrameState() noexcept = default;

    // Move-only. Copies would create parallel shadows of the same
    // wire region; see the type-level rationale above.
    InlineFrameState(const InlineFrameState&)            = delete;
    InlineFrameState& operator=(const InlineFrameState&) = delete;
    InlineFrameState(InlineFrameState&&) noexcept            = default;
    InlineFrameState& operator=(InlineFrameState&&) noexcept = default;

    // ── Pure read accessors ─────────────────────────────────────────
    // Return by value. The state never exposes a reference or
    // pointer to any of its fields — outsiders cannot observe
    // identity, only contents.
    [[nodiscard]] int      prev_width()       const noexcept { return prev_width_; }
    [[nodiscard]] int      prev_rows()        const noexcept { return prev_rows_; }
    [[nodiscard]] int      wire_cursor_rows() const noexcept { return wire_cursor_rows_; }
    [[nodiscard]] uint64_t shadow_hash()      const noexcept { return shadow_hash_; }

    // ── Pure advancing operations ───────────────────────────────────
    // Each consumes `*this` by rvalue-move and returns the successor
    // state by value. Calling them on an lvalue is a compile error —
    // the caller has to `std::move(state).X()` and the source is
    // gone after the call.

    /// Return a fresh state with every invariant cleared. The
    /// underlying `prev_cells` buffer allocation is retained
    /// (AlignedBuffer move is a pointer swap) so the next compose
    /// can re-use the capacity, but every observable field is set
    /// to its initial value. Equivalent in observable behaviour to
    /// `InlineFrameState{}` with retained capacity.
    [[nodiscard]] InlineFrameState reset_state() && noexcept {
        InlineFrameState s{std::move(*this)};
        s.prev_width_        = 0;
        s.prev_rows_         = 0;
        s.decawm_off_        = false;
        s.cursor_hidden_     = false;
        s.shadow_hash_       = static_cast<uint64_t>(-1);
        s.ghost_rows_above_  = 0;
        s.wire_cursor_rows_  = 0;
        return s;
    }

    /// Emit the bytes that restore terminal-mode flags this state
    /// claims (cursor visibility, DECAWM), and return the successor
    /// in which those flags are no longer claimed. The returned
    /// state's `finalize()` is observably a no-op.
    [[nodiscard]] FinalizeResult finalize() && noexcept;

    /// Build a typed marker for committing `rows` rows of scrollback.
    /// Clamped to [0, prev_rows]; passing prev_rows means "commit
    /// everything that's currently considered prev frame." Returns
    /// an empty marker (no-op when consumed) when prev_rows == 0.
    /// This is a const read — the marker is consumed by
    /// `committed()`, not by issuing it.
    [[nodiscard]] ScrollbackMarker scrollback_marker(int rows) const noexcept {
        if (rows <= 0 || prev_rows_ <= 0) return ScrollbackMarker{};
        return ScrollbackMarker{std::min(rows, prev_rows_)};
    }

    /// Consume the marker and return the successor state with
    /// `marker.rows()` rows shifted off the top of prev_cells. A
    /// marker that targets all rows (or more) returns a reset
    /// state. The shadow_hash is recomputed over the post-shift
    /// cells so the next compose's verify_shadow sees a consistent
    /// reference.
    [[nodiscard]] InlineFrameState committed(ScrollbackMarker marker) && noexcept;

    /// Recovery factory: return the state shape required after a
    /// soft failure (FrameBytes::abandon, Synced::demote_to_stale).
    /// `prev_rows` is zeroed, `shadow_hash` is sentinel'd, and
    /// `ghost_rows_above` is set to the prior wire_cursor_rows so
    /// the next compose's case-(B) emit walks the correct upward
    /// erase distance.
    [[nodiscard]] InlineFrameState abandoned_for_recovery() && noexcept {
        InlineFrameState s{std::move(*this)};
        const int prior_wire_rows = s.wire_cursor_rows_;
        s.ghost_rows_above_ = prior_wire_rows;
        s.prev_rows_        = 0;
        s.cursor_hidden_    = false;
        s.decawm_off_       = false;
        s.shadow_hash_      = static_cast<uint64_t>(-1);
        return s;
    }

private:
    // ── Friend set: the only code allowed to touch private fields ───
    //
    // The byte-emitter takes the state by &&, writes into a function-
    // local owned copy, and returns the result; friendship lets it
    // touch the storage of the local it just took ownership of.
    friend std::pair<std::string, InlineFrameState>
    compose_inline_frame_impl(const Canvas&, int, int,
                              const StylePool&,
                              InlineFrameState&&, bool);
    friend std::optional<ShadowWitness> verify_shadow(const InlineFrameState&) noexcept;
    friend class FrameBytes;
    template <class Tag> friend class inline_frame::InlineFrame;
    // The witness-chain free function lives in frame_bytes.cpp; it
    // re-verifies the witness across the move boundary by reading
    // the moved-in state's hashable cells.
    friend FrameBytes compose_inline_frame(
        const Canvas&, ContentRows, TermRows, const StylePool&,
        InlineFrameState&&, ShadowWitness&&, bool);

    // ── Storage (private) ───────────────────────────────────────────
    // Logically immutable — written only by friends that took the
    // state by &&-move and own the storage they're writing to. The
    // C++ `const`-keyword would forbid move-assignment (needed for
    // the chain's `state_ = std::move(other)`); immutability is
    // therefore enforced by the public API, not by the storage
    // qualifier.
    AlignedBuffer prev_cells_;
    int           prev_width_       = 0;
    int           prev_rows_        = 0;
    bool          decawm_off_       = false;
    bool          cursor_hidden_    = false;
    int           ghost_rows_above_ = 0;
    int           wire_cursor_rows_ = 0;
    uint64_t      shadow_hash_      = static_cast<uint64_t>(-1);
};

/// Return value of `InlineFrameState::finalize() &&`. Carries the
/// bytes the terminal needs to restore modes the state claimed, plus
/// the successor state that no longer claims them. Defined at
/// namespace scope because the nested-class variant would create a
/// member of incomplete type (the enclosing class is incomplete
/// inside its own definition).
struct FinalizeResult {
    std::string      restore_bytes;
    InlineFrameState sealed;
};

/// Compose the byte stream for one inline frame.
///
/// Public API: see `inline_frame::InlineFrame<Tag>::render(...)` and
/// the witness-chain entrypoint declared in `frame_bytes.hpp`. There
/// is no out-parameter / mutating-state form — the Witness Chain is
/// the only path. Callers without a Witness Chain state should drive
/// rendering through `maya::print` / `maya::live` / `maya::Runtime`,
/// which manage the chain internally.

// ============================================================================
// ShadowWitness — proof that a state's shadow currently matches the wire
// ============================================================================
//
// Part of the Witness Chain (see docs/internals/witness-chain.md).
//
// Theorem T2 (no-corruption) hinges on `compose_inline_frame` diffing
// against a shadow that genuinely reflects what the terminal is
// displaying. ShadowWitness lifts that precondition into the type
// system: it is a move-only token whose constructor is private, and
// the only producer is `verify_shadow(state)` below — which returns
// `std::optional<ShadowWitness>` (nullopt when the shadow is poisoned,
// populated when it provably matches the wire at the moment of the
// call). The compose entrypoint requires `ShadowWitness&&` as an
// argument, so "compose without verifying the shadow" is a compile
// error.
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

class [[nodiscard("ShadowWitness is single-use proof that the shadow matches the wire — dropping it forces the next render to re-verify or fall through to recovery")]] ShadowWitness {
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
    /// Consumers (compose_inline_frame) assert this before use.
    [[nodiscard]] bool valid() const noexcept { return state_ != nullptr; }

    /// Pointer-identity of the state the witness was issued against.
    /// Used by compose to detect cross-state misuse.
    [[nodiscard]] const InlineFrameState* bound_to() const noexcept {
        return state_;
    }

    /// Default constructor exists only for the move-from invariant
    /// (a moved-from witness must be safely destructible). It is
    /// deliberately deleted so external callers cannot synthesize a
    /// witness — the only producer is `verify_shadow(state)` below.
    ShadowWitness() = delete;

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
///     `compose_inline_frame` on this state; the only safe response
///     is to demote to Divergent and re-paint from a clear viewport.
///
/// Cost: O(prev_rows × prev_width) of u64 FNV-1a folding, fully
/// vectorisable by the compiler. ~1µs on a 200×80 viewport. This is
/// the same hash compose stores at the end of the prior frame;
/// `verify_shadow` is the symmetric reader.
[[nodiscard]] std::optional<ShadowWitness> verify_shadow(const InlineFrameState& state) noexcept;

} // namespace maya
