#pragma once
// maya::jaal_host — maya as a jaal host.
//
// maya is two things fused together: a terminal UI TOOLKIT (elements,
// layout, rendering, widgets, text — ~113k lines) and a RUNTIME (the event
// loop, Cmd interpreter, background queue, timers — the loop in app.hpp).
// jaal is a runtime. So maya-on-jaal keeps the toolkit and replaces only the
// loop, and this file is the seam between them.
//
// It's deliberately thin. Everything that was already proven stays exactly
// where it is and is CALLED, not rewritten:
//
//   detail::Runtime   the terminal: raw mode, alt screen / inline, the input
//                     parser, the double-buffered renderer, and the RAII
//                     type-state that restores the terminal on every exit
//   read_events()     bytes -> KeyEvent / MouseEvent / PasteEvent / ...
//   render(Element)   Element tree -> diffed bytes on the terminal
//
// What jaal takes over is the part it's better at: the loop, the mailbox,
// tasks and streams with their thread-safety guarantees, faults, timers,
// replay and resume.
//
// How it plugs in (the host protocol, jaal/kernel/run.hpp):
//
//   attach(cx)      watch the terminal's input handle with jaal's reactor
//   on_ready(cx,r)  input is ready: read_events(), then emit each one. One
//                   at a time, so the program is re-subscribed between them
//                   (D13 — maya's own "^T m o" bug, which is why jaal has
//                   the rule in the first place)
//   present(k)      the model changed: view() and render the Element
//   release()       nothing to do: the Runtime's destructor restores the
//                   terminal, and jaal's teardown runs it in the right order
//
// Resize is a SIGNAL (SIGWINCH), and jaal already turns signals into events
// on the loop thread. So it arrives as signal_event{resize}: the host asks
// the Runtime for the new size and emits a ResizeEvent, exactly as maya's
// own loop does.
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
//           return Sub::on(maya::on_key{}, [](const maya::KeyEvent& k) -> std::optional<Msg> {...});
//       }
//   };
//   int main() { return maya::run_jaal<Counter>({.title = "counter"}); }

#if !MAYA_WITH_JAAL
#error "maya/app/jaal_host.hpp needs MAYA_WITH_JAAL (cmake -DMAYA_WITH_JAAL=ON)"
#endif

#include <algorithm>
#include <string>
#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

#include <jaal/jaal.hpp>

#include "../element/element.hpp"
#include "../terminal/input.hpp"
#include "app.hpp"

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
template <JaalView P>
class jaal_host {
public:
    // Everything the terminal produces. signal_event is added by jaal's
    // run() (kernel_event_t), which is how SIGWINCH arrives.
    using event_type = std::variant<KeyEvent, MouseEvent, PasteEvent, FocusEvent, ResizeEvent>;

