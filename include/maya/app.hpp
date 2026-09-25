#pragma once
// maya/app.hpp — write a terminal app. This is the include a program uses,
// and it is a table of contents: every declaration lives in maya/jaal/.
//
// maya is to jaal what Ink is to React. jaal is the runtime — model, update,
// Cmd, Sub, threads, timers, shutdown. maya is the view layer and the
// terminal device. maya/jaal/ is the seam between them, and the ONLY place
// in maya that may mention jaal:
//
//     grep -rl jaal include/maya --include=*.hpp | grep -v maya/jaal/
//
// prints this file and nothing else, and tests/seam.sh checks it in CI.
// docs/internals/design.md has the rules.
//
//   interop.hpp   maya's value types, as jaal sees them (Sendable/Frozen)
//   effects.hpp   what only the TERMINAL can do — set_title, commit_scrollback,
//                 write_clipboard, suspend, ... A program lists the ones it
//                 uses in its Cmd, so a host that can't do them won't compile.
//   sources.hpp   what the terminal REPORTS — on_key, on_mouse, on_paste,
//                 on_focus, on_resize; the Program concept; keys<Sub>().
//   host.hpp      terminal_host: the class that makes maya a jaal host.
//   run.hpp       run<P>(): open the Screen, hand it to jaal.
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
//
// Link maya::app.

#include "jaal/interop.hpp"
#include "jaal/effects.hpp"
#include "jaal/sources.hpp"
#include "jaal/host.hpp"
#include "jaal/run.hpp"
