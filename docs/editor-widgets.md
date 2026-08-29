# Editor widgets

A toolkit of **30 foreground-only widgets** for building a terminal code editor
on maya, plus one interactive buffer (`TextEditor`) and three layout composers.
Every widget is an independent header under `include/maya/widget/`, uses the
terminal's own background (no hard-coded fills — only intentional soft *shades*
on active rows and reverse-video caret notches), and converts to an `Element`
via `operator Element()` or `.build()`.

## Examples

| Target | What it shows |
|--------|---------------|
| `maya_editor_widgets` | One-widget-at-a-time browser (`←/→` switch, `↑/↓` interact) |
| `maya_editor_workbench` | Interactive IDE shell (activity bar · sidebar · split editor · panel · status) |
| `maya_editor_ide` | Static composed workspace |
| `maya_editor_live` | **Working editor** — type to edit, undo, select, copy/paste |
| `maya_editor_widget_check` | Lifetime/render smoke test (renders every widget as a temporary) |

```sh
cmake --build build --target maya_editor_widgets && ./build/maya_editor_widgets
```

## Lifetime rule (important)

A widget's deferred `ComponentElement.render` lambda **must not capture `this`**
unless the widget is guaranteed to outlive the frame. Display widgets are often
built as temporaries (`return FileTree{...} | width(30);`), so their render
lambdas capture **values** (or `[self = *this]`). `TextEditor` keeps mutable
scroll state in a `shared_ptr` and snapshots the buffer by value. The
`maya_editor_widget_check` target renders each widget as a temporary at widths
1–200 to catch regressions (run it under ASAN in CI).

---

## Shell / layout

