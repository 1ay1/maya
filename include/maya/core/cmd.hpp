#pragma once
// maya::Cmd<Msg> - Side effects as data
//
// The core type-theoretic insight: a pure function cannot perform I/O.
// But it CAN return a *description* of I/O to be performed later.
// Cmd<Msg> is that description.
//
// An update function receives the current state and an event, and returns
// the new state alongside a Cmd describing what should happen next:
//
//   auto update(Model m, Msg msg) -> std::pair<Model, Cmd<Msg>> {
//       return std::visit(overload{
//           [&](Save) { return {m, Cmd<Msg>::write_clipboard(m.text)}; },
//           [&](Quit) { return {m, Cmd<Msg>::quit()}; },
//       }, msg);
//   }
//
// This makes update() a pure function — same inputs, same outputs,
// testable with operator==. The runtime interprets the Cmd and performs
// the actual terminal I/O.
//
// Cmd is an algebraic data type (std::variant) following the same pattern
// as Element, Event, and RenderOp elsewhere in maya. Each alternative
// describes one kind of effect. Smart constructors provide a clean API.
// The functor map() operation allows child components to embed their
// local Msg type into a parent's.

#include <chrono>
#include <functional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "overload.hpp"
#include "../render/scrollback_ledger.hpp"

namespace maya {

// ============================================================================
// Cmd<Msg> - Algebraic description of a side effect
// ============================================================================
// A sum type: exactly one of the alternatives below. The runtime
// pattern-matches on the variant and performs the effect. User code
// never performs effects directly — it returns Cmd values.
//
// This is maya's equivalent of Haskell's IO monad or Rust's Command:
// a value that *describes* an effect without *performing* it.

template <typename Msg>
class Cmd {
public:

    // -- Alternatives (each describes one kind of effect) ---------------------

    struct None {};
    struct Quit {};

    struct Batch {
        std::vector<Cmd> cmds;
    };

    struct After {
        std::chrono::milliseconds delay;
        Msg                       msg;
    };

    struct SetTitle {
        std::string title;
    };

    struct WriteClipboard {
        std::string content;
    };

    // Ask the terminal to report its system clipboard back over the
    // input stream (OSC 52 read). The reply arrives as a PasteEvent
    // carrying the decoded clipboard bytes — see ansi::request_clipboard
    // and InputParser::parse_osc. The portable, remote-tool-free way to
    // read the clipboard across an SSH pty.
    struct QueryClipboard {};

    /// Escape hatch: arbitrary async work. The function receives a dispatch
    /// callback and calls it with a Msg when the result is ready. Runs on
    /// the runtime's shared, elastic-but-bounded BG worker pool — cheap,
    /// good for short tasks (HTTP fetch, model list, browser launch).
    struct Task {
        std::function<void(std::function<void(Msg)>)> run;
    };

    /// Like Task, but runs on its own dedicated, detached std::thread
    /// instead of the shared BG pool. Use this for work that can wedge a
    /// thread indefinitely on a blocking syscall (slow / dead filesystem
    /// mounts, network freezes, sandboxed subprocesses). A wedged
    /// IsolatedTask leaks one thread; a wedged Task on the shared pool
    /// permanently consumes one of the pool's slots and starves
    /// subsequent tasks. Trade: ~100-300 µs of std::thread construction
    /// per call, paid only for genuinely hang-prone work.
    struct IsolatedTask {
        std::function<void(std::function<void(Msg)>)> run;
    };

    /// Mark the top `rows` rows of the last rendered inline frame as
    /// committed to terminal scrollback.  Apply when the view function is
    /// about to return a shorter element tree (e.g. a chat UI virtualizing
    /// old messages), so the row-diff renderer doesn't interpret the
    /// shrinkage as "content removed from the bottom" and erase visible
    /// rows.  No effect in fullscreen mode.
    struct CommitScrollback {
        int rows;
    };

