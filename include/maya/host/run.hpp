#pragma once
// maya/host/run.hpp — write a terminal app. This is the include a program
// uses, and the whole of maya comes with it.
//
// maya is to jaal what Ink is to React. jaal runs the program — model,
// update, Cmd, Sub, threads, timers, shutdown. maya is the view layer and
// the terminal it draws on. maya/host/ is where maya meets a runtime:
//
//   interop.hpp    maya's value types, as jaal sees them (Sendable/Frozen)
//   effects.hpp    what only the TERMINAL can do — set_title, commit_scrollback,
//                  write_clipboard, suspend, ... A program lists the ones it
//                  uses in its Cmd, so a host that can't do them won't compile.
//   sources.hpp    what the terminal REPORTS — on_key, on_mouse, on_paste,
//                  on_focus, on_resize; the Program concept; keys<Sub>().
//   terminal.hpp   terminal_host: maya as a jaal host.
//   run.hpp        this file: run<P>().
//
// Everything below maya/host/ is runtime-agnostic — the elements, the
// layout, the widgets, the renderer, the device. That is the whole point of
// the split, and tests/seam.sh keeps it true.
//
// A program is an ordinary jaal program with a view():
//
//   #include <maya/host/run.hpp>
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
//
// The entry point is four lines, and that is the design working: the loop is
// jaal's, the terminal is Screen's, and run() only introduces them. If it
// ever grows a branch on program state, something that belongs in a
// Program's update() has leaked into the host.
//
// Link maya::app. docs/internals/design.md has the rules.

#include <utility>

#include <jaal/jaal.hpp>

#include "../device/options.hpp"
#include "../screen.hpp"
#include "interop.hpp"
#include "effects.hpp"
#include "sources.hpp"
#include "terminal.hpp"

namespace maya {

/// Run a Program on jaal, in this terminal. The one entry point.
///
/// Exit codes: the program's own via Cmd::quit(n), or 70 (EX_SOFTWARE) if
/// the terminal can't be opened — there is no screen to report on, so it is
/// the one failure run() answers for itself.
template <Program P>
int run(Options cfg = {}, jaal::run_options opt = {}) {
    // Report a missing handle()/start_source() in jaal's words — it names
    // the effect and the program. Without this the error still happens, as
    // an unsatisfied constraint on jaal::run several levels down.
    jaal::require_host_for<terminal_host<P>, P>();

    auto term = Screen::open(cfg);
    if (!term) return 70;                         // couldn't take the terminal
    terminal_host<P> host{*term, cfg.fps};
    return jaal::run<P>(host, std::move(opt));
}

}  // namespace maya