- **`Workbench`** (`workbench.hpp`) — composes title bar · activity rail ·
  sidebar · editor group · bottom panel · status bar with dividers. Any region
  left unset is omitted (show/hide = don't call the setter).
- **`ActivityBar`** (`activity_rail.hpp`) — slim vertical icon rail; active
  accent bar, count badges, bottom-pinned items.
- **`SplitView`** (`split_view.hpp`) — `Row`/`Col` editor panes with dividers,
  titled headers, focus accent, weighted sizing.

## Chrome

- **`EditorTabBar`** (`editor_tab_bar.hpp`) — file tabs: per-language icon
  colors, modified/diagnostic/close badges, pinned + preview, overflow chevrons,
  full-width active-underline separator.
- **`Breadcrumb`** (`breadcrumb_bar.hpp`) — `folder › file › Class › method`
  with per-kind glyphs/colors (shared `sym_kind.hpp`).
- **`EditorStatusLine`** (`editor_status_line.hpp`) — powerline-ish status with
  thin dividers; presets `mode/branch/file/lang/pos/info`.
- **`FileTree`** (`file_tree.hpp`) — real tree connectors (`├─└─│`), filename +
  extension icons, right-aligned status column (diagnostics + git), symlinks,
  hidden dimming, extension-preserving truncation, reveal-active, filter-match
  highlight, marked files.
- **`SymbolOutline`** (`symbol_outline.hpp`) — nested symbol tree with kind
  glyphs, tree guides, detail text, active pointer.
- **`OverviewRuler`** (`overview_ruler.hpp`) — right-edge scrollbar with
  viewport thumb + colored diagnostic/search/change ticks + cursor.
- **`MarkerGutter`** (`marker_gutter.hpp`) — breakpoints/bookmarks/execution
  column, fixed-width to align with `CodeView`.
- **`PanelTabs`** (`panel_tabs.hpp`) — bottom-panel header (PROBLEMS/OUTPUT/…)
  with count badges + active dot.
- **`Toolbar`** (`toolbar.hpp`) — icon-button strip: labels, toggles, disabled,
  group separators.

## Code surface

- **`CodeView`** (`code_view.hpp`) — read-only syntax-highlighted pane: gutter
  (absolute/relative), git ribbon, indent guides, current-line shade, caret
  (reverse-video notch), selection underline, tab expansion. Powered by
  `maya::syntax`.
- **`TextEditor`** (`text_editor.hpp`) — **interactive** buffer rendered through
  `CodeView`: cursor, shift-selection, undo/redo, clipboard, viewport scroll.
  `handle(KeyEvent)` / `handle_paste(PasteEvent)`; store it in your Model.
- **`Minimap`** (`minimap.hpp`) — 2× density code map (▀▄█), per-line syntax
  hue, viewport slider, dim-outside-viewport, active glow.
- **`GitBlameGutter`** (`git_blame_gutter.hpp`) — inline author + relative time
  + short sha per line, fixed-width; "You, uncommitted" accent.
- **`DiffHunkView`** (`diff_hunk_view.hpp`) — unified diff: old/new gutters,
  `+`/`-` markers, add/del/context colors, `@@` headers.

## Panels

- **`ProblemsPanel`** (`problems_panel.hpp`) — diagnostics grouped by file with
  count badges; items show severity glyph, message, code, right-aligned Ln:Col.
- **`SearchResults`** (`search_results.hpp`) — matches grouped by file, each hit
  as `before / MATCH / after` with the match highlighted; tree guides.
- **`TerminalPane`** (`terminal_pane.hpp`) — integrated terminal: `cwd ❯ cmd`
  prompts, colored stdout/stderr/info/ok, live input with block caret.
- **`DiffStat`** (`diff_stat.hpp`) — `git diff --stat`: per-file `+N -M` + `▰▱`
  bar, name column truncates.

## Overlays / popups

- **`CompletionMenu`** (`completion_menu.hpp`) — LSP autocomplete: kind glyph,
  fuzzy-highlighted label, right-aligned type, selected-row shade, doc line,
  scroll hints.
- **`SignatureHelp`** (`signature_help.hpp`) — call signature with active
  parameter highlighted, overload counter, doc.
- **`HoverCard`** (`hover_card.hpp`) — signature header + full-width divider +
  wrapped doc + footnotes.
- **`CommandPalette`** (`command_palette_bar.hpp`) — prompt + fuzzy-matched
  command list with keybinding chips.
- **`FindReplaceBar`** (`find_replace_bar.hpp`) — query/replace fields, match
  count, case/word/regex toggles.
- **`ContextMenu`** (`context_menu.hpp`) — icon + label + shortcut/submenu,
  separators, disabled, checks, active shade.
- **`DiagnosticLens`** (`diagnostic_lens.hpp`) — squiggle + `╰─` connector +
  severity message inline under a code line.
- **`NotificationStack`** (`notification_stack.hpp`) — toast cards by kind with
  optional progress bar.
- **`QuickInput`** (`quick_input.hpp`) — Rename/Go-to-Line modal input; red on
  error.
- **`WhichKeyMenu`** (`which_key_menu.hpp`) — leader-key `key → action` grid;
  group entries marked `+`.
- **`DebugToolbar`** (`debug_toolbar.hpp`) — run/step/stop controls with
  state-aware enabling.
- **`PeekView`** (`peek_view.hpp`) — titled inline frame wrapping any body
  Element (e.g. a `CodeView` snippet).

## Primitives

- **`FuzzyLine`** (`fuzzy_line.hpp`) — fuzzy subsequence matcher +
  match-highlighted row. Reused by CompletionMenu / CommandPalette / FileTree.
- **`sym_kind.hpp`** — shared `SymKind` enum + `sym_glyph()` / `sym_color()`.

---

## Minimal live-editor sketch

```cpp
struct App {
    struct Model { TextEditor ed{{.lang = syntax::Lang::Cpp}}; };
    struct KeyMsg { KeyEvent ev; }; struct Quit {};
    using Msg = std::variant<KeyMsg, Quit>;

    static Model init() { Model m; m.ed.set_text(src); return m; }

    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        if (std::holds_alternative<Quit>(msg)) return {std::move(m), Cmd<Msg>::quit()};
        if (auto* k = std::get_if<KeyMsg>(&msg)) (void)m.ed.handle(k->ev);
        return {std::move(m), Cmd<Msg>::none()};
    }

    static Element view(const Model& m) { return m.ed | dsl::grow(); }

    static Sub<Msg> subscribe(const Model&) {
        return Sub<Msg>::on_key([](const KeyEvent& k) -> std::optional<Msg> {
            if (k.mods.ctrl && std::holds_alternative<CharKey>(k.key) &&
                std::get<CharKey>(k.key).codepoint == 'q') return Quit{};
            return KeyMsg{k};
        });
    }
};
```