    /// Commit all rows of the last rendered inline frame that have
    /// PROVABLY already overflowed the terminal viewport —
    /// `max(0, prev_rows - term_h)`. This is a strict lower bound on
    /// the rows already living in native scrollback as immutable
    /// cells, computed inside the renderer from state it owns. The
    /// host doesn't need to count anything; the renderer's prev_rows
    /// + term_h answer the question precisely.
    ///
    /// Use this when the host wants to keep prev_cells from growing
    /// unboundedly across a long session without risking the
    /// over-commit ghosting failure mode (committing rows that are
    /// still on-screen in the live viewport). Typical pattern: after
    /// virtualizing old messages out of the view tree, send this Cmd
    /// so prev_cells shrinks by everything that's definitely
    /// scrollback, and let compose_inline_frame's natural shrink path
    /// reconcile the surviving live-viewport rows.
    ///
    /// Safe because rows beyond term_h were pushed into native
    /// scrollback by compose_inline_frame's own \r\n emits in prior
    /// frames — by the time prev_rows > term_h holds, at least
    /// `prev_rows - term_h` rows have already overflowed and are
    /// immutable to the application. Equivalent to writing
    /// `CommitScrollback{rt.inline_content_rows() - term_h}` if the
    /// host had access to both numbers, but maya knows them itself
    /// and the contract is clearer at the Cmd level.
    ///
    /// No effect in fullscreen mode or when `prev_rows <= term_h`
    /// (everything still fits on screen — nothing has overflowed).
    struct CommitScrollbackOverflow {};

    /// Soft repaint of the live viewport. Demotes the inline
    /// coherence state Synced → Stale, routing the next render
    /// through compose's case (B): walk cursor up by the prior
    /// frame's painted-region height, re-emit every visible row
    /// in place, tail-erase with `\x1b[J`.
    ///
    /// **This is NOT a hard reset.** It does not emit
    /// `\x1b[2J\x1b[3J\x1b[H`. Pre-agentty shell scrollback above
    /// the viewport is preserved. Only `Runtime::handle_resize`
    /// (SIGWINCH) emits the hard wipe — there is no host-callable
    /// hard-reset Cmd.
    ///
    /// Use cases (narrow):
    ///   - Ghost cells **inside** the live viewport: composer
    ///     outline survivors of a stream-finish shrink, stale
    ///     status / footer rows below the new content_rows, SGR
    ///     residue from a half-written frame.
    ///   - User-facing "redraw screen" hotkey (Ctrl-L) when the
    ///     in-viewport rendering looks wrong.
    ///
    /// **Hazard — do NOT use on wholesale model swap.** Case (B)'s
    /// scroll-to-fit branch emits `\n` at the viewport bottom
    /// when the new frame is taller than the old cursor's offset
    /// from viewport top — each `\n` permanently scrolls one row
    /// of host content into terminal-owned scrollback. On thread
    /// switch / new-thread / checkpoint restore, prefer
    /// `commit_scrollback_overflow()` instead: it drops the stale
    /// overflow from `prev_cells` (zero wire bytes) so the normal
    /// diff path can re-emit the full viewport correctly against
    /// the swapped-in model.
    ///
    /// Choosing between Cmd::force_redraw and
    /// Cmd::commit_scrollback_overflow:
    ///
    ///   | Situation                                | Right Cmd          |
    ///   |------------------------------------------|--------------------|
    ///   | Ghost cells inside the viewport          | force_redraw       |
    ///   | Ctrl-L style user redraw                 | force_redraw       |
    ///   | Wholesale model swap (thread / restore)  | commit_..._overflow + force_redraw |
    ///   | Bounded-frozen trim (drop oldest N rows) | commit_..._overflow|
    ///   | Terminal genuinely corrupted             | (resize only)      |
    struct ForceRedraw {};

