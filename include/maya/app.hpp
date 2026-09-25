#pragma once
// maya/app.hpp — run a program in the terminal. maya is to jaal what Ink is
// to React: jaal is the runtime (model, update, Cmd, Sub, threads, timers,
// shutdown), maya draws it (elements, layout, widgets, the terminal device).
// This header is the one place they meet; link maya::app.
// docs/internals/design.md has the rules.
//
// The host protocol (jaal/kernel/run.hpp), each step one Screen call:
//
//   attach(cx)       watch screen.input_handle() with jaal's reactor
//   on_ready(cx, r)  screen.read(); emit each event ON ITS OWN, so the
//                    program re-subscribes between them (jaal D13: maya's
//                    "^T m o" bug, fixed by construction)
//   on_signal(resize) screen.on_resize(); emit a ResizeEvent
//   present(k)       screen.present(build view(model)) -> Presented; schedule
//                    Presented::redraw_at, honour redraw_now / backpressured
//   owes_frame()     an animation deadline arrived, a redraw is owed, or
//   wait_hint()      the tty is backed up: jaal wakes for those, and sleeps
//                    otherwise (an idle app costs nothing)
//   handle(effect)   terminal effects: title, clipboard, scrollback, suspend
//
// A program for this host is an ordinary jaal program with a view():
//
//   struct Counter {
//       struct Model { int n = 0; };
//       struct Inc {}; struct Quit {};
//       using Msg = std::variant<Inc, Quit>;
//       using Cmd = jaal::Cmd<Msg>;
//       using Sub = jaal::Sub<Msg, maya::on_key>;
//       static Cmd update(Model& m, Inc)  { ++m.n; return {}; }
//       static Cmd update(Model&, Quit)   { return Cmd::quit(0); }
//       static maya::Element view(const Model& m) { return text(...); }
//       static Sub subscribe(const Model&) {
//           return maya::keys<Sub>({{'+', Inc{}}, {'q', Quit{}}});
//       }
//   };
//   int main() { return maya::run<Counter>({.title = "counter"}); }

#include <algorithm>
#include <string>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <variant>

#include <jaal/jaal.hpp>

#include "maya.hpp"            // an app needs the whole view layer: one include
#include "element/element.hpp"
#include "terminal/input.hpp"
#include "app/app.hpp"
#include "screen.hpp"

namespace maya {

// ── maya's value types, as jaal sees them ───────────────────────────────────────
// jaal's Sendable walks a type's fields to prove a Msg is safe to hand to
// another thread. Strong<Tag, T> (Columns, Rows — inside every MouseEvent and
// Size) has user-declared constructors, so it isn't an aggregate and jaal
// can't look inside; it rejects it. The first ports each re-declared it safe
// in their own file. It's declared once, here, and CONDITIONALLY: a
// Strong<Tag, T> is exactly as safe as the T it wraps. (A blanket "true"
// would also bless Strong<Tag, std::string_view>, a borrowed view — the one
// thing Sendable exists to stop.)

}  // namespace maya

template <class Tag, class T>
inline constexpr bool jaal::sendable_opt_in<maya::Strong<Tag, T>> = jaal::Sendable<T>;
template <class Tag, class T>
inline constexpr bool jaal::frozen_opt_in<maya::Strong<Tag, T>> = jaal::Frozen<T>;
// ScrollbackDebt is one int behind a private constructor (only the ledger
// mints one), so jaal can't look inside — but there's nothing inside to
// share. It rides in a Cmd (commit_scrollback), which must be Sendable.
template <> inline constexpr bool jaal::sendable_opt_in<maya::ScrollbackDebt> = true;
template <> inline constexpr bool jaal::frozen_opt_in<maya::ScrollbackDebt>   = true;

