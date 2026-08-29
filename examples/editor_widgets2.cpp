// examples/editor_widgets2.cpp — second widget showcase browser (git / debug /
// panels / decorations). Keeps the large editor_widgets.cpp untouched.
//
//   cmake --build build --target maya_editor_widgets2 && ./build/maya_editor_widgets2
//
//   ← / → (h/l)  switch widget      ↑ / ↓ (k/j)  interact      q  quit

#include <maya/maya.hpp>
#include <maya/widget/key_caps.hpp>
#include <maya/widget/color_swatch.hpp>
#include <maya/widget/empty_state.hpp>
#include <maya/widget/segmented_control.hpp>
#include <maya/widget/status_progress.hpp>
#include <maya/widget/commit_graph.hpp>
#include <maya/widget/merge_conflict.hpp>
#include <maya/widget/staging_view.hpp>
#include <maya/widget/branch_picker.hpp>
#include <maya/widget/quick_pick.hpp>
#include <maya/widget/call_hierarchy.hpp>
#include <maya/widget/test_explorer.hpp>
#include <maya/widget/output_channel.hpp>
#include <maya/widget/variables_tree.hpp>
#include <maya/widget/call_stack.hpp>
#include <maya/widget/history_timeline.hpp>
#include <maya/widget/settings_editor.hpp>
#include <maya/widget/extensions_list.hpp>
#include <maya/widget/tooltip.hpp>
#include <maya/widget/multi_cursor_strip.hpp>
#include <maya/widget/zen_layout.hpp>
#include <maya/widget/grid_split.hpp>
#include <maya/widget/inlay_hint_line.hpp>
#include <maya/widget/watch_panel.hpp>
#include <maya/widget/code_view.hpp>

#include <array>
#include <string>

using namespace maya;
using namespace maya::dsl;

static constexpr std::array<const char*, 24> kNames = {
    "CommitGraph", "StagingView", "BranchPicker", "MergeConflict",
    "QuickPick", "CallHierarchy", "TestExplorer", "OutputChannel",
    "VariablesTree", "CallStack", "HistoryTimeline", "SegmentedControl",
    "StatusProgress", "ColorSwatch", "KeyCaps", "EmptyState",
    "SettingsEditor", "ExtensionsList", "Tooltip", "MultiCursorStrip",
    "ZenLayout", "GridSplit", "InlayHintLine", "WatchPanel",
};

struct Show {
    struct Model { int page = 0; int k = 0; };
    struct Prev{}; struct Next{}; struct Up{}; struct Down{}; struct Quit{};
    using Msg = std::variant<Prev, Next, Up, Down, Quit>;
    static constexpr int N = static_cast<int>(kNames.size());

