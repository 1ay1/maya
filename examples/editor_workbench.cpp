// examples/editor_workbench.cpp — a full IDE shell assembled from the layout
// widgets (Workbench, ActivityBar, SplitView) + the content widgets.
//
//   cmake --build build --target maya_editor_workbench && ./build/maya_editor_workbench
//
// Keys: Tab switches the focused split pane, p toggles the bottom panel,
//       b toggles the sidebar, ↑/↓ move the file-tree selection, q quits.

#include <maya/maya.hpp>
#include <maya/widget/workbench.hpp>
#include <maya/widget/activity_rail.hpp>
#include <maya/widget/split_view.hpp>
#include <maya/widget/code_view.hpp>
#include <maya/widget/file_tree.hpp>
#include <maya/widget/editor_tab_bar.hpp>
#include <maya/widget/breadcrumb_bar.hpp>
#include <maya/widget/editor_status_line.hpp>
#include <maya/widget/problems_panel.hpp>
#include <maya/widget/overview_ruler.hpp>

#include <string>

using namespace maya;
using namespace maya::dsl;

static const std::string kA =
    "#pragma once\n#include <string>\n\n"
    "struct Rope {\n"
    "    std::string text;\n"
    "    Rope* left = nullptr;\n"
    "    Rope* right = nullptr;\n"
    "    std::size_t weight = 0;\n"
    "    char at(std::size_t i) const;\n"
    "};\n";
static const std::string kB =
    "#include \"rope.hpp\"\n\n"
    "char Rope::at(std::size_t i) const {\n"
    "    if (i < weight && left)\n"
    "        return left->at(i);\n"
    "    return text[i - weight];\n"
    "}\n";

struct App {
    struct Model { int pane = 0; int sel = 1; bool panel = true; bool side = true; };
    struct TabP{}; struct TogPanel{}; struct TogSide{}; struct Up{}; struct Down{}; struct Quit{};
    using Msg = std::variant<TabP, TogPanel, TogSide, Up, Down, Quit>;

    static Model init() { return {}; }

    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        if (std::holds_alternative<Quit>(msg))     return {m, Cmd<Msg>::quit()};
        if (std::holds_alternative<TabP>(msg))     m.pane ^= 1;
        if (std::holds_alternative<TogPanel>(msg)) m.panel = !m.panel;
        if (std::holds_alternative<TogSide>(msg))  m.side = !m.side;
        if (std::holds_alternative<Up>(msg))       m.sel = std::max(0, m.sel - 1);
        if (std::holds_alternative<Down>(msg))     m.sel = std::min(6, m.sel + 1);
        return {m, Cmd<Msg>::none()};
    }

    static Element view(const Model& m) {
        // activity rail
        ActivityBar rail;
        rail.item("\xef\x81\xbb")                //  explorer
            .item("\xef\x80\x82")                //  search
            .item("\xef\x92\x93", 3)             //  source control (3 changes)
            .item("\xef\x86\x88")                //  run/debug
            .item("\xef\x84\xa6")                //  extensions
            .bottom("\xef\x80\x93")              //  settings
            .bottom("\xef\x80\x87")              //  account
            .active(0);

        // sidebar: file tree
        FileTree ft;
        ft.root("rope");
        ft.folder("include", 0);
          ft.file("rope.hpp", 1).git(GitState::Modified);
        ft.folder("src", 0);
          ft.file("rope.cpp", 1).diag(1, 0);
          ft.file("editor.cpp", 1).git(GitState::Added);
        ft.folder("tests", 0, false).count(3);
        ft.file("CMakeLists.txt", 0);
        ft.file("README.md", 0);
        ft.active(m.sel);
        auto sidebar = v(
            text(" EXPLORER") | fgc(Color::hex(0x585B70)) | Bold,
            ft | grow(1)
        );

        // editor group: two code panes + breadcrumb + tabs on top
        EditorTabBar tabs;
        tabs.tab({.name = "rope.hpp", .modified = true}).tab({.name = "rope.cpp"}).active(m.pane);
        Breadcrumb bc;
        bc.crumb(SymKind::Folder, "src").crumb(SymKind::File, "rope.cpp")
          .crumb(SymKind::Struct, "Rope").crumb(SymKind::Method, "at");

        CodeView cvA{kA, {.lang = syntax::Lang::Cpp}}; cvA.set_active_line(m.pane == 0 ? 9 : -1);
        CodeView cvB{kB, {.lang = syntax::Lang::Cpp}}; cvB.set_active_line(m.pane == 1 ? 5 : -1);
        cvB.mark(5, LineMark::Modified);

        SplitView split{SplitView::Row};
        split.pane(cvA, "rope.hpp").pane(cvB, "rope.cpp").focus(m.pane);

        auto editor = v(bc, split | grow(1)) | grow(1);

        // bottom panel: problems
        ProblemsPanel prob;
        prob.file("rope.cpp", true)
              .problem(Severity::Warning, "unused parameter 'i'", 3, 26)
            .file("editor.cpp", true)
              .problem(Severity::Error, "expected ';' after return statement", 42, 5, "E0001");
        auto panel = v(
            text(" PROBLEMS   OUTPUT   TERMINAL") | fgc(Color::hex(0x585B70)),
            prob | grow(1)
        );

        // status bar
        EditorStatusLine sl;
        sl.left(EditorStatusLine::mode("NORMAL"))
          .left(EditorStatusLine::branch("main"))
          .left(EditorStatusLine::file(m.pane ? "rope.cpp" : "rope.hpp", m.pane == 0))
          .right(EditorStatusLine::info("Tab: pane  b: sidebar  p: panel"))
          .right(EditorStatusLine::lang("C++"))
          .right(EditorStatusLine::pos(m.pane ? 5 : 9, 1, 40));

        Workbench wb;
        wb.titlebar(tabs).activity(rail);
        if (m.side) wb.sidebar(sidebar, 30);
        wb.editor(editor);
        if (m.panel) wb.panel(panel, 9);
        wb.statusbar(sl);

        return wb | grow(1);
    }

    static Sub<Msg> subscribe(const Model&) {
        return key_map<Msg>({
            {SpecialKey::Tab, TabP{}},
            {'p', TogPanel{}}, {'b', TogSide{}},
            {SpecialKey::Up, Up{}}, {'k', Up{}},
            {SpecialKey::Down, Down{}}, {'j', Down{}},
            {'q', Quit{}}, {SpecialKey::Escape, Quit{}},
        });
    }
};

int main() { run<App>({.title = "maya editor — workbench"}); }
