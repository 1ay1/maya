// examples/editor_widgets3.cpp — third showcase browser (decorations, lenses,
// status readouts, doc rendering). Keeps the other browsers small.
//
//   cmake --build build --target maya_editor_widgets3 && ./build/maya_editor_widgets3
//
//   ← / → (h/l)  switch      ↑ / ↓ (k/j)  interact      q  quit

#include <maya/maya.hpp>
#include <maya/widget/code_lens.hpp>
#include <maya/widget/snippet_preview.hpp>
#include <maya/widget/progress_ring.hpp>
#include <maya/widget/notification_badge.hpp>
#include <maya/widget/status_items.hpp>
#include <maya/widget/word_count.hpp>
#include <maya/widget/comment_block.hpp>
#include <maya/widget/git_lens_inline.hpp>
#include <maya/widget/folded_region.hpp>
#include <maya/widget/change_bar.hpp>
#include <maya/widget/sash.hpp>
#include <maya/widget/drop_indicator.hpp>
#include <maya/widget/option_picker.hpp>
#include <maya/widget/selection_info.hpp>
#include <maya/widget/ruler_guide.hpp>
#include <maya/widget/checkbox_list.hpp>
#include <maya/widget/tag_input.hpp>
#include <maya/widget/color_picker_grid.hpp>
#include <maya/widget/hunk_controls.hpp>
#include <maya/widget/keybinding_capture.hpp>
#include <maya/widget/blame_heatmap.hpp>
#include <maya/widget/tree_filter_bar.hpp>
#include <maya/widget/indent_scope_guides.hpp>

#include <array>
#include <string>

using namespace maya;
using namespace maya::dsl;