    static Model init() { return {}; }
    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        if (std::holds_alternative<Quit>(msg)) return {m, Cmd<Msg>::quit()};
        if (std::holds_alternative<Prev>(msg)) { m.page = (m.page + N - 1) % N; m.k = 0; }
        if (std::holds_alternative<Next>(msg)) { m.page = (m.page + 1) % N; m.k = 0; }
        if (std::holds_alternative<Up>(msg))   m.k = std::max(0, m.k - 1);
        if (std::holds_alternative<Down>(msg)) m.k += 1;
        return {m, Cmd<Msg>::none()};
    }

    static Element widget(int page, int k) {
        switch (page) {
        case 0: { CommitGraph g;
            g.commit(0, "a1c2f3e", "fix rope index bounds", {"HEAD", "main"})
             .commit(0, "9f0b12d", "add Rope::concat")
             .merge (0, 1, "77aa10c", "merge feature/balance")
             .commit(1, "0b1d2e3", "wip: rebalance")
             .commit(0, "3ea5c19", "initial commit");
            return g; }
        case 1: { StagingView s;
            s.staged("M", "src/rope.cpp").staged("A", "include/rope.hpp")
             .change("M", "README.md").change("D", "old.txt").change("U", "scratch.cpp")
             .active(k % 5);
            return s | width(44); }
        case 2: { BranchPicker b;
            b.branch("main", true, 0, 0, "fix rope index")
             .branch("feature/balance", false, 3, 1, "wip: rebalance")
             .branch("hotfix/oob", false, 1, 0, "clamp index")
             .remote("origin/main", "add concat").active(k % 4);
            return b | width(50); }
        case 3: { MergeConflict c;
            c.ours("HEAD", {"    return left->at(i);"})
             .theirs("feature/balance", {"    return left ? left->at(i) : text[i - weight];"});
            return c; }
        case 4: { QuickPick q; q.query = "rope";
            q.item("src/rope.cpp", "src").item("include/rope.hpp", "include")
             .item("tests/rope_test.cpp", "tests").select(k % 3)
             .preview({"struct Rope {", "  std::string text;", "  Rope* left;", "  char at(size_t) const;", "};"});
            return q | width(72); }
        case 5: { CallHierarchy h{CallHierarchy::Incoming};
            h.root(SymKind::Method, "Rope::at")
             .call(SymKind::Function, "operator[]", 1, "rope.hpp:20")
             .call(SymKind::Method, "Editor::char_at", 2, "editor.cpp:88")
             .call(SymKind::Function, "render_line", 3, "view.cpp:140");
            return h | width(54); }
        case 6: { TestExplorer t;
            t.suite("RopeTest", 2, 3)
               .test(TestStatus::Passed, "concat_joins", 1, "0.4ms")
               .test(TestStatus::Failed, "at_out_of_range", 1, "1.2ms")
               .test(TestStatus::Passed, "weight_correct", 1, "0.3ms")
             .suite("EditorTest", 1, 1)
               .test(TestStatus::Skipped, "huge_file", 1);
            return t | width(50); }
        case 7: { OutputChannel o{"C/C++ \xc2\xb7 clangd"};
            o.info("indexed 1,204 files", "12:04:29")
             .warn("no compile_commands.json found", "12:04:30")
             .error("unresolved include <foo.h>", "12:04:31")
             .debug("semantic tokens: 8,410", "12:04:31");
            return o; }
        case 8: { VariablesTree v;
            v.scope("Locals")
             .var("i", "3", VarKind::Number, 1)
             .var("rope", "Rope * 0x55f0", VarKind::Pointer, 1, true, true)
               .var("weight", "8", VarKind::Number, 2, false, false, k % 2)
               .var("text", "\"hello world\"", VarKind::String, 2)
               .var("left", "nullptr", VarKind::Null, 2)
             .var("balanced", "true", VarKind::Bool, 1);
            return v | width(44); }
        case 9: { CallStack c;
            c.frame("Rope::at", "rope.hpp:12", true)
             .frame("Editor::char_at", "editor.cpp:88")
             .frame("render_line", "view.cpp:140")
             .frame("main", "main.cpp:20")
             .frame("__libc_start_main", "libc.so.6", false, true);
            return c | width(50); }
        case 10: { HistoryTimeline t;
            t.event(TimelineKind::Commit, "fix rope index bounds", "2h ago", "a1c2f3e")
             .event(TimelineKind::Branch, "switched to feature/balance", "2h ago")
             .event(TimelineKind::Save, "File Saved", "3h ago")
             .event(TimelineKind::Edit, "42 edits", "3h ago")
             .event(TimelineKind::Commit, "add Rope::concat", "1d ago", "9f0b12d");
            return t | width(50); }
        case 11: { SegmentedControl s;
            s.seg("Code").seg("Blame").seg("Preview").seg("Diff").active(k % 4);
            return s; }
        case 12: { return v(
            (StatusProgress{}.label("Indexing").value(0.10f + 0.08f * (k % 10))),
            (StatusProgress{}.label("Cloning ").indeterminate(k)),
            (StatusProgress{}.label("Building").value(1.0f))
          ) | gap(1); }
        case 13: { ColorSwatch cs;
            cs.color("mauve", 0xCBA6F7).color("green", 0xA6E3A1).color("peach", 0xFAB387)
              .color("blue", 0x89B4FA).color("red", 0xF38BA8).color("yellow", 0xF9E2AF);
            return cs; }
        case 14: { return v(
            KeyCaps{"Ctrl", "Shift", "P"},
            KeyCaps{"Ctrl", "K"}.key("Ctrl").key("S"),
            KeyCaps{"F12"}
          ) | gap(1); }
        case 15: { EmptyState e;
            e.glyph("\xef\x81\xbb").title("No Folder Opened")
             .hint("Open a folder to start browsing and editing files.")
             .action("Ctrl+O", "Open Folder").action("Ctrl+N", "New File")
             .action("Ctrl+Shift+P", "Command Palette");
            return e | grow(1); }
        case 16: { SettingsEditor s; s.query = "editor";
            s.toggle("editor.wordWrap", "Word Wrap", "Controls how lines wrap.", (k % 2))
             .number("editor.fontSize", "Font Size", "Editor font size in pixels.", "14")
             .choice("workbench.colorTheme", "Color Theme", "", "Catppuccin Mocha")
             .toggle("editor.minimap.enabled", "Minimap", "Show the code minimap.", true)
             .active(k % 4);
            return s | width(66); }
        case 17: { ExtensionsList e;
            e.ext("clangd", "12.0", "LLVM", "C/C++ completion, navigation & insights", 4.8f, true, true)
             .ext("Catppuccin", "1.3", "catppuccin", "Soothing pastel theme for the high-spirited", 5.0f, true, false)
             .ext("Vim", "1.29", "vscodevim", "Vim emulation", 4.3f, false, false)
             .active(k % 3);
            return e | width(58); }
        case 18: { return v(
            (Tooltip{"Go to Definition"}.shortcut("F12")),
            (Tooltip{"Rename Symbol"}.shortcut("F2")),
            (Tooltip{"Trigger Suggest"})
          ) | gap(1); }
        case 19: { return v(
            MultiCursorStrip{}.cursors(1 + (k % 5)).selections((k % 3)).selected_chars(12 * (k % 6))
          ); }
        case 20: { ZenLayout z;
            z.max_width(46).content(v(
                text("    The quick brown fox jumps") | fgc(Color::hex(0xBAC2DE)),
                text("    over the lazy dog.") | fgc(Color::hex(0xBAC2DE)),
                text(""),
                text("    \xe2\x80\x94 centered, max 46 cols") | fgc(Color::hex(0x585B70))));
            return z | grow(1); }
        case 21: { GridSplit g;
            auto mk = [](const char* t){ return CodeView{t, {.lang=syntax::Lang::Cpp}} | grow(1); };
            g.pane(0, mk("int a = 1;\n"), "a.cpp")
             .pane(1, mk("int b = 2;\n"), "b.hpp")
             .pane(2, mk("int c = 3;\n"), "c.rs")
             .pane(3, mk("int d = 4;\n"), "d.go")
             .focus(k % 4);
            return g | grow(1); }
        case 22: { InlayHintLine l;
            l.code("auto ").code("sum").hint(": int").code(" = add(")
             .hint("a:").code("1, ").hint("b:").code("2);");
            return v(l,
                InlayHintLine{}.code("for (").code("auto").hint(": char").code(" c : text) {}")
            ) | gap(1); }
        default: { WatchPanel w;
            w.watch("rope->weight", "8", VarKind::Number)
             .watch("rope->text", "\"hello world\"", VarKind::String)
             .watch("rope->left", "0x0", VarKind::Pointer)
             .watch("balanced", "true", VarKind::Bool)
             .error("rope->parent", "no member named 'parent'")
             .active(k % 5);
            return w | width(48); }
        }
    }

    static Element view(const Model& m) {
        std::string title = "\xe2\x97\x86  " + std::string(kNames[m.page]);
        std::string counter = std::to_string(m.page + 1) + " / " + std::to_string(N);
        auto header = h(text(title) | fgc(Color::hex(0xCBA6F7)) | Bold, spacer(),
                        text(counter) | fgc(Color::hex(0x585B70)));
        auto footer = text("\xe2\x86\x90 \xe2\x86\x92 switch    \xe2\x86\x91 \xe2\x86\x93 interact    q quit")
                    | fgc(Color::hex(0x585B70));
        return v(header, text(""), widget(m.page, m.k), spacer(), footer)
             | padding(1, 3, 1, 3) | grow(1);
    }

    static Sub<Msg> subscribe(const Model&) {
        return key_map<Msg>({
            {SpecialKey::Left, Prev{}},  {'h', Prev{}},
            {SpecialKey::Right, Next{}}, {'l', Next{}},
            {SpecialKey::Up, Up{}},      {'k', Up{}},
            {SpecialKey::Down, Down{}},  {'j', Down{}},
            {'q', Quit{}}, {SpecialKey::Escape, Quit{}},
        });
    }
};

int main() { run<Show>({.title = "maya editor widgets II"}); }
