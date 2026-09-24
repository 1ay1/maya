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

// ── the host ───────────────────────────────────────────────────────────────
template <JaalView P>
class jaal_host {
public:
    // Everything the terminal produces. signal_event is added by jaal's
    // run() (kernel_event_t), which is how SIGWINCH arrives.
    using event_type = std::variant<KeyEvent, MouseEvent, PasteEvent, FocusEvent, ResizeEvent>;

    explicit jaal_host(detail::Runtime& rt) noexcept : rt_(rt) {}

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

    // 5. the model changed: draw it. view() is pure, render() diffs.
    template <class K>
    void present(K& k) {
        if constexpr (jaal::HasVisualHash<P>) {
            // A program that says what its pixels depend on lets us skip
            // view() entirely when that hasn't moved (same contract as
            // maya's own loop) — unless a frame is still owed, in which
            // case skipping would strand it.
            const std::uint64_t h = P::visual_hash(k.model());
            if (!dirty_ && !owes_frame() && last_hash_ && *last_hash_ == h) return;
            last_hash_ = h;
        }
        dirty_ = false;
        (void)rt_.render(P::view(k.model()));
    }

    // maya's renderer can DEFER a frame: it coalesces when the terminal is
    // congested, or leaves bytes queued a slow tty wouldn't take. Its own
    // loop answers that by polling again within a few ms; under jaal the
    // host says so. Without these, every keystroke painted the PREVIOUS
    // model (found driving this host in a real pty): the frame for the key
    // just pressed was owed and nothing asked for it until the next key.
    [[nodiscard]] bool owes_frame() const noexcept {
        return rt_.has_pending_writes() || rt_.has_deferred_frame();
    }

    /// While a frame is owed, don't sleep longer than maya's own loop would:
    /// its retry band is 2-8 ms (app.hpp, "Deferred-write retry"). The floor
    /// matters as much as the ceiling: a zero wait would spin the CPU until
    /// the tty drains.
    [[nodiscard]] std::optional<std::chrono::milliseconds> wait_hint() const noexcept {
        if (!owes_frame()) return std::nullopt;
        return std::chrono::milliseconds(4);
    }

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
    jaal_host<P> host{*rt};
    return jaal::run<P>(host, std::move(opt));
}

}  // namespace maya