static constexpr std::array<const char*, 23> kNames = {
    "CodeLens", "SnippetPreview", "ProgressRing", "NotificationBadge",
    "StatusItems", "WordCount", "CommentBlock", "GitLensInline",
    "FoldedRegion", "ChangeBar", "Sash", "DropIndicator",
    "OptionPicker", "SelectionInfo", "RulerGuide", "CheckboxList",
    "TagInput", "ColorPickerGrid", "HunkControls", "KeybindingCapture",
    "BlameHeatmap", "TreeFilterBar", "IndentScopeGuides",
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
        case 0: return v(
            text("char Rope::at(std::size_t i) const") | fgc(Color::hex(0xCBA6F7)),
            CodeLens{}.info("3 references").info("2 implementations")
                      .action("Run Test").action("Debug"));
        case 1: { SnippetPreview s;
            s.lit("for (int ").stop(1, "i").lit(" = 0; ").stop(1, "i").lit(" < ")
             .stop(2, "n").lit("; ++").stop(1, "i").lit(") {\n    ").stop(0, "")
             .lit("\n}").active(1 + (k % 2));
            return s; }
        case 2: return v(
            ProgressRing{}.value(0.10f + 0.09f * (k % 10)).label("Downloading clangd"),
            ProgressRing{}.value(1.0f).label("Indexing"),
            ProgressRing{}.frame(k).label("Cloning repository")
          ) | gap(1);
        case 3: return v(
            NotificationBadge{}.icon("\xef\x83\xb3").label("Notifications").count(k % 9),
            NotificationBadge{}.icon("\xef\x81\x88").label("Problems").count(3),
            NotificationBadge{}.icon("\xef\x84\x87").label("Source Control").count(12)
          ) | gap(1);
        case 4: return StatusItems{}
            .item("\xef\x81\x88", "0", Color::hex(0xF38BA8))
            .item("\xef\x81\xb1", std::to_string(k % 5), Color::hex(0xE2B341))
            .item("", "UTF-8").item("", "LF").item("", "Spaces: 4")
            .item("\xef\x84\x81", "main", Color::hex(0xA6E3A1))
            .item("\xef\x84\xa1", "C++", Color::hex(0x89B4FA));
        case 5: return WordCount::of(
            "The quick brown fox jumps over the lazy dog. "
            "Pack my box with five dozen liquor jugs.\n"
            "How vexingly quick daft zebras jump!\n");
        case 6: { CommentBlock c;
            c.line("/// @brief  Random access into the rope by flat index.")
             .line("///")
             .line("/// @param  i       0-based character index")
             .line("/// @tparam T       the element type")
             .line("/// @return the character stored at `i`, O(log n)")
             .line("/// @note   throws `std::out_of_range` if `i >= size()`");
            return c; }
        case 7: return v(
            GitLensInline{"    return text[i - weight];"}
                .blame("Ada Lovelace", "3 days ago", "fix rope leaf access"),
            GitLensInline{"    if (i < weight && left)"}
                .blame("You", "2 min ago", "Uncommitted changes")
          );
        case 8: return FoldedRegion{"struct Rope {"}.hidden(14 + (k % 8));
        case 9: { ChangeBar b;
            b.line(Change::None).line(Change::Added).line(Change::Added)
             .line(Change::Modified).line(Change::None).line(Change::Deleted)
             .line(Change::Modified).line(Change::None);
            return h(b | width(1),
                     text("  \n  \n  \n  \n  \n  \n  \n  ") | fgc(Color::hex(0x585B70))); }
        case 10: return h(
            text("left pane") | fgc(Color::hex(0x9399B2)) | width(14),
            Sash{}.vertical().active(k % 2) | height(6),
            text("  right pane") | fgc(Color::hex(0x9399B2)) | grow(1)
          );
        case 11: return DropIndicator{}.label("Move to New Editor Group");
        case 12: { OptionPicker p;
            p.option("LF", "Line Feed (\\n) \xe2\x80\x94 Unix, macOS")
             .option("CRLF", "Carriage Return + Line Feed (\\r\\n) \xe2\x80\x94 Windows")
             .option("CR", "Carriage Return (\\r) \xe2\x80\x94 classic Mac")
             .select(k % 3).focus(k % 3);
            return p | width(56); }
        case 13: return v(
            SelectionInfo{}.at(42, 7),
            SelectionInfo{}.at(42, 7).selection(1, 12),
            SelectionInfo{}.at(58, 1).selection(4, 213)
          ) | gap(1);
        default: { RulerGuide r{40};
            r.line("int add(int a, int b) { return a + b; }")
             .line("auto really_long_identifier = some_function_call(with, several, arguments);")
             .line("short();");
            return r; }
        case 15: { CheckboxList c;
            c.item("Format on Save", (k % 2), "editor.formatOnSave")
             .item("Trim Trailing Whitespace", true, "files")
             .item("Insert Final Newline", true)
             .item("Detect Indentation", false, "editor")
             .focus(k % 4);
            return c | width(52); }
        case 16: { TagInput t;
            t.tag("bug").tag("ui").tag("good-first-issue").input("perf");
            return t; }
        case 17: { ColorPickerGrid g;
            for (uint32_t h : {0xF38BA8u,0xFAB387u,0xF9E2AFu,0xA6E3A1u,0x94E2D5u,0x89B4FAu,
                               0xCBA6F7u,0xF5C2E7u,0xEBA0ACu,0xF2CDCDu,0x89DCEBu,0x74C7ECu,
                               0xB4BEFEu,0xA6ADC8u,0x585B70u,0x11111Bu})
                g.color(h);
            g.columns(8).select(k % 16);
            return g; }
        case 18: return v(
            HunkControls{"@@ -10,6 +10,7 @@ char Rope::at"}.staged(false),
            HunkControls{"@@ -40,3 +41,3 @@ Rope::concat"}.staged(true)
          ) | gap(1);
        case 19: return KeybindingCapture{}
            .recording((k % 2) == 0)
            .keys((k % 2) == 0 ? std::vector<std::string>{} 
                               : std::vector<std::string>{"Ctrl","K","Ctrl","S"});
        case 20: { BlameHeatmap b;
            int ages[] = {0, 1, 4, 20, 45, 120, 300, 800, 2000};
            for (int a : ages) b.line(a);
            return h(b | width(1),
                     text(" recent \xe2\x86\x92 old") | fgc(Color::hex(0x585B70))); }
        case 21: { TreeFilterBar f;
            f.query = "rope"; f.matches = 3; f.total = 40;
            f.case_sensitive = (k % 2); f.fuzzy = ((k / 2) % 2);
            return f; }
        case 22: { IndentScopeGuides g;
            g.line("void render(int n) {", 0)
             .line("    for (int i = 0; i < n; ++i) {", 1)
             .line("        if (visible(i)) {", 2)
             .line("            draw(i);", 3)
             .line("        }", 2)
             .line("    }", 1)
             .line("}", 0)
             .active(3, 2, 4);
            return g; }
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

int main() { run<Show>({.title = "maya editor widgets III"}); }