    /// HARD inline reset: the next render wipes the viewport AND the
    /// terminal's saved-lines (`\x1b[2J\x1b[3J\x1b[H`) and repaints
    /// the new content from a clean home position.
    ///
    /// The ONLY host-callable Cmd that can erase rows the application
    /// already scrolled into native scrollback. Use it for a
    /// WHOLESALE, DESTRUCTIVE content swap into potentially shorter
    /// content — thread switch, new thread, checkpoint restore —
    /// where the old transcript may have overflowed the viewport and
    /// left dozens of its rows committed above the live frame.
    ///
    /// Why neither alternative works on that transition:
    ///   - `commit_scrollback_overflow` only drops the overflow from
    ///     `prev_cells` (so the diff scans the full viewport); it does
    ///     NOT emit any bytes, so the old thread's physical rows above
    ///     the viewport stay on the wire — a stranded duplicate when
    ///     the new content is shorter.
    ///   - `force_redraw` (case-B) is viewport-only and its
    ///     scroll-to-fit branch scrolls host history away.
    ///
    /// Cost: `\x1b[3J` is destructive to pre-agentty shell scrollback.
    /// Acceptable for an explicit user-initiated swap; NEVER wire to a
    /// routine per-frame / per-turn path. For resize, the runtime's
    /// SIGWINCH handler already does its own hard reset.
    struct ResetInline {};

    /// Legacy toggle for the inline composer anti-bounce. NO-OP: the
    /// anti-bounce is now fully autonomous inside Runtime::render (maya
    /// detects a transient content-height dip and bridges it itself).
    /// Kept so existing host call sites and the Cmd variant stay valid;
    /// `Runtime::set_height_hold` ignores the bit. See that method.
    struct SetHeightHold { bool on = false; };

    /// Suspend the TUI and hand the REAL terminal to `run` — the
    /// interactive-child escape hatch (sudo password prompts, editors,
    /// pagers). The runtime tears the TUI down to a cooked, clean tty,
    /// calls `run()` SYNCHRONOUSLY on the UI thread (the user is
    /// interacting with the child; there is nothing else to do), then
    /// restores raw mode + the TUI escapes, re-anchors the renderer
    /// (inline: fresh case-A serialize below the child's output;
    /// fullscreen: Divergent full repaint), and dispatches the Msg
    /// `run` returned so the reducer can fold the child's result back
    /// into the model.
    ///
    /// `run` returning a Msg (rather than void) is what closes the
    /// loop: the callable typically spawns the child with inherited
    /// stdio, tees its output to a buffer, and returns a completion
    /// Msg carrying exit code + captured bytes.
    struct Suspend {
        std::function<Msg()> run;
    };

    using Variant = std::variant<None, Quit, Batch, After, SetTitle,
                                 WriteClipboard, QueryClipboard, Task, IsolatedTask,
                                 CommitScrollback, CommitScrollbackOverflow,
                                 ForceRedraw, ResetInline, SetHeightHold,
                                 Suspend>;
    Variant inner;

    // -- Smart constructors ---------------------------------------------------

    [[nodiscard]] static constexpr auto none() -> Cmd { return {None{}}; }
    [[nodiscard]] static constexpr auto quit() -> Cmd { return {Quit{}}; }

    [[nodiscard]] static auto after(std::chrono::milliseconds d, Msg m) -> Cmd {
        return {After{d, std::move(m)}};
    }

    [[nodiscard]] static auto set_title(std::string t) -> Cmd {
        return {SetTitle{std::move(t)}};
    }

    [[nodiscard]] static auto write_clipboard(std::string s) -> Cmd {
        return {WriteClipboard{std::move(s)}};
    }

    /// Ask the terminal to send its clipboard back as a PasteEvent
    /// (OSC 52 read). Works across SSH with no remote clipboard tool.
    [[nodiscard]] static auto query_clipboard() -> Cmd {
        return {QueryClipboard{}};
    }

    template <std::invocable<std::function<void(Msg)>> F>
    [[nodiscard]] static auto task(F&& f) -> Cmd {
        return {Task{std::forward<F>(f)}};
    }

