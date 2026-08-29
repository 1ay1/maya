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

    static Model init() {
        Model m; m.ed.set_text(kSeed); return m;
    }

    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        if (std::holds_alternative<Quit>(msg)) return {std::move(m), Cmd<Msg>::quit()};
        if (auto* k = std::get_if<KeyMsg>(&msg)) { if (m.ed.handle(k->ev)) m.dirty = true; }
        if (auto* p = std::get_if<PasteMsg>(&msg)) {
            PasteEvent pe; pe.content = p->text; m.ed.handle_paste(pe); m.dirty = true;
        }
        return {std::move(m), Cmd<Msg>::none()};
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

    static Sub<Msg> subscribe(const Model&) {
        return Sub<Msg>::batch(
            Sub<Msg>::on_key([](const KeyEvent& k) -> std::optional<Msg> {
                if (k.mods.ctrl && std::holds_alternative<CharKey>(k.key) &&
                    std::get<CharKey>(k.key).codepoint == 'q')
                    return Quit{};
                return KeyMsg{k};
            }),
            Sub<Msg>::on_paste([](std::string s) -> Msg { return PasteMsg{std::move(s)}; })
        );
    }
};

int main() { run<App>({.title = "maya — live editor"}); }
