#pragma once
// maya/jaal/host.hpp — maya on jaal: the adapter between a Screen and a runtime.
//
// maya is a view layer (Element, layout, paint, widgets, theme) and a device
// (maya::Screen: the terminal, driven one non-blocking step at a time).
// jaal is the runtime: the loop, timers, tasks, streams, signals, shutdown,
// replay. This file is the only place the two meet, and it's its own
// target (maya::jaal), so maya's core never depends on the runtime.
// docs/internals/runtime-free.md has the design.
//
// The host protocol (jaal/kernel/run.hpp), each step one Screen call:
//
//   attach(cx)       watch screen.input_handle() with jaal's reactor
//   on_ready(cx, r)  screen.read(); emit each event ON ITS OWN, so the
//                    program re-subscribes between them (jaal D13: maya's
//                    "^T m o" bug, fixed by construction)
//   on_signal(resize) screen.on_resize(); emit a ResizeEvent
//   present(k)       screen.present(build view(model)) -> Frame; schedule
//                    Frame::redraw_at, honour redraw_now / backpressured
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
//           return maya::jaal_key_map<Sub>({{'+', Inc{}}, {'q', Quit{}}});
//       }
//   };
//   int main() { return maya::run_jaal<Counter>({.title = "counter"}); }

#if !MAYA_WITH_JAAL
#error "maya/jaal/host.hpp needs MAYA_WITH_JAAL (cmake -DMAYA_WITH_JAAL=ON)"
#endif

#include <algorithm>
#include <string>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <variant>

#include <jaal/jaal.hpp>

#include "../element/element.hpp"
#include "../terminal/input.hpp"
#include "../app/app.hpp"
#include "../screen.hpp"

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
concept JaalView = jaal::Program<P> && jaal::Viewable<P, Element>;

// ── terminal effects ──────────────────────────────────────────────────────────
// Things only the TERMINAL can do, so they're this host's effects rather than
// jaal core ones. A program lists the ones it uses in its Cmd:
//
//     using Cmd = jaal::Cmd<Msg, commit_scrollback>;
//     return commit_from(m.frozen.harvest());
//
// and a host that can't do them (a test host, a GUI) won't compile against
// it. They're the same operations as maya's own Cmd alternatives, which is
// what makes a port a rename rather than a redesign.

/// Commit rows of the last inline frame to the terminal's scrollback
/// (maya's Cmd::commit_scrollback). Use it when view() is about to return
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

/// Set the terminal window title (maya's Cmd::set_title).
struct SetTitle { std::string title; };
using set_title = jaal::pure_fx<SetTitle, "set_title">;

/// Put text on the system clipboard (OSC 52; maya's Cmd::write_clipboard).
struct WriteClipboard { std::string text; };
using write_clipboard = jaal::pure_fx<WriteClipboard, "write_clipboard">;

/// Ask the terminal for its clipboard (OSC 52 read). The reply arrives as a
/// PasteEvent, so subscribe with on_paste (maya's Cmd::query_clipboard).
struct QueryClipboard {};
using query_clipboard = jaal::pure_fx<QueryClipboard, "query_clipboard">;

/// An already-formed control sequence for a cooperating HOST terminal
/// (editor OSC hooks, notifications), out of band with the frame renderer.
struct EmitHostSequence { std::string sequence; };
using emit_host_sequence = jaal::pure_fx<EmitHostSequence, "emit_host_sequence">;

/// maya's Cmd::emit_osc: `ESC ] code ; payload ST`.
[[nodiscard]] inline EmitHostSequence osc(int code, std::string_view payload) {
    std::string seq = "\x1b]" + std::to_string(code) + ';';
    seq.append(payload);
    seq += "\x1b\\";
    return {std::move(seq)};
}

/// Commit whatever of the last inline frame overflowed the viewport
/// (maya's Cmd::commit_scrollback_overflow).
struct CommitOverflow {};
using commit_overflow = jaal::pure_fx<CommitOverflow, "commit_overflow">;

/// Repaint everything from scratch (maya's Cmd::force_redraw).
struct ForceRedraw {};
using force_redraw = jaal::pure_fx<ForceRedraw, "force_redraw">;

/// Drop the inline frame's history and start a fresh one below it
/// (maya's Cmd::reset_inline).
struct ResetInline {};
using reset_inline = jaal::pure_fx<ResetInline, "reset_inline">;

/// Hand the real terminal to an interactive child (sudo, $EDITOR, a pager)
/// and fold the result back in (maya's Cmd::suspend). `run` executes on the
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
                               force_redraw, reset_inline, suspend>;