namespace maya {

// Optional hooks a program may define, detected here.
namespace detail {
// Optional Program::visual_hash detector. When a Program type defines
// `static std::uint64_t visual_hash(const Model&)`, the host
// hashes the model just before calling view() and skips the view()
// + render() pair when the hash is unchanged since the last render.
// Cuts the wasted work for Tick-driven wakeups whose deltas don't
// affect anything visible (smoothing pacer drained 0 bytes, spinner
// frame unchanged because the bucket didn't roll over, status
// toast already cleared).
template <typename P, typename = void>
struct HasVisualHash : std::false_type {};
template <typename P>
struct HasVisualHash<P, std::void_t<decltype(
    P::visual_hash(std::declval<const typename P::Model&>()))>>
    : std::true_type {};

// Optional Program::needs_warmup detector. When a Program type defines
// `static bool needs_warmup(const Model&)` AND it returns true for the
// current model, the host performs an off-wire warmup_render of
// the same view BEFORE the user-visible render. The warmup populates
// maya's hash-keyed component cache; the user-visible render then
// takes the cell-blit fast path. Burns one extra render() worth of
// CPU off-frame to convert a tens-to-hundreds-of-ms cold paint into a
// sub-millisecond warm paint — the right trade after a model swap
// that loads a large frozen scrollback (agentty thread resume).
//
// The Program is responsible for clearing the flag on the next reducer
// step so warmup fires exactly once per swap; leaving it stuck on
// would double every frame's render cost.
template <typename P, typename = void>
struct HasNeedsWarmup : std::false_type {};
template <typename P>
struct HasNeedsWarmup<P, std::void_t<decltype(
    P::needs_warmup(std::declval<const typename P::Model&>()))>>
    : std::true_type {};
} // namespace detail

// ── what the host reports, as subscription kinds ───────────────────────────
// One router per event kind, so a program subscribes to exactly what it
// uses and a host that doesn't produce, say, mouse events would reject a
// program that asks for them at compile time.
using on_key    = jaal::router<KeyEvent,    "on_key">;
using on_mouse  = jaal::router<MouseEvent,  "on_mouse">;
using on_paste  = jaal::router<PasteEvent,  "on_paste">;
using on_focus  = jaal::router<FocusEvent,  "on_focus">;
using on_resize = jaal::router<ResizeEvent, "on_resize">;

/// A jaal program that maya can draw: it has a view() returning an Element.
template <class P>
concept Program = jaal::Program<P> && jaal::Viewable<P, Element>;

// ── terminal effects ──────────────────────────────────────────────────────────
// Things only the TERMINAL can do, so they're this host's effects rather than
// jaal core ones. A program lists the ones it uses in its Cmd:
//
//     using Cmd = jaal::Cmd<Msg, commit_scrollback>;
//     return commit_from(m.frozen.harvest());
//
// and a host that can't do them (a test host, a GUI) won't compile against
// it.

/// Commit rows of the last inline frame to the terminal's scrollback. Use it when view() is about to return
/// a shorter tree — a chat that virtualises old messages — so the row-diff
/// renderer doesn't read the shrink as rows removed from the bottom and
/// erase them. No effect in fullscreen.
///
/// It carries maya's TYPED ScrollbackDebt, not an int, on purpose. A debt
/// can only be minted by ScrollbackLedger::harvest(), whose rows were
/// recorded by maya's own paint pass, so a program structurally can't
/// commit a row count that drifts from what's on the wire — maya deprecated
/// its raw-int commit for exactly that reason. An `int rows` payload here
/// would have quietly thrown that guarantee away.
struct CommitScrollback { ScrollbackDebt debt; };
using commit_scrollback = jaal::pure_fx<CommitScrollback, "commit_scrollback">;

/// The Cmd for a harvested debt: nothing to do when it's empty.
template <class C>
[[nodiscard]] C commit_from(ScrollbackDebt debt) {
    if (debt.empty()) return C{};
    return C(CommitScrollback{debt});
}

/// Set the terminal window title.
struct SetTitle { std::string title; };
using set_title = jaal::pure_fx<SetTitle, "set_title">;

/// Put text on the system clipboard (OSC 52).
struct WriteClipboard { std::string text; };
using write_clipboard = jaal::pure_fx<WriteClipboard, "write_clipboard">;

/// Ask the terminal for its clipboard (OSC 52 read). The reply arrives as a
/// PasteEvent, so subscribe with on_paste.
struct QueryClipboard {};
using query_clipboard = jaal::pure_fx<QueryClipboard, "query_clipboard">;

/// An already-formed control sequence for a cooperating HOST terminal
/// (editor OSC hooks, notifications), out of band with the frame renderer.
struct EmitHostSequence { std::string sequence; };
using emit_host_sequence = jaal::pure_fx<EmitHostSequence, "emit_host_sequence">;

/// `ESC ] code ; payload ST`.
[[nodiscard]] inline EmitHostSequence osc(int code, std::string_view payload) {
    std::string seq = "\x1b]" + std::to_string(code) + ';';
    seq.append(payload);
    seq += "\x1b\\";
    return {std::move(seq)};
}

/// Commit whatever of the last inline frame overflowed the viewport.
struct CommitOverflow {};
using commit_overflow = jaal::pure_fx<CommitOverflow, "commit_overflow">;

/// Repaint everything from scratch.
struct ForceRedraw {};
using force_redraw = jaal::pure_fx<ForceRedraw, "force_redraw">;

/// Drop the inline frame's history and start a fresh one below it.
struct ResetInline {};
using reset_inline = jaal::pure_fx<ResetInline, "reset_inline">;

/// Turn mouse capture on or off (off lets the terminal's own text
/// selection work). Starts as Options::mouse says.
struct SetMouse { bool on; };
using set_mouse = jaal::pure_fx<SetMouse, "set_mouse">;

/// Hand the real terminal to an interactive child (sudo, $EDITOR, a pager)
/// and fold the result back in. `run` executes on the
/// loop thread with the TUI torn down to a cooked tty; its return value is
/// the host's ANSWER (jaal D39), folded in the same step. It's a callback
/// because the child IS the effect, but it never leaves the loop thread,
/// and the answer is an ordinary message, so replay folds the result and
/// never re-runs the child.
template <class Msg> struct Suspend { std::function<Msg()> run; };
struct suspend {
    static constexpr std::string_view name = "suspend";
    template <class Msg> using type = Suspend<Msg>;
    template <class F, class M>
    static auto fmap(F&& f, Suspend<M> e) -> Suspend<std::invoke_result_t<F, M>> {
        return {[g = std::move(e.run), f = std::forward<F>(f)] { return f(g()); }};
    }
    template <class Id, class F, class M>
    static auto fmap_with(const Id& id, F&& f, Suspend<M> e)
        -> Suspend<std::invoke_result_t<F, const Id&, M>> {
        return {[g = std::move(e.run), f = std::forward<F>(f), id] { return f(id, g()); }};
    }
    template <class Self, class Msg> struct ctors {
        template <class F> requires std::is_invocable_r_v<Msg, F&>
        [[nodiscard]] static Self suspend(F f) { return Self(Suspend<Msg>{std::move(f)}); }
    };
};

/// Every terminal effect this host can carry out, for a program that wants
/// the whole set: `jaal::Cmd<Msg, maya::terminal_fx>` is not valid (a row
/// is a list), so spell it as `terminal_cmd<Msg>`.
template <class Msg>
using terminal_cmd = jaal::Cmd<Msg, commit_scrollback, set_title, write_clipboard,
                               query_clipboard, emit_host_sequence, commit_overflow,
                               force_redraw, reset_inline, set_mouse, suspend>;

// ── keys: the most common subscription ───────────────────────────────────────────
// A table from keys to messages, as a subscription in the program's own Sub
// type (so the router is checked against the program's row like any other):
//
//   static Sub subscribe(const Model&) {
//       return keys<Sub>({{'q', Quit{}}, {SpecialKey::Up, Inc{}}});
//   }
//
// Matching is key_is(): a plain key with no modifiers, so 'q' doesn't also
// fire on Ctrl+Q. For anything richer, write Sub::on(on_key{}, fn).
using KeySpec = std::variant<char, SpecialKey>;

template <class S>
[[nodiscard]] S keys(
    std::initializer_list<std::pair<KeySpec, typename S::msg_type>> entries) {
    using Msg = typename S::msg_type;
    return S::on(on_key{},
        [table = std::vector(entries.begin(), entries.end())](const KeyEvent& k)
            -> std::optional<Msg> {
            for (const auto& [key, msg] : table) {
                const bool hit = std::visit([&](auto want) { return key_is(k, want); }, key);
                if (hit) return msg;
            }
            return std::nullopt;
        });
}

// ── the host ───────────────────────────────────────────────────────────────
//
// jaal's side of maya::Screen. The host owns no terminal logic: it watches
// the Screen's input handle, routes what read() parses, and turns jaal's
// "the model changed" into Screen::present(). Everything a draw needs from
// the scheduler comes back in Presented (an animation deadline, a redraw
// the frame asked for, a backed-up tty): the host reads no globals.
template <Program P>
class terminal_host {
public:
    // Everything the terminal produces. signal_event is added by jaal's
    // run() (kernel_event_t), which is how SIGWINCH arrives.
    using event_type = std::variant<KeyEvent, MouseEvent, PasteEvent, FocusEvent, ResizeEvent>;
    using clock      = Presented::clock;

