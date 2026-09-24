// examples/jaal_terminal_fx.cpp — every terminal effect the jaal host carries.
//
// A small checklist program: each key fires one effect and the screen says
// what came back. It exists so each effect agentty needs is exercised on a
// real tty (tests/jaal_smoke.py drives it), not just compiled.
//
//   t   set_title           c   write_clipboard (OSC 52)
//   o   emit_host_sequence  r   force_redraw
//   i   reset_inline        s   suspend: run `sh -c 'echo …'` on the real tty,
//   q   quit                    and fold how it exited back in (jaal D39)
//
// Built only with -DMAYA_WITH_JAAL=ON.

#include <maya/app/jaal_host.hpp>
#include <maya/maya.hpp>
#include <maya/style/schemes.hpp>

#include <cstdlib>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <sys/wait.h>

using namespace maya;
using namespace maya::dsl;

struct TerminalFx {
    struct Model {
        std::vector<std::string> log;
        int n = 0;
    };

    struct Title {}; struct Clip {}; struct Osc {}; struct Redraw {}; struct Reset {};
    struct RunChild {};
    struct ChildExited { int code; };
    struct Quit {};
    using Msg = std::variant<Title, Clip, Osc, Redraw, Reset, RunChild, ChildExited, Quit>;

    using Cmd = terminal_cmd<Msg>;
    using Sub = jaal::Sub<Msg, on_key>;

    static void note(Model& m, std::string s) {
        m.log.push_back(std::to_string(++m.n) + ". " + std::move(s));
        if (m.log.size() > 8) m.log.erase(m.log.begin());
    }

    static Cmd update(Model& m, Title)  { note(m, "set_title");          return Cmd(SetTitle{"terminal fx " + std::to_string(m.n)}); }
    static Cmd update(Model& m, Clip)   { note(m, "write_clipboard");    return Cmd(WriteClipboard{"from jaal"}); }
    static Cmd update(Model& m, Osc)    { note(m, "emit_host_sequence"); return Cmd(osc(9, "jaal says hi")); }
    static Cmd update(Model& m, Redraw) { note(m, "force_redraw");       return Cmd(ForceRedraw{}); }
    static Cmd update(Model& m, Reset)  { note(m, "reset_inline");       return Cmd(ResetInline{}); }
    static Cmd update(Model& m, RunChild) {
        note(m, "suspend ->");
        return Cmd::suspend([]() -> Msg {
            const int st = std::system("sh -c 'echo child ran on the real tty; exit 3'");
            return ChildExited{WIFEXITED(st) ? WEXITSTATUS(st) : -1};
        });
    }
    static Cmd update(Model& m, ChildExited e) {
        note(m, "child exited " + std::to_string(e.code));
        return {};
    }
    static Cmd update(Model&, Quit) { return Cmd::quit(0); }

    static Element view(const Model& m) {
        std::vector<Element> rows;
        rows.push_back(text("terminal fx on jaal") | Bold);
        rows.push_back(text("t title  c clip  o osc  r redraw  i reset  s suspend  q quit") | Dim);
        for (const auto& l : m.log) rows.push_back(text(l));
        // Narrower than the terminal (content-sized, like agentty's frame),
        // so the theme test checks the fill past the frame's own border.
        return v(std::move(rows)) | pad<1> | border_<Round> | width(72);
    }

    static Sub subscribe(const Model&) {
        return jaal_key_map<Sub>({
            {'t', Title{}}, {'c', Clip{}}, {'o', Osc{}}, {'r', Redraw{}},
            {'i', Reset{}}, {'s', RunChild{}}, {'q', Quit{}},
        });
    }
};

static_assert(JaalView<TerminalFx>);

int main(int argc, char** argv) {
    // --dracula: a scheme that OWNS its canvas (states a real background),
    // so the host must fill the frame with it (apply_theme_canvas).
    const bool canvas = argc > 1 && std::string_view(argv[1]) == "--dracula";
    return run_jaal<TerminalFx>({.title = "terminal fx",
                                 .mode  = Mode::Inline,
                                 .theme = canvas ? theme::dracula : theme::native});
}
