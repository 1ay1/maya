// examples/editor_live.cpp — a working interactive code editor.
//
//   cmake --build build --target maya_editor_live && ./build/maya_editor_live
//
// A real editable buffer (TextEditor) with syntax highlight, cursor, shift-
// selection, undo/redo, and clipboard — wrapped in editor chrome (tab bar,
// breadcrumb, status line). Type to edit; Ctrl-Q to quit.
//
// Keys:  text/enter/backspace/delete/tab · arrows (Shift extends selection) ·
//        Home/End · Ctrl-Z undo · Ctrl-Y redo · Ctrl-A all · Ctrl-C/X/V ·
//        Ctrl-Q quit.

#include <maya/app.hpp>
#include <maya/maya.hpp>
#include <maya/widget/text_editor.hpp>
#include <maya/widget/editor_tab_bar.hpp>
#include <maya/widget/breadcrumb_bar.hpp>
#include <maya/widget/editor_status_line.hpp>

#include <string>

using namespace maya;
using namespace maya::dsl;

static const std::string kSeed =
    "#include <vector>\n"
    "#include <string>\n"
    "\n"
    "// Try editing me: type, select with Shift+arrows,\n"
    "// undo with Ctrl-Z, copy/paste with Ctrl-C / Ctrl-V.\n"
    "template <class T>\n"
    "T fib(int n, std::vector<T>& memo) {\n"
    "    if (n < 2) return n;\n"
    "    if (memo[n]) return memo[n];\n"
    "    return memo[n] = fib(n-1, memo) + fib(n-2, memo);\n"
    "}\n";

struct App {
    struct Model {
        TextEditor ed{{.lang = syntax::Lang::Cpp}};
        bool dirty = false;
    };
    struct KeyMsg { KeyEvent ev; };
    struct PasteMsg { std::string text; };
    struct Quit {};
    using Msg = std::variant<KeyMsg, PasteMsg, Quit>;

    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg, on_key, on_paste>;

    static Cmd init(Model& m) {
        m.ed.set_text(kSeed); return {};
    }

    static Cmd update(Model&, Quit) { return Cmd::quit(0); }
    static Cmd update(Model& m, KeyMsg k) {
        if (m.ed.handle(k.ev)) m.dirty = true;
        return {};
    }
    static Cmd update(Model& m, PasteMsg p) {
        PasteEvent pe; pe.content = p.text; m.ed.handle_paste(pe); m.dirty = true;
        return {};
    }

    static Element view(const Model& m) {
        EditorTabBar tabs;
        tabs.tab({.name = "fib.hpp", .modified = m.dirty}).active(0);

        Breadcrumb bc;
        bc.crumb(SymKind::Folder, "src").crumb(SymKind::File, "fib.hpp")
          .crumb(SymKind::Function, "fib");

        EditorStatusLine sl;
        sl.left(EditorStatusLine::mode("INSERT"))
          .left(EditorStatusLine::branch("main"))
          .left(EditorStatusLine::file("fib.hpp", m.dirty))
          .right(EditorStatusLine::info("Ctrl-Q quit"))
          .right(EditorStatusLine::lang("C++"))
          .right(EditorStatusLine::pos(m.ed.line(), m.ed.column(), m.ed.line_count()));

        return v(
            tabs,
            bc,
            m.ed | grow(1),
            sl
        ) | grow(1);
    }

    static Sub subscribe(const Model&) {
        return Sub::batch(
            Sub::on(on_key{}, [](const KeyEvent& k) -> std::optional<Msg> {
                if (k.mods.ctrl && std::holds_alternative<CharKey>(k.key) &&
                    std::get<CharKey>(k.key).codepoint == 'q')
                    return Quit{};
                return KeyMsg{k};
            }),
            Sub::on(on_paste{}, [](const PasteEvent& p) -> std::optional<Msg> {
                return PasteMsg{p.content};
            })
        );
    }
};

static_assert(Program<App>);

int main() { return run<App>({.title = "maya — live editor"}); }