    /// `fps` > 0 redraws continuously at that rate (Options::fps), for a
    /// program whose view() reads the wall clock itself: a clock, a
    /// throughput graph, an FPS counter. 0 (the default) is event-driven:
    /// draw only when the model changes or a widget asks.
    explicit terminal_host(Screen& term, int fps = 0) noexcept
        : term_(term),
          frame_period_(fps > 0 ? std::chrono::nanoseconds(1'000'000'000LL / fps)
                                : std::chrono::nanoseconds::zero()) {}

    terminal_host(const terminal_host&)            = delete;
    terminal_host& operator=(const terminal_host&) = delete;

    // Register the terminal's input with jaal's reactor, and tell the
    // program its size before it ever draws.
    void attach(jaal::host_context<terminal_host>& cx) {
        auto reg = cx.watch(term_.input_handle(), jaal::interest::read, kInput);
        if (!reg) { cx.stop(70); return; }        // no input: nothing to drive us
        input_reg_.emplace(std::move(*reg));
        cx_ = &cx;
        const auto sz = term_.size();
        cx.emit(ResizeEvent{sz.width, sz.height});
    }

    void on_ready(jaal::host_context<terminal_host>& cx, const jaal::readiness& r) {
        if (r.token == kOutput) {
            // The tty took bytes again: push the rest of the backed-up frame.
            // Only when ALL of it has gone is the next frame composed (a
            // frame diffed against a front that isn't on screen yet would be
            // wrong); then stop watching for writability.
            if (term_.flush()) watch_output(false);
            return;
        }
        if (r.token != kInput) return;
        if (r.hangup) { cx.stop(0); return; }     // the terminal went away
        // Leftovers from the last read first: a navigation key ended that
        // batch early so its frame could be drawn (see below).
        const bool resuming = !held_.empty();
        std::vector<Event> events = std::exchange(held_, {});
        if (auto fresh = term_.read(); !fresh) { cx.stop(70); return; }
        else events.insert(events.end(), std::make_move_iterator(fresh->begin()),
                           std::make_move_iterator(fresh->end()));
        // The read ended inside a sequence (a lone ESC so far): note when, so
        // wait_hint() can wake us to resolve it if no more bytes come.
        // Stamp only the START of a pending run: a later read (the frame ack,
        // which also arrives as input) must not push the deadline back.
        if (!term_.has_pending_input()) escape_since_.reset();
        else if (!escape_since_) escape_since_ = clock::now();
        // A navigation key's frame is still owed (input is held): this read
        // may be just the terminal's frame ack arriving. Queue what came in
        // behind the held keys, and let present() resume them after the
        // frame. Emitting here would fold the next key before its frame and
        // skip a row (measured: rows 1, 3, 5, ... of a burst).
        if (!events.empty() && resuming) { held_ = std::move(events); return; }
        emit_until_navigation(cx, events);
    }

    // Emit events one at a time; stop after a NAVIGATION key (arrows,
    // Home/End, PgUp/PgDn, Tab) and hold the rest for the next wakeup.
    //
    // A fast terminal delivers a whole key-repeat run in one read. Folding
    // it all before drawing is right for typing (the end state is all anyone
    // wants) and wrong for navigation: holding Down through a list, the rows
    // in between were computed and never shown, so the cursor visibly
    // jumped 2 -> 4 -> 6. The cursor IS the feedback for an arrow key, so
    // each one is drawn. (maya's old loop did this too; it's the same rule.)
    // A partial sequence has waited out the escape timeout.
    [[nodiscard]] bool escape_due() const noexcept {
        return escape_since_ && clock::now() - *escape_since_ >= Screen::kEscapeTimeout;
    }
    // How long until it will have.
    [[nodiscard]] std::optional<std::chrono::milliseconds> escape_wait() const noexcept {
        if (!escape_since_) return std::nullopt;
        const auto left = *escape_since_ + Screen::kEscapeTimeout - clock::now();
        return std::max(std::chrono::milliseconds(0), std::chrono::ceil<std::chrono::milliseconds>(left));
    }
    static std::optional<std::chrono::milliseconds> earliest(std::optional<std::chrono::milliseconds> a,
                                                              std::chrono::milliseconds b) noexcept {
        return a && *a < b ? *a : b;
    }

    void emit_until_navigation(jaal::host_context<terminal_host>& cx, std::vector<Event>& events) {
        // jaal folds an emitted event on the spot (route() runs update), so
        // "emit, then draw" needs the loop to turn between two navigation
        // keys: stop right after one and hold the rest.
        for (std::size_t i = 0; i < events.size(); ++i) {
            const bool nav = detail::is_navigation_key(events[i]);
            std::visit([&](auto& e) { cx.emit(event_type{std::move(e)}); }, events[i]);
            if (nav && i + 1 < events.size()) {
                held_.assign(std::make_move_iterator(events.begin() + static_cast<std::ptrdiff_t>(i) + 1),
                             std::make_move_iterator(events.end()));
                return;
            }
        }
    }

    // SIGWINCH is the one signal the host owns.
    void on_signal(jaal::host_context<terminal_host>& cx, jaal::sig s) {
        if (s != jaal::sig::resize) return;
        term_.on_resize();
        const auto sz = term_.size();
        cx.emit(ResizeEvent{sz.width, sz.height});
        dirty_ = true;                            // re-lay-out even if nobody subscribed
    }

    // The model changed, or a frame is owed: draw it.
    //
    // A program that declares visual_hash lets the Screen skip view()
    // when its pixels can't have changed; the Screen overrides that when
    // a widget asked for an animation frame or a redraw (a skipped build
    // can't re-request, so honouring the hash there would freeze an
    // animation until a keypress).
    template <class K>
    void present(K& k) {
        resolved_ = false;
        // A lone ESC (or a truncated sequence) that nothing followed: the
        // parser can only call it the Escape key once 50 ms have passed
        // with no next byte, and no byte means no input wakeup. wait_hint()
        // woke us for exactly this; resolve it and hand the key over.
        if (escape_due() && cx_) {
            escape_since_.reset();
            auto flushed = term_.resolve_pending_input();
            // Emitting folds the key now; returning with owes_frame() true
            // turns the loop once more, so its effect (a quit, a redraw)
            // is acted on in the next step rather than after the next input.
            if (!flushed.empty()) { emit_until_navigation(*cx_, flushed); resolved_ = true; return; }
        }
        const bool drew = present_frame(k);
        // A navigation key ended the last input batch so ITS frame could be
        // drawn. Only once that frame has actually gone out (not held back
        // by the ack window or a backed-up tty) does the batch resume, and
        // then only up to the NEXT navigation key: emitting folds it at
        // once, so the loop has to turn (and draw) before the one after.
        // Resuming the whole rest here drew every other row of a burst.
        if (drew && !held_.empty() && cx_) {
            auto rest = std::exchange(held_, {});
            emit_until_navigation(*cx_, rest);
        }
    }

    // Draw a frame if the Screen can take one now. True if one went out.
    template <class K>
    bool present_frame(K& k) {
        // A frame is still going out: don't compose another on top of it.
        // (The Screen diffs against what it believes is on the terminal;
        // composing now would diff against bytes the tty hasn't taken yet.)
        // The output watch drains it, and the frame is owed afterwards.
        if (term_.pending_output()) {
            if (term_.flush()) watch_output(false);
            else { watch_output(true); owed_ = true; return false; }
        }
        // The last frame hasn't reached the glass yet (no ack): hold this one.
        // The model keeps changing meanwhile; when the ack arrives (it comes
        // in as input, which wakes us) the frame drawn is the latest state,
        // so input never queues behind stale frames. On a local terminal the
        // ack is back before the next frame is due, and this never blocks.
        if (!term_.ready()) { owed_ = true; return false; }
        owed_ = false;

        const bool force = std::exchange(dirty_, false) || frame_due();
        auto build = [&] {
            // view() may call app_set_theme(); the Screen reads the theme
            // after build() returns, so a theme set here paints this frame.
            return P::view(k.model());
        };
        Presented f;
        if constexpr (jaal::HasVisualHash<P>) {
            f = term_.present_if(P::visual_hash(k.model()), build, force);
        } else {
            f = term_.present([&] {
                Element root = build();
                if constexpr (detail::HasNeedsWarmup<P>::value) warm(k, root);
                return root;
            });
        }
        if (f.redraw_now) dirty_ = true;          // drawn from stale scroll sizes
        // The tty took only part of it: watch for writability and push the
        // rest from on_ready, instead of re-rendering on a timer (which is
        // what throttled a 2.5 MB/s animation to 7 fps).
        if (term_.pending_output()) watch_output(true);
        schedule(f);
        return !f.skipped;
    }

    // One deadline for the next frame: the EARLIER of a widget's animation
    // request and the fixed-rate tick. owes_frame() and wait_hint() read it.
    void schedule(const Presented& f) {
        const auto now = clock::now();
        std::optional<clock::time_point> next = f.redraw_at;
        if (frame_period_ > std::chrono::nanoseconds::zero()) {
            // Keep PHASE: advance by whole periods, so a slow frame doesn't
            // push every later one back; resync after a stall instead of
            // firing a burst of catch-up frames (the rule jaal's `every`
            // follows too).
            if (next_tick_ == clock::time_point{})
                next_tick_ = now + frame_period_;
            else if (next_tick_ <= now) {
                next_tick_ += frame_period_;
                if (next_tick_ <= now) next_tick_ = now + frame_period_;
            }
            if (!next || next_tick_ < *next) next = next_tick_;
        }
        next_frame_at_ = next;                    // nullopt: settled, back to idle
    }

    // jaal asks after every step whether we owe a frame even though the
    // model didn't change: a redraw the last frame asked for, an animation
    // deadline that has arrived, a frame held back by pending output that
    // has now drained, or a coalesced frame the Screen still owes.
    [[nodiscard]] bool owes_frame() const noexcept {
        if (escape_due()) return true;              // a lone ESC is ready to resolve
        if (resolved_) return true;                 // ...and was: turn once more to act on it
        if (!held_.empty()) return true;            // held input resumes after a frame
        if (term_.pending_output()) return false;   // the output watch will wake us
        if (awaiting_ack()) return false;           // the ack (input) will wake us
        return dirty_ || owed_ || term_.backpressured() || frame_due();
    }

    // ...and how long it may sleep before asking again. While output is
    // pending, as long as it likes: writability wakes it, not a timer. While
    // a frame is unacknowledged, until the ack (it arrives as input) or the
    // ack timeout, whichever is first.
    [[nodiscard]] std::optional<std::chrono::milliseconds> wait_hint() const noexcept {
        if (!held_.empty() || resolved_) return std::chrono::milliseconds(0);
        std::optional<std::chrono::milliseconds> esc = escape_wait();
        if (term_.pending_output()) return std::nullopt;
        if (awaiting_ack()) {
            const auto left = *term_.ready_deadline() - clock::now();
            return earliest(esc, std::max(std::chrono::milliseconds(0),
                                          std::chrono::ceil<std::chrono::milliseconds>(left)));
        }
        if (dirty_ || owed_) return std::chrono::milliseconds(0);
        if (term_.backpressured()) return earliest(esc, std::chrono::milliseconds(4));   // a coalesced frame
        if (next_frame_at_) {
            const auto left = *next_frame_at_ - clock::now();
            if (left <= clock::duration::zero()) return std::chrono::milliseconds(0);
            return earliest(esc, std::chrono::ceil<std::chrono::milliseconds>(left));
        }
        return esc;
    }

    // Screen effects: each one is a single Screen call.
    void handle(CommitScrollback c) { term_.commit_scrollback(c.debt); }
    void handle(SetTitle t)         { term_.set_title(t.title); }
    void handle(WriteClipboard w)   { term_.write_clipboard(w.text); }
    void handle(QueryClipboard)     { term_.query_clipboard(); }
    void handle(EmitHostSequence e) { term_.emit_host_sequence(e.sequence); }
    void handle(CommitOverflow)     { term_.commit_overflow(); dirty_ = true; }
    void handle(ForceRedraw)        { term_.force_redraw();    dirty_ = true; }
    void handle(ResetInline)        { term_.reset_inline();    dirty_ = true; }
    void handle(SetMouse s)         { term_.set_mouse(s.on); }

    // Hand the real tty to an interactive child and answer with how it went
    // (jaal D39: the answer is folded in this step).
    template <class M>
    std::optional<M> handle(Suspend<M> s) {
        if (!s.run) return std::nullopt;
        M out = term_.suspend([&] { return s.run(); });
        dirty_ = true;                            // repaint over what the child left
        return out;
    }

    void release() { output_reg_.reset(); input_reg_.reset(); cx_ = nullptr; }

private:
    static constexpr std::uint64_t kInput = 1;

    [[nodiscard]] bool frame_due() const noexcept {
        return next_frame_at_ && clock::now() >= *next_frame_at_;
    }

    // A frame is owed but the last one hasn't been acknowledged (and its ack
    // timeout hasn't passed): the host waits for the terminal.
    [[nodiscard]] bool awaiting_ack() const noexcept {
        const auto d = term_.ready_deadline();
        return (owed_ || dirty_ || frame_due()) && d && clock::now() < *d;
    }

    // Watch the output for writability only while bytes are waiting: an
    // idle tty is ALWAYS writable, so a permanent watch would wake the loop
    // continuously (the rule jaal's hosts guide spells out for sockets).
    // The output is its own fd (stdout; the input is stdin), so it gets its
    // own registration and token, and the registration is dropped the
    // moment the frame is out.
    void watch_output(bool on) {
        if (on == output_reg_.has_value() || !cx_) return;
        if (!on) { output_reg_.reset(); return; }
        if (auto reg = cx_->watch(term_.output_handle(), jaal::interest::write, kOutput))
            output_reg_.emplace(std::move(*reg));
    }

    // One-shot cache warmup on the RISING edge of needs_warmup (a heavy
    // thread just rehydrated), so the visible frame takes the blit path.
    template <class K>
    void warm(K& k, const Element& root) {
        const bool want = P::needs_warmup(k.model());
        if (want && !last_warmup_) term_.warm(root);
        last_warmup_ = want;
    }

    Screen&                                                 term_;
    jaal::host_context<terminal_host>*                          cx_ = nullptr;
    std::optional<jaal::platform::native_reactor::registration> input_reg_;
    std::optional<jaal::platform::native_reactor::registration> output_reg_;   // only while output is pending
    static constexpr std::uint64_t                            kOutput = 2;
    bool                                                      dirty_ = true;
    std::vector<Event>                                        held_;   // input after a navigation key, awaiting its frame
    std::optional<clock::time_point>                          escape_since_;   // input ended mid-sequence at
    bool                                                      resolved_ = false;   // a timed-out sequence was just emitted
    bool                                                      owed_  = false;   // a frame held back by pending output
    bool                                                      last_warmup_ = false;
    std::optional<clock::time_point>                          next_frame_at_;
    std::chrono::nanoseconds                                  frame_period_;   // 0 = event-driven
    clock::time_point                                         next_tick_{};    // next fps frame
};

/// Run a Program on jaal, in this terminal. The one entry point.
template <Program P>
int run(Options cfg = {}, jaal::run_options opt = {}) {
    auto term = Screen::open(cfg);
    if (!term) return 70;                         // couldn't take the terminal
    terminal_host<P> host{*term, cfg.fps};
    return jaal::run<P>(host, std::move(opt));
}

}  // namespace maya