// ── key_map: the most common subscription ──────────────────────────────────────
// maya's own key_map<Msg>() returns a maya::Sub, and 15 of maya's 20
// program examples use it. This is the same table, returning a subscription
// for a jaal program. The Sub type is the program's own, so the router is
// checked against the program's row like any other.
//
//   static Sub subscribe(const Model&) {
//       return jaal_key_map<Sub>({{'q', Quit{}}, {SpecialKey::Up, Inc{}}});
//   }
//
// Matching is exactly maya's key_is(): a plain key with no modifiers, so
// 'q' doesn't also fire on Ctrl+Q.
template <class S>
[[nodiscard]] S jaal_key_map(
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
// the scheduler comes back in the Frame (an animation deadline, a redraw
// the frame asked for, a backed-up tty): the host reads no globals.
template <JaalView P>
class jaal_host {
public:
    // Everything the terminal produces. signal_event is added by jaal's
    // run() (kernel_event_t), which is how SIGWINCH arrives.
    using event_type = std::variant<KeyEvent, MouseEvent, PasteEvent, FocusEvent, ResizeEvent>;
    using clock      = Frame::clock;

    /// `fps` > 0 redraws continuously at that rate (RunConfig::fps), for a
    /// program whose view() reads the wall clock itself: a clock, a
    /// throughput graph, an FPS counter. 0 (the default) is event-driven:
    /// draw only when the model changes or a widget asks.
    explicit jaal_host(Screen& term, int fps = 0) noexcept
        : term_(term),
          frame_period_(fps > 0 ? std::chrono::nanoseconds(1'000'000'000LL / fps)
                                : std::chrono::nanoseconds::zero()) {}

    jaal_host(const jaal_host&)            = delete;
    jaal_host& operator=(const jaal_host&) = delete;

    // Register the terminal's input with jaal's reactor, and tell the
    // program its size before it ever draws.
    void attach(jaal::host_context<jaal_host>& cx) {
        auto reg = cx.watch(term_.input_handle(), jaal::interest::read, kInput);
        if (!reg) { cx.stop(70); return; }        // no input: nothing to drive us
        input_reg_.emplace(std::move(*reg));
        const auto sz = term_.size();
        cx.emit(ResizeEvent{sz.width, sz.height});
    }

    // Input is ready: parse it and emit each event on its own. ONE event per
    // emit: jaal folds it and re-subscribes before the next, so a key that
    // opens a picker routes the NEXT key to the picker (maya's "^T m o" bug,
    // fixed by construction).
    void on_ready(jaal::host_context<jaal_host>& cx, const jaal::readiness& r) {
        if (r.token != kInput) return;
        if (r.hangup) { cx.stop(0); return; }     // the terminal went away
        auto events = term_.read();
        if (!events) { cx.stop(70); return; }
        for (auto& ev : *events)
            std::visit([&](auto& e) { cx.emit(event_type{std::move(e)}); }, ev);
    }

    // SIGWINCH is the one signal the host owns.
    void on_signal(jaal::host_context<jaal_host>& cx, jaal::sig s) {
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
        const bool force = std::exchange(dirty_, false) || frame_due();
        auto build = [&] {
            // view() may call app_set_theme(); the Screen reads the theme
            // after build() returns, so a theme set here paints this frame.
            return P::view(k.model());
        };
        Frame f;
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
        schedule(f);
    }

    // One deadline for the next frame: the EARLIER of a widget's animation
    // request and the fixed-rate tick. owes_frame() and wait_hint() read it.
    void schedule(const Frame& f) {
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
    // model didn't change: a backed-up tty, a redraw the last frame asked
    // for, or an animation deadline that has arrived.
    [[nodiscard]] bool owes_frame() const noexcept {
        return dirty_ || term_.backpressured() || frame_due();
    }

    // ...and how long it may sleep before asking again.
    [[nodiscard]] std::optional<std::chrono::milliseconds> wait_hint() const noexcept {
        if (dirty_) return std::chrono::milliseconds(0);
        if (term_.backpressured()) return std::chrono::milliseconds(4);
        if (next_frame_at_) {
            const auto left = *next_frame_at_ - clock::now();
            if (left <= clock::duration::zero()) return std::chrono::milliseconds(0);
            return std::chrono::ceil<std::chrono::milliseconds>(left);
        }
        return std::nullopt;
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

    // Hand the real tty to an interactive child and answer with how it went
    // (jaal D39: the answer is folded in this step).
    template <class M>
    std::optional<M> handle(Suspend<M> s) {
        if (!s.run) return std::nullopt;
        M out = term_.suspend([&] { return s.run(); });
        dirty_ = true;                            // repaint over what the child left
        return out;
    }

    void release() {}

private:
    static constexpr std::uint64_t kInput = 1;

    [[nodiscard]] bool frame_due() const noexcept {
        return next_frame_at_ && clock::now() >= *next_frame_at_;
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
    std::optional<jaal::platform::native_reactor::registration> input_reg_;
    bool                                                      dirty_ = true;
    bool                                                      last_warmup_ = false;
    std::optional<clock::time_point>                          next_frame_at_;
    std::chrono::nanoseconds                                  frame_period_;   // 0 = event-driven
    clock::time_point                                         next_tick_{};    // next fps frame
};

/// Run a Program on jaal, in this terminal. The one entry point.
template <JaalView P>
int run_jaal(RunConfig cfg = {}, jaal::run_options opt = {}) {
    auto term = Screen::open({.title = cfg.title, .mode = cfg.mode, .mouse = cfg.mouse,
                                .hover_motion = cfg.hover_motion, .backend = cfg.backend,
                                .theme = cfg.theme, .enhanced_keyboard = cfg.enhanced_keyboard});
    if (!term) return 70;                         // couldn't take the terminal
    jaal_host<P> host{*term, cfg.fps};
    return jaal::run<P>(host, std::move(opt));
}

}  // namespace maya