    /// Run on a dedicated detached thread. See IsolatedTask for the
    /// rationale — use this for hang-prone work (filesystem syscalls,
    /// blocking subprocess waits, network calls without timeouts) so
    /// a wedged worker can't starve the shared BG pool.
    template <std::invocable<std::function<void(Msg)>> F>
    [[nodiscard]] static auto task_isolated(F&& f) -> Cmd {
        return {IsolatedTask{std::forward<F>(f)}};
    }

    /// LEGACY raw-int commit. Deprecated: the count is host-guessed,
    /// and every historical trim-corruption bug was drift between a
    /// host guess and what maya painted (see docs/internals/
    /// witness-chain.md, Trim Accounting). Hold your sealed prefix in
    /// a maya::ScrollbackLedger, render it via ledger_ref /
    /// Conversation::Config::ledger, and pass ledger.harvest() to the
    /// typed overload below — the commit count then comes from maya's
    /// own paint pass and structurally cannot drift.
    [[deprecated("host-guessed row counts drift from the wire; use "
                 "ScrollbackLedger::harvest() + "
                 "commit_scrollback(ScrollbackDebt)")]]
    [[nodiscard]] static auto commit_scrollback(int rows) -> Cmd {
        return {CommitScrollback{rows}};
    }

    /// Typed-token commit (Witness Chain — Trim Accounting). The ONLY
    /// way to obtain a ScrollbackDebt is ScrollbackLedger::harvest(),
    /// whose rows are paint-recorded by maya's own layout pass — so a
    /// host that routes its trims through the ledger structurally
    /// CANNOT commit a count that drifts from the wire. Prefer this
    /// over the raw-int overload everywhere a ledger exists; the int
    /// overload remains for non-ledger hosts and is clamped to the
    /// provable overflow inside commit_inline_prefix anyway.
    [[nodiscard]] static auto commit_scrollback(ScrollbackDebt debt) -> Cmd {
        if (debt.empty()) return none();
        return {CommitScrollback{debt.rows()}};
    }

    /// Commit every row of the last inline frame that has provably
    /// already overflowed the viewport — see
    /// `CommitScrollbackOverflow` doc for the safety argument.
    [[nodiscard]] static auto commit_scrollback_overflow() -> Cmd {
        return {CommitScrollbackOverflow{}};
    }

    /// Schedule a soft viewport repaint on the next render. See
    /// `ForceRedraw` doc above for the exact mechanics and the
    /// chooser table vs `commit_scrollback_overflow`.
    [[nodiscard]] static auto force_redraw() -> Cmd {
        return {ForceRedraw{}};
    }

    /// Schedule a HARD inline reset on the next render. See
    /// `ResetInline` doc above — destructive scrollback wipe, for
    /// wholesale model swaps only.
    [[nodiscard]] static auto reset_inline() -> Cmd {
        return {ResetInline{}};
    }

    /// Toggle the inline monotonic-height hold. See `SetHeightHold`.
    [[nodiscard]] static auto set_height_hold(bool on) -> Cmd {
        return {SetHeightHold{on}};
    }

    /// Suspend the TUI for an interactive child. See `Suspend`.
    template <std::invocable F>
        requires std::convertible_to<std::invoke_result_t<F>, Msg>
    [[nodiscard]] static auto suspend(F&& f) -> Cmd {
        return {Suspend{std::forward<F>(f)}};
    }

    /// Combine multiple Cmds. Flattens nested batches and strips Nones.
    [[nodiscard]] static auto batch(std::vector<Cmd> cmds) -> Cmd {
        std::vector<Cmd> flat;
        flat.reserve(cmds.size());
        for (auto& c : cmds) {
            if (std::holds_alternative<None>(c.inner)) continue;
            if (auto* b = std::get_if<Batch>(&c.inner)) {
                for (auto& inner : b->cmds)
                    flat.push_back(std::move(inner));
            } else {
                flat.push_back(std::move(c));
            }
        }
        if (flat.empty()) return none();
        if (flat.size() == 1) return std::move(flat[0]);
        return {Batch{std::move(flat)}};
    }

