#pragma once
// maya/jaal/run.hpp — maya::run<P>(): open the terminal, hand it to jaal.
//
// The whole entry point is four lines, and that is the design working. The
// loop is jaal's, the terminal is Screen's, and this function only introduces
// them:
//
//     Screen::open(cfg)  ->  terminal_host<P>  ->  jaal::run<P>
//
// If it ever grows a branch on program state, something that belongs in a
// Program's update() has leaked into the host.

#include <utility>

#include <jaal/jaal.hpp>

#include "../app/options.hpp"
#include "../screen.hpp"
#include "host.hpp"

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