    /// `fps` > 0 redraws continuously at that rate (RunConfig::fps), for a
    /// program whose view() reads the wall clock itself: a clock, a
    /// throughput graph, an FPS counter. 0 (the default) is event-driven:
    /// draw only when the model changes or a widget asks.
    explicit jaal_host(detail::Runtime& rt, int fps = 0) noexcept
        : rt_(rt),
          frame_period_(fps > 0 ? std::chrono::nanoseconds(1'000'000'000LL / fps)
                                : std::chrono::nanoseconds::zero()) {}

    jaal_host(const jaal_host&)            = delete;
    jaal_host& operator=(const jaal_host&) = delete;

    // 2. register the terminal's input with jaal's reactor.
    void attach(jaal::host_context<jaal_host>& cx) {
        auto reg = cx.watch(rt_.input_handle(), jaal::interest::read, kInput);
        if (!reg) { cx.stop(70); return; }        // no input: nothing to drive us
        input_reg_.emplace(std::move(*reg));

        // maya's own loop fires a resize before the first frame so view()
        // knows the terminal size. Same here: the program sees its size
        // before it ever draws.
        const auto sz = rt_.size();
        cx.emit(ResizeEvent{sz.width, sz.height});
    }

    // 3→4. input is ready: parse it and emit each event on its own.
    void on_ready(jaal::host_context<jaal_host>& cx, const jaal::readiness& r) {
        if (r.token != kInput) return;
        if (r.hangup) { cx.stop(0); return; }     // the terminal went away

        auto events = rt_.read_events();
        if (!events) { cx.stop(70); return; }
        // ONE event per emit: jaal folds it and re-subscribes before the
        // next, so a key that opens a picker routes the NEXT key to the
        // picker. That's maya's "^T m o" bug, fixed by construction.
        for (auto& ev : *events) {
            std::visit([&](auto& e) { cx.emit(event_type{std::move(e)}); }, ev);
            if (!rt_.is_running()) return;
        }
    }

    // A signal the program didn't route: SIGWINCH is the one the host owns.
    void on_signal(jaal::host_context<jaal_host>& cx, jaal::sig s) {
        if (s != jaal::sig::resize) return;
        rt_.handle_resize();
        const auto sz = rt_.size();
        cx.emit(ResizeEvent{sz.width, sz.height});
        dirty_ = true;                            // re-lay-out even if nobody subscribed
    }

    // 5. the model changed (or a frame is owed): draw it.
    //
    // Two things decide whether view() runs, exactly as in maya's own loop:
    //
    //   * visual_hash: a program that says what its pixels depend on lets
    //     us skip view() when that hasn't moved.
    //   * animation frames: a widget that called request_animation_frame()
    //     during the last build() is ASKING to be drawn again (a spinner, a
    //     caret, the motion framework's tweens). That request overrides the
    //     hash — a skipped build can't re-request, so honouring the hash
    //     there would freeze the animation until a keypress, which is the
    //     bug class maya's loop documents at length.
    template <class K>
    void present(K& k) {
        const auto now = std::chrono::steady_clock::now();
        const bool frame_due = next_frame_at_ && now >= *next_frame_at_;
        if constexpr (jaal::HasVisualHash<P>) {
            const std::uint64_t h = P::visual_hash(k.model());
            if (!dirty_ && !owes_frame() && !frame_due
                && !detail::animation_requested_ && last_hash_ && *last_hash_ == h)
                return;
            last_hash_ = h;
        }
        // present() runs after a model change, or when owes_frame() said a
        // frame is due — both mean draw now. (An animation frame that isn't
        // due yet never gets here: owes_frame() is false until it is.)
        (void)frame_due;
        dirty_ = false;

        // build() is what (re)sets the request, so clear it just before.
        detail::animation_requested_ = false;
        detail::next_frame_delay_ms_ = -1;
        (void)rt_.render(P::view(k.model()));

        schedule_next_frame();
    }


    // The next frame, if anything wants one: the EARLIER of a widget's
    // animation request and the fixed-rate tick (RunConfig::fps). One
    // deadline, so owes_frame() and wait_hint() don't need to know which.
    void schedule_next_frame() {
        const auto now = std::chrono::steady_clock::now();
        std::optional<std::chrono::steady_clock::time_point> next;
        if (detail::animation_requested_) {
            // its own minimum delay (a slow caret blink) or maya's default
            const auto delay = detail::next_frame_delay_ms_ > 0
                ? std::chrono::milliseconds(detail::next_frame_delay_ms_)
                : detail::kAnimationFrameInterval;
            next = now + delay;
        }
        if (frame_period_ > std::chrono::nanoseconds::zero()) {
            // next_tick_ is when the next fixed-rate frame is due. Advance it
            // by whole periods, so it keeps PHASE: a slow frame doesn't push
            // every later frame back (30 fps stays 30, not 30 minus the
            // render time). If we're a period or more behind (the loop
            // stalled), resync to now instead of firing a burst of catch-up
            // frames — the same rule jaal's `every` follows.
            if (next_tick_ == std::chrono::steady_clock::time_point{})
                next_tick_ = now + frame_period_;          // first frame
            else if (next_tick_ <= now) {
                next_tick_ += frame_period_;
                if (next_tick_ <= now) next_tick_ = now + frame_period_;
            }
            if (!next || next_tick_ < *next) next = next_tick_;
        }
        next_frame_at_ = next;                    // nullopt: settled, back to idle
    }

    // maya's renderer can DEFER a frame: it coalesces when the terminal is
    // congested, or leaves bytes queued a slow tty wouldn't take. Its own
    // loop answers that by polling again within a few ms; under jaal the
    // host says so. Without these, every keystroke painted the PREVIOUS
    // model (found driving this host in a real pty): the frame for the key
    // just pressed was owed and nothing asked for it until the next key.
    [[nodiscard]] bool owes_frame() const noexcept {
        if (rt_.has_pending_writes() || rt_.has_deferred_frame()) return true;
        // An animation frame that has come due is owed too: present() must
        // run even though no message changed the model.
        return next_frame_at_ && std::chrono::steady_clock::now() >= *next_frame_at_;
    }

    /// While a frame is owed, don't sleep longer than maya's own loop would:
    /// its retry band is 2-8 ms (app.hpp, "Deferred-write retry"). The floor
    /// matters as much as the ceiling: a zero wait would spin the CPU until
    /// the tty drains.
    [[nodiscard]] std::optional<std::chrono::milliseconds> wait_hint() const noexcept {
        if (rt_.has_pending_writes() || rt_.has_deferred_frame())
            return std::chrono::milliseconds(4);
        if (next_frame_at_) {
            // Wake for the next animation frame. CEIL, as maya's loop does:
            // rounding a 0.4 ms remainder down to 0 is a hot spin for the
            // sub-millisecond tail of every frame.
            const auto left = *next_frame_at_ - std::chrono::steady_clock::now();
            if (left <= std::chrono::steady_clock::duration::zero())
                return std::chrono::milliseconds(0);
            return std::chrono::ceil<std::chrono::milliseconds>(left);
        }
        return std::nullopt;
    }

    // Host effects: the terminal operations a program can ask for (listed in
    // its Cmd). Each is the call maya's own Cmd interpreter makes.
    void handle(CommitScrollback c) { rt_.commit_inline_prefix(c.debt.rows()); }
    void handle(SetTitle t)         { rt_.set_title(t.title); }

    // 6. nothing: the Runtime's destructor restores the terminal, and
    //    jaal's teardown (kernel/teardown.hpp) has already taken the signal
    //    handlers off by the time this runs.
    void release() {}

private:
    static constexpr std::uint64_t kInput = 1;

    detail::Runtime&                                          rt_;
    std::optional<jaal::platform::native_reactor::registration> input_reg_;
    std::optional<std::uint64_t>                              last_hash_;
    bool                                                      dirty_ = true;
    // The next animation frame a widget asked for, if any.
    std::optional<std::chrono::steady_clock::time_point>      next_frame_at_;
    std::chrono::nanoseconds                                  frame_period_;   // 0 = event-driven
    std::chrono::steady_clock::time_point                     next_tick_{};    // next fps frame
};

// ── the entry point ────────────────────────────────────────────────────────
/// Run a jaal program on the terminal. The maya equivalent of
/// jaal::run<P>(): same RunConfig as maya::run, same terminal handling, and
/// jaal's loop underneath.
template <JaalView P>
int run_jaal(RunConfig cfg = {}, jaal::run_options opt = {}) {
    auto rt = detail::Runtime::create(cfg);
    if (!rt) return 70;                           // couldn't take the terminal
    rt->publish_theme_slot();
    jaal_host<P> host{*rt, cfg.fps};
    return jaal::run<P>(host, std::move(opt));
}

}  // namespace maya
