// examples/view_fault.cpp — what happens when view() throws.
//
// A draw can't change the model, so jaal treats a throwing view() as a
// FAULT with model_kept: the fault is reported with fault_site::view, the
// program's policy decides (stop by default, skip carries on with the last
// good frame), and the terminal is given back either way. Before the host
// caught it, the exception unwound out of the loop and the program died
// with SIGABRT and no report.
//
// Keys: x throw once   s throw every frame   q quit
//
// Run it with MAYA_VIEW_FAULT_SKIP=1 to see the skip policy: the screen
// freezes on the last good frame, the program keeps taking keys.

#include <maya/app.hpp>

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <variant>

using namespace maya;
using namespace maya::dsl;

namespace {

struct Model {
    int  count   = 0;
    bool always  = false;    // throw on every frame from now on
    bool once    = false;    // throw on the next frame only
};

struct Inc {}; struct ThrowOnce {}; struct ThrowAlways {}; struct Quit {};
using Msg = std::variant<Inc, ThrowOnce, ThrowAlways, Quit>;

struct ViewFault {
    using Model = ::Model;
    using Msg   = ::Msg;
    using Cmd   = jaal::Cmd<Msg>;
    using Sub   = jaal::Sub<Msg, on_key>;

    static Cmd update(Model& m, Inc)         { ++m.count; return {}; }
    static Cmd update(Model& m, ThrowOnce)   { m.once = true; return {}; }
    static Cmd update(Model& m, ThrowAlways) { m.always = true; return {}; }
    static Cmd update(Model&,   Quit)        { return Cmd::quit(0); }

    static Element view(const Model& m) {
        if (m.always || m.once) throw std::runtime_error("view() threw on purpose");
        return v(text("count: " + std::to_string(m.count)) | Bold,
                 t<"[+] count  [x] throw once  [s] throw always  [q] quit"> | Dim)
               | pad<1> | border_<Round>;
    }

    static Sub subscribe(const Model&) {
        return keys<Sub>({{'+', Inc{}}, {'x', ThrowOnce{}}, {'s', ThrowAlways{}},
                          {'q', Quit{}}, {SpecialKey::Escape, Quit{}}});
    }
};

static_assert(Program<ViewFault>);

}  // namespace

int main() {
    jaal::run_options opt;
    if (const char* skip = std::getenv("MAYA_VIEW_FAULT_SKIP"); skip && *skip == '1')
        opt.kernel.on_fault = jaal::fault_policy::skip;
    return run<ViewFault>({.title = "view fault"}, std::move(opt));
}