    template <typename... Cmds>
        requires (sizeof...(Cmds) > 1 && (std::same_as<std::remove_cvref_t<Cmds>, Cmd> && ...))
    [[nodiscard]] static auto batch(Cmds&&... cmds) -> Cmd {
        std::vector<Cmd> v;
        v.reserve(sizeof...(Cmds));
        (v.push_back(std::forward<Cmds>(cmds)), ...);
        return batch(std::move(v));
    }

    // -- Functor map ----------------------------------------------------------
    // Transform the Msg type. If you have a child component with its own
    // Msg type, map() lets you embed its Cmds into the parent's Cmd<ParentMsg>.
    //
    //   Cmd<ChildMsg> child_cmd = child_update(child_model, child_msg);
    //   Cmd<ParentMsg> parent_cmd = child_cmd.map([](ChildMsg m) {
    //       return ParentMsg{ChildEvent{std::move(m)}};
    //   });

    template <std::invocable<Msg> F>
    [[nodiscard]] auto map(F&& f) const -> Cmd<std::invoke_result_t<F, Msg>> {
        using B = std::invoke_result_t<F, Msg>;
        return std::visit(overload{
            [](const None&)           -> Cmd<B> { return Cmd<B>::none(); },
            [](const Quit&)           -> Cmd<B> { return Cmd<B>::quit(); },
            [&](const After& a)       -> Cmd<B> { return Cmd<B>::after(a.delay, f(a.msg)); },
            [](const SetTitle& s)     -> Cmd<B> { return Cmd<B>::set_title(s.title); },
            [](const WriteClipboard& w) -> Cmd<B> { return Cmd<B>::write_clipboard(w.content); },
            [](const QueryClipboard&) -> Cmd<B> { return Cmd<B>::query_clipboard(); },
            [&](const Batch& b)       -> Cmd<B> {
                std::vector<Cmd<B>> mapped;
                mapped.reserve(b.cmds.size());
                for (auto& c : b.cmds)
                    mapped.push_back(c.map(f));
                return Cmd<B>::batch(std::move(mapped));
            },
            [&](const Task& t) -> Cmd<B> {
                return Cmd<B>::task(
                    [run = t.run, mapper = std::forward<F>(f)]
                    (std::function<void(B)> dispatch) {
                        run([&mapper, &dispatch](Msg m) {
                            dispatch(mapper(std::move(m)));
                        });
                    });
            },
            [&](const IsolatedTask& t) -> Cmd<B> {
                return Cmd<B>::task_isolated(
                    [run = t.run, mapper = std::forward<F>(f)]
                    (std::function<void(B)> dispatch) {
                        run([&mapper, &dispatch](Msg m) {
                            dispatch(mapper(std::move(m)));
                        });
                    });
            },
            [](const CommitScrollback& c) -> Cmd<B> {
                // Direct variant construction, not the smart
                // constructor: the int overload is deprecated for
                // hosts (guessed counts), but map() merely TRANSPORTS
                // an already-minted count across a Msg-type boundary.
                return Cmd<B>{typename Cmd<B>::CommitScrollback{c.rows}};
            },
            [](const CommitScrollbackOverflow&) -> Cmd<B> {
                return Cmd<B>::commit_scrollback_overflow();
            },
            [](const ForceRedraw&) -> Cmd<B> {
                return Cmd<B>::force_redraw();
            },
            [](const ResetInline&) -> Cmd<B> {
                return Cmd<B>::reset_inline();
            },
            [](const SetHeightHold& s) -> Cmd<B> {
                return Cmd<B>::set_height_hold(s.on);
            },
            [&](const Suspend& s) -> Cmd<B> {
                return Cmd<B>::suspend(
                    [run = s.run, mapper = std::forward<F>(f)]() -> B {
                        return mapper(run());
                    });
            },
        }, inner);
    }

    // -- Queries --------------------------------------------------------------

    [[nodiscard]] bool is_none() const noexcept {
        return std::holds_alternative<None>(inner);
    }

    [[nodiscard]] bool is_quit() const noexcept {
        return std::holds_alternative<Quit>(inner);
    }
};

} // namespace maya
