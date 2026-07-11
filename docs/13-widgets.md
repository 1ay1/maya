# Widget Reference

Widgets are runtime components that satisfy the `Node` concept — each provides
a `build()` method returning an `Element`, plus an implicit `operator Element()`
conversion. Interactive widgets integrate with the focus system and accept
`KeyEvent` via a `handle()` method. All types live in `namespace maya` and
headers are under `include/maya/widget/`.

## Using Widgets

Widgets work in both API styles. In simple `run()`, use them directly in the render lambda:

```cpp
Input<> prompt;
prompt.set_placeholder("Type here...");

run({.title = "app"}, event_fn, [&] {
    return v(prompt, t<"[Enter] submit"> | Dim) | pad<1>;
});
```

In Program apps, widgets are typically stored in the Model or as globals, and rendered in `view()`:

```cpp
static Element view(const Model& m) {
    return v(m.prompt, t<"[Enter] submit"> | Dim) | pad<1>;
}
```

Interactive widgets need their `handle(KeyEvent)` method called from the event handler (simple run) or from `subscribe()`/`update()` (Program).

---

## Input Widgets

### Input

Single-line text input with cursor, editing, history, and password masking.

**Header:** `widget/input.hpp`

```cpp
struct InputConfig {
    bool multiline  = false;
    bool history    = true;
    int  max_length = 0;   // 0 = unlimited
    bool password   = false;
};

template <InputConfig Cfg = InputConfig{}>
class Input;
```

**Key methods:** `on_submit(F)`, `on_change(F)`, `set_placeholder(sv)`,
`set_value(sv)`, `clear()`, `handle(KeyEvent)`, `handle_paste(PasteEvent)`,
`value()`, `cursor()`, `focus_node()`.

```cpp
Input<> prompt;
prompt.set_placeholder("Type a message...");
prompt.on_submit([](std::string_view text) { process(text); });
auto ui = dsl::h(text("> ") | Dim, prompt);
```

---

### TextArea

Multi-line text editor with line numbers, scrolling, and vertical cursor
movement.

**Header:** `widget/textarea.hpp`

```cpp
struct TextAreaConfig {
    bool line_numbers    = true;
    bool show_line_count = true;
    int  visible_lines   = 0;   // 0 = show all
    int  max_length      = 0;
};

class TextArea;
// TextArea()
// TextArea(TextAreaConfig cfg)
```

**Key methods:** `on_change(F)`, `on_submit(F)`, `set_placeholder(sv)`,
`set_value(sv)`, `clear()`, `handle(KeyEvent)`, `handle_paste(PasteEvent)`,
`value()`, `cursor()`, `focus_node()`.

```cpp
TextArea editor(TextAreaConfig{.visible_lines = 10});
editor.on_change([](std::string_view text) { save(text); });
auto ui = editor.build();
```

---

### Checkbox / ToggleSwitch

Toggle checkbox (`[x]/[ ]`) and sliding toggle switch.

**Header:** `widget/checkbox.hpp`

```cpp
class Checkbox;
// Checkbox()
// Checkbox(std::string label, bool initial = false)

class ToggleSwitch;
// ToggleSwitch()
// ToggleSwitch(std::string label, bool initial = false)
```

**Key methods (both):** `on_change(F)`, `handle(KeyEvent)`, `focus_node()`.
Checkbox: `checked()`, `set_checked(bool)`.
ToggleSwitch: `on()`, `set_on(bool)`.

```cpp
Checkbox cb("Enable dark mode");
cb.on_change([](bool checked) { update_theme(checked); });

ToggleSwitch ts("Notifications", true);
ts.on_change([](bool on) { toggle_notifs(on); });
```

---

### Radio

Radio button group with single selection. Arrow-key navigable.

**Header:** `widget/radio.hpp`

```cpp
struct RadioConfig {
    std::string selected_indicator;    // default "● "
    std::string unselected_indicator;  // default "○ "
    Style selected_style, unselected_style;
    int visible_count = 0;
};

class Radio;
// Radio(std::vector<std::string> items, RadioConfig cfg = {})
// Radio(std::initializer_list<std::string_view> items, RadioConfig cfg = {})
```

**Key methods:** `on_change(F)` where `F(int idx, std::string_view label)`,
`selected()`, `selected_label()`, `set_selected(int)`, `set_items(...)`,
`handle(KeyEvent)`, `focus_node()`.

```cpp
Radio theme({"Light mode", "Dark mode", "System default"});
theme.on_change([](int idx, std::string_view label) {
    apply_theme(label);
});
auto ui = dsl::v(text("Theme:") | Bold, theme);
```

---

### Button

Clickable button with variant styles: Default, Primary, Danger, Ghost.

**Header:** `widget/button.hpp`

```cpp
enum class ButtonVariant : uint8_t { Default, Primary, Danger, Ghost };

class Button;
// Button()
// Button(std::string label, std::move_only_function<void()> on_click = {},
//        ButtonVariant variant = ButtonVariant::Default)
```

**Key methods:** `on_click(F)`, `set_label(sv)`, `set_variant(v)`,
`handle(KeyEvent)`, `focus_node()`.

```cpp
Button submit("Submit", [] { save(); }, ButtonVariant::Primary);
Button cancel("Cancel", [] { close(); }, ButtonVariant::Ghost);
auto ui = dsl::h(submit, text(" "), cancel);
```

---

### Select

Interactive selection menu with arrow-key navigation and mouse support.

**Header:** `widget/select.hpp`

```cpp
struct SelectConfig {
    std::string indicator;        // default "❯ "
    std::string inactive_prefix;  // default "  "
    Style active_style, inactive_style;
    int visible_count = 0;
};

class Select;
// Select(std::vector<std::string> items, SelectConfig cfg = {})
// Select(std::initializer_list<std::string_view> items, SelectConfig cfg = {})
```

**Key methods:** `on_select(F)` where `F(int, std::string_view)`,
`selected_label()`, `set_items(...)`, `handle(KeyEvent)`,
`handle_mouse(MouseEvent, int render_y_start)`, `focus_node()`.

```cpp
Select menu({"Option A", "Option B", "Option C"});
menu.on_select([](int idx, std::string_view label) {
    handle_selection(idx);
});
```

---

### Slider

Numeric range slider with visual track. Left/Right or h/l to adjust.

**Header:** `widget/slider.hpp`

```cpp
struct SliderConfig {
    float min = 0.0f, max = 1.0f, step = 0.01f;
    int   width = 0;  // 0 = fill available width
    Color fill_color, track_color;
    bool  show_percent = true;
};

class Slider;
// Slider()
// Slider(std::string label, SliderConfig cfg = {})
```

**Key methods:** `on_change(F)`, `set_value(float)`, `set_label(sv)`,
`value()`, `handle(KeyEvent)`, `focus_node()`.

```cpp
Slider volume("Volume", {.min = 0, .max = 100, .step = 5});
volume.on_change([](float v) { set_volume(v); });
auto ui = volume.build();
```

---

## Data Display

### Table

The data table: configurable columns, alignment, striping, optional
bordered card — plus selection, keyboard navigation, height-aware
windowing, a scrollbar, sort indicators, flexible columns, and RICH
cells (styled spans, width-adaptive builders). The rockbottom process
list — tree rails, inline CPU meter, culprit marker and all — is a
config away (see `examples/proc_table.cpp`).

**Responsive (width).** ONE column plan (`maya::solve_columns`) is solved
from the width the table is actually given and feeds the header, the
separator, and every row — they can never drift apart or shear mid-cell.
Columns shed lowest-`keep` first when narrow (first column never, rest
rightmost-first by default). A column with `weight > 0` is FLEXIBLE: it
absorbs surplus width (capped by `max_width` if set — an inline meter
breathes `min_width → max_width`) and shrinks toward `min_width` when
tight — cells truncate with `…`.

**Responsive (height).** The natural height is all rows — nothing windows
in an auto-height context. In a definite slot that is SHORTER (or with
`visible_rows` set) the body windows around the cursor and a `▐` scrollbar
gutter appears. `tbl.build() | grow(1)` fills a pane and the row count
falls out of the layout.

**Rich cells.** A row is a `TableRow` of `TableCell`s. A cell is plain
text, text + `StyledRun` spans (multi-color content inside ONE cell —
tree rails, a dim argv trail after a bright name), or DYNAMIC
(`TableCell::dyn([](int w){ … })`) — built at the column's SOLVED width
every frame (inline meters, sparklines). Truncation clips the spans with
the text, so a rich cell can never shear the columns to its right. Rows
carry identity too: `TableRow::style` merges UNDER every span (semantic
row tints), `edge` + `edge_color` paint a gutter marker (» culprit)
whenever the cursor bar isn't on that row. Plain
`add_row({"a", "b"})` / `set_rows(vector<vector<string>>)` still work.

**Interactive.** `selectable = true` adds the cursor (▎ bar +
`selected_style` + optional `selected_bg` full-band strip) and keyboard
nav — `↑↓`/`j`/`k`, `PgUp`/`PgDn`, `Home`/`End`/`g`/`G`, `Enter` →
`on_activate`. Set `row_hit_kind` / `header_hit_kind` and every row /
header cell registers a `hit_id(kind, index)` rect at paint — resolve
clicks with `maya::hit_test`, zero coordinate math. `sort_col` +
`sort_desc` render the `▾`/`▴` arrow and accent on the sorted header (the
host owns the actual sort).

**Host-driven.** A TEA host that owns its own state machine can take over
every interactive default: `window_top` pins the scroll window (sticky
scroll-margins live in YOUR model), `show_header = false` swaps the header
for host chrome (a kill-confirm strip) without changing the row budget,
per-column `header_style` paints your own ink tiers, and per-column
`hit_index` remaps header clicks onto YOUR enum (two columns can share one
sort key; `kNoHeaderHit` opts a column out).

**Header:** `widget/table.hpp`

```cpp
enum class ColumnAlign : uint8_t { Left, Center, Right };

struct TableCell {
    std::string text;
    std::vector<StyledRun> runs;                 // spans over text
    std::function<TableCell(int)> dynamic;       // solved width → cell
    TableCell& span(std::string_view s, Style st = {});  // chainable
    static TableCell dyn(F&& fn);                // width-adaptive cell
};

struct TableRow {
    std::vector<TableCell> cells;
    Style style;                  // merges UNDER every span
    std::string edge;             // gutter marker when not selected (»)
    std::optional<Color> edge_color;
};

struct ColumnDef {
    std::string header;
    int width = 0;        // fixed; 0 = auto-fit
    ColumnAlign align = ColumnAlign::Left;
    int keep = 0;         // shed order when narrow: LOWER first;
                          // kKeepAlways never; 0 = auto
    float weight = 0.0f;  // > 0 = flexible (absorbs surplus, shrinks to…
    int min_width = 0;    // …this floor; cells truncate with …)
    int max_width = 0;    // growth cap for a weighted column; 0 = ∞
    std::optional<Style> header_style;  // per-column ink override
    int hit_index = -1;   // header hit payload; kNoHeaderHit = no click
};

struct TableConfig {
    Style header_style, row_style, alt_row_style, separator_style;
    bool stripe_rows = true;
    int cell_padding = 1;
    int column_gap = 0;               // extra cells between columns
    bool show_border = false;
    std::string title;
    Color border_color;

    bool selectable = false;          // cursor + keyboard nav
    Style selected_style;             // bold by default
    Color cursor_bar_color;           // the ▎ edge bar
    std::optional<Color> selected_bg; // full-band strip on the cursor row

    int sort_col = -1;  bool sort_desc = true;   // ▾/▴ indicator
    Style sort_header_style;

    bool show_header = true;          // off = host renders its own strip
    bool show_separator = true;       // the ── rule under the header
    std::optional<Color> header_bg;   // full-band header rail tint
    std::string empty_text;           // dim placeholder when rows empty

    int visible_rows = 0;             // fixed window; 0 = fill the slot
    bool show_scrollbar = true;       // gutter appears when windowed
    bool show_status = false;         // "cursor/total" footer
    int window_top = -1;              // host scroll top; -1 = auto-center
    Color scrollbar_thumb_color, scrollbar_track_color;

    uint32_t row_hit_kind = 0;        // hit_id(kind, row) per row
    uint32_t header_hit_kind = 0;     // hit_id(kind, col.hit_index) per header
};

class Table;
// Table(std::vector<ColumnDef> columns)
// Table(std::vector<ColumnDef> columns, TableConfig cfg)
```

**Key methods:** `add_row(TableRow)` (accepts `{"a", "b"}`),
`set_rows(vector<TableRow>)`, `set_rows(vector<vector<string>>)`,
`clear_rows()`, `row_count()`, `set_title(sv)`, `set_bordered(bool)`,
`set_sort(col, desc)`, `selected()`, `set_selected(idx)`,
`handle(KeyEvent)`, `on_change(fn)`, `on_activate(fn)`, `focus_node()`,
`config()`.

```cpp
// Classic static card — unchanged:
Table tbl({{"Name", 20}, {"Status", 10}});
tbl.add_row({"main.cpp", "modified"});
auto ui = tbl.build();

// The proc list (rich cells + host-owned scroll):
TableConfig cfg;
cfg.selectable = true;  cfg.sort_col = 3;  cfg.sort_desc = true;
cfg.selected_bg = Color::hex(0x33354A);  cfg.window_top = m.scroll_top;
cfg.row_hit_kind = HK_ProcRow;  cfg.header_hit_kind = HK_SortCol;
Table procs({{.header = "PID", .align = ColumnAlign::Right},
             {.header = "NAME", .weight = 3.0f, .min_width = 10,
              .hit_index = static_cast<int>(SortKey::Name)},
             {.header = "", .keep = 1, .weight = 1.0f,     // inline meter
              .min_width = 6, .max_width = 14, .hit_index = kNoHeaderHit},
             {.header = "CPU", .align = ColumnAlign::Right,
              .hit_index = static_cast<int>(SortKey::Cpu)}}, cfg);
TableRow row;
row.cells.emplace_back("937", pid_style);
row.cells.push_back(TableCell{}.span("├─ ", rail).span("rb", name)
                               .span("  ./build/rb", dim_trail));
row.cells.push_back(TableCell::dyn([f](int w) { return meter(f, w); }));
row.cells.emplace_back("42.5", load_style);
row.edge = "»";  row.edge_color = Color::red();   // culprit marker
procs.add_row(std::move(row));
procs.set_selected(m.cursor);
auto pane = procs.build() | grow(1);   // rows = whatever fits
```

---

### List

Scrollable selectable list with icon, label, description, and optional
filtering via `/` key. Width-aware: a row sheds its DESCRIPTION (whole,
fit_row-style) when the measured line wouldn't fit the slot — the label
never shears.

**Header:** `widget/list.hpp`

```cpp
struct ListItem {
    std::string label, description, icon;
};

struct ListConfig {
    std::string indicator;  // default "▸ "
    Style active_style, inactive_style, desc_style, filter_style;
    int visible_count = 10;
    bool filterable = false;
};

class List;
// List(std::vector<ListItem> items, ListConfig cfg = {})
```

**Key methods:** `on_select(F)` where `F(int, std::string_view)`,
`selected_label()`, `set_items(...)`, `set_filter(sv)`, `is_filtering()`,
`handle(KeyEvent)`, `focus_node()`.

```cpp
List menu({
    {"Add new item", "Create a new entry", "+"},
    {"Remove item", "Delete selected entry", "-"},
});
menu.on_select([](int idx, std::string_view label) { /* ... */ });
```

---

### Tree

Hierarchical tree view with expand/collapse and keyboard navigation.

**Header:** `widget/tree.hpp`

```cpp
struct TreeNode {
    std::string label;
    std::vector<TreeNode> children;
    bool expanded = false;
    bool selected = false;
};

struct TreeConfig {
    std::string expanded_icon;   // "▾ "
    std::string collapsed_icon;  // "▸ "
    int indent_width = 2;
    Style active_style, branch_style, leaf_style, count_style;
};

class Tree;
// Tree(TreeNode root, TreeConfig cfg = {})
```

**Key methods:** `on_select(F)` where `F(std::string_view)`, `root()`,
`cursor()`, `handle(KeyEvent)`, `focus_node()`.

```cpp
TreeNode root{"src", {
    {"widget", {{"list.hpp"}, {"tree.hpp"}}},
    {"core", {{"types.hpp"}, {"signal.hpp"}}},
    {"main.cpp"},
}};
Tree tree(std::move(root));
tree.on_select([](std::string_view label) { open_file(label); });
```

---

### Sparkline

Inline mini chart using Unicode block characters (single-line).

**Header:** `widget/sparkline.hpp`

```cpp
struct SparklineConfig {
    Color color;
    Style label_style, value_style;
    bool show_min_max = false;
    bool show_last    = false;
};

class Sparkline;
// Sparkline(std::vector<float> data, SparklineConfig cfg = {})
```

**Key methods:** `set_data(...)`, `set_label(sv)`, `set_min(float)`,
`set_max(float)`, `set_color(Color)`, `set_show_min_max(bool)`,
`set_show_last(bool)`.

```cpp
Sparkline spark({0.1f, 0.3f, 0.5f, 0.8f, 1.0f, 0.6f});
spark.set_label("CPU");
spark.set_show_min_max(true);
auto ui = spark.build();
```

---

## Navigation

### Tabs

Tabbed content switcher. Left/Right, h/l, and 1-9 number key navigation.
Responsive via `pick()` (ViewThatFits): the full label row renders while its
measured width fits; a slot too narrow for every label falls back to
`‹ active-label  i/n ›` — always readable, never sheared.

**Header:** `widget/tabs.hpp`

```cpp
class Tabs;
// Tabs(std::vector<std::string> labels)
// Tabs(std::initializer_list<std::string_view> labels)
```

**Key methods:** `on_change(F)` where `F(int)`, `active()`, `set_active(int)`,
`add_tab(string)`, `handle(KeyEvent)`, `focus_node()`.

```cpp
Tabs tabs({"General", "Editor", "Keybindings"});
tabs.on_change([](int idx) { switch_content(idx); });
auto ui = dsl::v(tabs, content);
```

---

### Breadcrumb

Passive path/context breadcrumb trail. Last segment is highlighted.
Responsive via `pick()` (ViewThatFits): the full trail renders while its
measured width fits; a narrow slot collapses the middle to `first › … ›
last`; the tightest slot shows just the current segment.

**Header:** `widget/breadcrumb.hpp`

```cpp
class Breadcrumb;
// Breadcrumb(std::vector<std::string> segments)
// Breadcrumb(std::initializer_list<std::string_view> segments)
```

**Key methods:** `set_segments(...)`, `push(string)`, `pop()`, `size()`.

```cpp
Breadcrumb bc({"src", "widget", "breadcrumb.hpp"});
auto ui = dsl::v(bc, content);
```

---

### Menu

Vertical menu with keybind hints, separators, and disabled items.
Width-aware: a row sheds its SHORTCUT hint when label + shortcut would
overflow the measured slot — the label never shears.

**Header:** `widget/menu.hpp`

```cpp
struct MenuItem {
    std::string label, shortcut;
    bool enabled = true;
    bool separator = false;
};

class Menu;
// Menu(std::vector<MenuItem> items)
```

**Key methods:** `on_select(F)` where `F(int)`, `set_items(...)`,
`handle(KeyEvent)`, `focus_node()`.

```cpp
Menu menu({
    {.label = "New File",  .shortcut = "Ctrl+N"},
    {.label = "Open File", .shortcut = "Ctrl+O"},
    {.separator = true},
    {.label = "Quit",      .shortcut = "Ctrl+Q"},
});
menu.on_select([](int idx) { handle_menu(idx); });
```

---

### CommandPalette

Fuzzy-search command launcher with embedded Input and highlighted match results.
Width-aware: rows shed their DESCRIPTION first, then the SHORTCUT, when the
measured line would overflow; the whole palette is clamped to `max_width`
(default 72 — `set_max_width(0)` for full slot) and centered on wider slots.

**Header:** `widget/command_palette.hpp`

```cpp
struct Command {
    std::string name, description, shortcut;
};

class CommandPalette;
// CommandPalette(std::vector<Command> commands)
```

**Key methods:** `show()`, `hide()`, `toggle()`, `visible()`,
`on_execute(F)` where `F(int)`, `handle(KeyEvent)`,
`handle_paste(PasteEvent)`, `focus_node()`.

```cpp
CommandPalette palette({
    {.name = "Open File", .description = "Browse files", .shortcut = "Ctrl+O"},
    {.name = "Toggle Theme", .description = "Switch dark/light"},
});
palette.on_execute([](int idx) { run_command(idx); });
palette.show();
```

---

## Display

### Badge

Inline bracketed label/chip with preset color themes.

**Header:** `widget/badge.hpp`

```cpp
struct Badge {
    std::string label;
    Config config;  // style, left_cap, right_cap, bracket_style

    Badge(std::string label);
    Badge(std::string label, Config cfg);

    static Badge success(std::string label);
    static Badge error(std::string label);
    static Badge warning(std::string label);
    static Badge info(std::string label);
    static Badge tool(std::string label);  // purple
};
```

```cpp
auto ui = dsl::h(
    Badge::success("ok"),
    Badge::error("fail"),
    Badge::info("v2.1")
);
```

---

### Callout

Severity-based alert/notification with icon, title, and optional description
body with left border.

**Header:** `widget/callout.hpp`

```cpp
enum class Severity : uint8_t { Info, Success, Warning, Error };

class Callout;
// Callout(Config cfg)          — Config{severity, title, description}
// Callout(Severity, string title, string desc = "")

static Callout info(string title, string desc = "");
static Callout success(string title, string desc = "");
static Callout warning(string title, string desc = "");
static Callout error(string title, string desc = "");
```

```cpp
auto err = Callout::error("Failed to read file", "permission denied");
auto info = Callout::info("3 files modified");
auto ui = dsl::v(err, info);
```

---

### ToastManager

Notification toast stack with timed auto-dismissal and severity styling.
Cards are clamped to `Config::max_width` (default 60) and pinned to the
right edge — the toast convention — instead of stretching into full-width
banners on a wide terminal.

**Header:** `widget/toast.hpp`

```cpp
enum class ToastLevel : uint8_t { Info, Success, Warning, Error };

struct ToastManager {
    struct Config {
        float duration = 3.0f, fade_time = 0.5f;
        int max_visible = 3;
    };
    // ToastManager()
    // ToastManager(Config cfg)
};
```

**Key methods:** `push(string message, ToastLevel level)`, `advance(float dt)`,
`empty()`.

```cpp
ToastManager toasts;
toasts.push("Response complete", ToastLevel::Success);
toasts.advance(dt);
auto ui = toasts.build();
```

---

### FileRef

File path reference display with dimmed directory, underlined filename, and
optional line number. Width-aware: when the full `dir/name:line` chip can't
fit the measured slot the directory collapses to a leading `…/` — the
filename survives whole.

**Header:** `widget/file_ref.hpp`

```cpp
struct FileRef {
    std::string path;
    int line = 0;

    FileRef(std::string path, int line = 0);
    Element build() const;
    Element build(Config cfg) const;  // Config: path_style, name_style, lineno_style, icon
};
```

```cpp
FileRef ref("src/render/canvas.cpp", 42);
auto ui = ref.build();
```

---

### UserMessage / AssistantMessage

Chat message containers. User messages get a bordered box; assistant messages
get clean padding.

**Header:** `widget/message.hpp`

```cpp
struct UserMessage {
    static Element build(std::string_view content);
    static Element build(Element content);
};

struct AssistantMessage {
    static Element build(Element content);
};
```

```cpp
auto user_msg = UserMessage::build("show me the project structure");
auto asst_msg = AssistantMessage::build(markdown_element);
```

---

### ProgressBar

Progress bar with label, percentage, sub-character precision, and elapsed time.

**Header:** `widget/progress.hpp`

```cpp
struct ProgressConfig {
    int width = 0;  // 0 = fill available
    Color fill_color, bg_color;
    bool show_track = true, show_percentage = true;
};

class ProgressBar;
// ProgressBar()
// ProgressBar(ProgressConfig cfg)
```

**Key methods:** `set(float v)`, `set_elapsed(float seconds)`,
`set_label(sv)`, `value()`.

```cpp
ProgressBar bar;
bar.set_label("Indexing files...");
bar.set(0.68f);
bar.set_elapsed(2.3f);
auto ui = bar.build();
```

---

### ActivityBar

Bottom status/activity bar displaying model, tokens, cost, context usage, and
custom sections.

**Header:** `widget/activity_bar.hpp`

```cpp
struct ActivityBar {
    struct Config { /* separator_style, label_style, value_style, accent_style */ };
    // ActivityBar()
    // ActivityBar(Config cfg)
};
```

**Key methods:** `set_model(sv)`, `set_tokens(int in, int out)`,
`set_cost(float usd)`, `set_context_percent(int)`, `set_status(sv)`,
`add_section(string icon, string label, Style)`, `clear_sections()`.

```cpp
ActivityBar bar;
bar.set_model("claude-sonnet-4");
bar.set_tokens(1200, 3400);
bar.set_cost(0.03f);
bar.set_context_percent(45);
auto ui = bar.build();
```

---

### Divider

Horizontal separator with optional label. Fills available width.

**Header:** `widget/divider.hpp`

```cpp
struct DividerConfig {
    BorderStyle line = BorderStyle::Single;
    Style line_style, label_style;
};

class Divider;
// Divider()
// Divider(std::string_view label, DividerConfig cfg = {})
// Divider(DividerConfig cfg)
```

**Key methods:** `set_label(sv)`.

```cpp
auto sep = Divider("Section Title");
auto plain = Divider();
```

---

### Spinner

Animated frame-based spinner with 9 built-in styles. Updates via elapsed time.

**Header:** `widget/spinner.hpp`

```cpp
enum class SpinnerStyle : uint8_t {
    Dots, Line, Arc, Arrow, Bounce, Bar, Clock, Star, Pulse
};

template <SpinnerStyle S = SpinnerStyle::Dots>
class Spinner;
// Spinner()
// Spinner(Style s)
```

**Key methods:** `advance(float dt)`, `set_style(Style)`.

```cpp
Spinner spin;
spin.advance(dt);
auto ui = dsl::h(spin, text(" Loading..."));

Spinner<SpinnerStyle::Braille> s2;
```

---

## Overlay

### Modal

Modal dialog with title, content, and Tab-navigable action buttons. Enter to
activate, Escape to dismiss. Clamped to `set_max_width` (default 80) and
centered beyond — a dialog, not a banner, on an ultrawide.

**Header:** `widget/modal.hpp`

```cpp
struct ModalButton {
    enum Variant : uint8_t { Default, Primary, Danger };
    std::string label;
    Variant variant = Default;
    std::move_only_function<void()> callback;
};

class Modal;
// Modal()
// Modal(std::string title, Element content, std::vector<ModalButton> buttons)
```

**Key methods:** `show()`, `hide()`, `visible()`, `set_content(Element)`,
`set_title(sv)`, `handle(KeyEvent)`, `focus_node()`.

```cpp
Modal modal("Confirm", text("Are you sure?"), {
    {"Cancel", ModalButton::Default, [&]{ modal.hide(); }},
    {"OK",     ModalButton::Primary, [&]{ do_thing(); modal.hide(); }},
});
modal.show();
auto ui = modal.build();
```

---

### Popup

Non-interactive bordered message box with severity-based styling.

**Header:** `widget/popup.hpp`

```cpp
enum class PopupStyle : uint8_t { Info, Warning, Error };

class Popup;
// Popup()
// Popup(std::string content, PopupStyle style = PopupStyle::Info)
```

**Key methods:** `show(string_view msg, PopupStyle)`, `hide()`, `visible()`.

```cpp
Popup tip;
tip.show("File saved successfully", PopupStyle::Info);
auto ui = tip.build();
```

---

### Scrollable

Scrollable container with viewport clipping and scroll indicator.

**Header:** `widget/scrollable.hpp`

```cpp
struct ScrollConfig {
    int height = 10;
    int scroll_amount = 1;
    bool show_indicator = true;
    Color indicator_color, indicator_active;
};

class Scrollable;
// Scrollable(ScrollConfig cfg = {})
```

**Key methods:** `set_content(Element)`, `set_content_height(int)`,
`scroll_up(int n = 0)`, `scroll_down(int n = 0)`, `scroll_to_top()`,
`scroll_to_bottom()`, `offset()`, `height()`, `content_height()`,
`handle(KeyEvent)`, `handle_mouse(MouseEvent)`.

```cpp
Scrollable scroll({.height = 10});
scroll.set_content(my_tall_element);
scroll.set_content_height(50);
auto ui = scroll.build();
```

---

## Visualization

### BarChart

Non-interactive horizontal bar chart with proportional block bars.

**Header:** `widget/bar_chart.hpp`

```cpp
struct Bar {
    std::string label;
    float value = 0.0f;
    std::optional<Color> color;
};

class BarChart;
// BarChart()
// BarChart(std::vector<Bar> bars, float max_value = 0.0f)
```

**Key methods:** `set_bars(...)`, `set_max_value(float)`,
`set_default_color(Color)`.

```cpp
BarChart chart({
    {"CPU",    0.75f},
    {"Memory", 0.45f},
    {"Disk",   0.90f, Color::rgb(224, 108, 117)},
});
auto ui = chart.build();
```

---

### LineChart

Braille-dot line chart with 2x4 sub-cell resolution per terminal cell.

**Header:** `widget/line_chart.hpp`

```cpp
class LineChart;
// LineChart()
// LineChart(std::vector<float> data, int height = 8)
```

**Key methods:** `set_data(...)`, `set_height(int)`, `set_label(sv)`,
`set_color(Color)`.

```cpp
LineChart chart({1.0f, 3.5f, 2.0f, 7.0f, 4.5f, 6.0f});
chart.set_height(8);
chart.set_label("Requests/s");
auto ui = chart.build();
```

---

### Gauge

Arc or bar meter for displaying a 0-1 value. Arc mode renders a bordered box
with percentage; Bar mode renders a vertical block column.

**Header:** `widget/gauge.hpp`

```cpp
enum class GaugeStyle : uint8_t { Arc, Bar };

class Gauge;
// Gauge()
// Gauge(float value, std::string label = {},
//       Color color = ..., GaugeStyle style = GaugeStyle::Arc)
```

**Key methods:** `set_value(float)`, `set_label(sv)`, `set_color(Color)`,
`set_style(GaugeStyle)`, `value()`.

```cpp
Gauge g(0.45f, "CPU");
auto ui = g.build();
```

---

### Heatmap

2D color grid with interpolated cell colors between a low and high color.

**Header:** `widget/heatmap.hpp`

```cpp
class Heatmap;
// Heatmap()
// Heatmap(std::vector<std::vector<float>> data)
```

**Key methods:** `set_data(...)`, `set_x_labels(vector<string>)`,
`set_y_labels(vector<string>)`, `set_low_color(Color)`,
`set_high_color(Color)`.

```cpp
Heatmap hm({
    {0.1f, 0.5f, 0.9f},
    {0.3f, 0.7f, 0.2f},
});
hm.set_x_labels({"Mon", "Tue", "Wed"});
hm.set_y_labels({"AM", "PM"});
auto ui = hm.build();
```

---

### PixelCanvas

Freeform drawing surface using half-block characters for double vertical
resolution. Resolution: `width x (height * 2)` pixels.

**Header:** `widget/canvas.hpp`

```cpp
class PixelCanvas;
// PixelCanvas()
// PixelCanvas(int width, int height)
```

**Key methods:** `set_pixel(int x, int y, Color)`, `clear()`, `fill(Color)`,
`line(x1, y1, x2, y2, Color)`, `rect(x, y, w, h, Color)`,
`width()`, `height()`, `pixel_height()`.

```cpp
PixelCanvas c(40, 10);
c.set_pixel(5, 3, Color::rgb(224, 108, 117));
c.line(0, 0, 39, 19, Color::rgb(97, 175, 239));
auto ui = c.build();
```

---

## Specialized

### DiffView

Unified diff display with colored +/- lines, hunk markers, and optional line
numbers.

**Header:** `widget/diff_view.hpp`

```cpp
struct DiffView {
    struct Config {
        Style add_style, remove_style, context_style, hunk_style, header_style;
        Color border_color;
        bool show_border = true, show_line_numbers = true;
    };

    std::string file_path, diff_text;
    Config config;

    DiffView(std::string path, std::string diff);
    DiffView(std::string path, std::string diff, Config cfg);
};
```

```cpp
auto diff = DiffView("src/main.cpp",
    "@@ -12,4 +12,6 @@\n"
    " void Canvas::begin_frame() {\n"
    "-    damage_ = {0, 0, width_, height_};\n"
    "+    damage_ = {0, 0, 0, 0};\n"
    " }\n");
auto ui = diff.build();
```

---

### PlanView

Task/plan display with status icons (pending, in-progress, completed).

**Header:** `widget/plan_view.hpp`

```cpp
enum class TaskStatus : uint8_t { Pending, InProgress, Completed };

struct PlanView {
    struct Task { std::string label; TaskStatus status; };
    std::vector<Task> tasks;
};
```

**Key methods:** `add(string label, TaskStatus status = Pending)`,
`set_status(size_t index, TaskStatus)`.

```cpp
PlanView plan;
plan.add("Analyze codebase", TaskStatus::InProgress);
plan.add("Read configuration files", TaskStatus::Completed);
plan.add("Write implementation");
auto ui = plan.build();
```

---

### LogViewer

Streaming log viewer with scroll, auto-scroll, and level filtering.

**Header:** `widget/log_viewer.hpp`

```cpp
enum class LogLevel : uint8_t { Debug, Info, Warn, Error };
struct LogEntry { std::string timestamp, message; LogLevel level; };

class LogViewer;
// LogViewer()
// LogViewer(int visible, int max_entries = 1000)
```

**Key methods:** `push(LogEntry)`, `clear()`, `set_visible(int)`,
`set_max_entries(int)`, `entry_count()`, `auto_scroll()`,
`handle(KeyEvent)`, `focus_node()`.

```cpp
LogViewer viewer(20);
viewer.push({.timestamp = "12:00:01", .message = "Started", .level = LogLevel::Info});
viewer.push({.timestamp = "12:00:02", .message = "Cache miss", .level = LogLevel::Warn});
auto ui = viewer.build();
```

---

### Calendar

Interactive month calendar grid with arrow-key day selection and today
highlighting.

**Header:** `widget/calendar.hpp`

```cpp
class Calendar;
// Calendar(int year, int month)
// Calendar(int year, int month, int today_y, int today_m, int today_d)
```

**Key methods:** `on_select(F)` where `F(int y, int m, int d)`,
`next_month()`, `prev_month()`, `set_today(y, m, d)`, `selected_day()`,
`year()`, `month()`, `handle(KeyEvent)`, `focus_node()`.

```cpp
Calendar cal(2026, 4, 2026, 4, 9);
cal.on_select([](int y, int m, int d) { set_date(y, m, d); });
auto ui = cal.build();
```

---

### KeyHelp

Keyboard shortcut reference panel. Responsive via `pick()` (ViewThatFits):
the two-column arrangement renders when its REAL measured width fits the
slot, otherwise the single column does — no byte-count estimates, no
breakpoints.

**Header:** `widget/key_help.hpp`

```cpp
struct KeyBinding { std::string key, description, group; };

class KeyHelp;
// KeyHelp()
// KeyHelp(std::vector<KeyBinding> bindings)
```

**Key methods:** `add(string key, string description, string group = "")`,
`set_title(sv)`, `clear()`.

```cpp
KeyHelp help;
help.add("Up/Down", "Move cursor", "Navigation");
help.add("Enter", "Select item", "Navigation");
help.add("Ctrl+S", "Save file", "Editing");
auto ui = help.build();
```

---

### Disclosure

Chevron-based collapse/expand control with optional content body.

**Header:** `widget/disclosure.hpp`

```cpp
struct Disclosure {
    struct Config {
        std::string label;
        Style label_style, icon_style;
        std::string open_icon, closed_icon;  // default "▼", "▶"
    };

    // Disclosure(Config cfg)
};
```

**Key methods:** `is_open()`, `set_open(bool)`, `toggle()`, `build()` (header
only), `build(Element content)` (header + content when open).

```cpp
Disclosure d({.label = "Advanced Settings"});
d.set_open(true);
auto ui = d.build(settings_content);
```

---

### ThinkingBlock

Collapsible thinking/reasoning block with spinner and gutter-bordered content.

**Header:** `widget/thinking.hpp`

```cpp
class ThinkingBlock;
// ThinkingBlock()
```

**Key methods:** `set_active(bool)`, `set_expanded(bool)`, `toggle()`,
`set_content(sv)`, `append(sv)`, `advance(float dt)`,
`set_max_visible_lines(int)`, `is_active()`, `is_expanded()`.

```cpp
ThinkingBlock thinking;
thinking.set_active(true);
thinking.set_content("Analyzing the request...\nReading project structure...");
thinking.advance(dt);
auto ui = thinking.build();
```

---

## Tool Widgets

These widgets render tool execution cards in a Claude Code / Zed agent style:
bordered boxes with status icons, elapsed time, and collapsible content.

### ToolCall

Generic collapsible tool execution card.

**Header:** `widget/tool_call.hpp`

```cpp
enum class ToolCallStatus : uint8_t { Pending, Running, Completed, Failed, Confirmation };
enum class ToolCallKind : uint8_t {
    Read, Edit, Execute, Search, Delete, Move, Fetch, Think, Agent, Other
};

class ToolCall;
// ToolCall(Config cfg)   — Config{tool_name, kind, description}
```

**Key methods:** `set_status(ToolCallStatus)`, `set_elapsed(float)`,
`set_expanded(bool)`, `toggle()`, `set_content(Element)`, `status()`.

```cpp
ToolCall tc({.tool_name = "Read", .kind = ToolCallKind::Read,
             .description = "src/main.cpp"});
tc.set_status(ToolCallStatus::Completed);
tc.set_elapsed(0.3f);
auto ui = tc.build();
```

---

### BashTool

Command execution card with `$ command` display, output, and exit code.

**Header:** `widget/bash_tool.hpp`

```cpp
enum class BashStatus : uint8_t { Pending, Running, Success, Failed };

class BashTool;
// BashTool()
// BashTool(std::string command)
```

**Key methods:** `set_command(sv)`, `set_output(sv)`, `append_output(sv)`,
`set_status(BashStatus)`, `set_elapsed(float)`, `set_exit_code(int)`,
`set_expanded(bool)`, `toggle()`, `set_max_output_lines(int)`.

```cpp
BashTool bash("npm install --save-dev typescript");
bash.set_status(BashStatus::Success);
bash.set_elapsed(2.3f);
bash.set_output("added 1 package in 2.3s");
bash.set_expanded(true);
auto ui = bash.build();
```

---

### EditTool

File edit operation card with search/replace diff display.

**Header:** `widget/edit_tool.hpp`

```cpp
enum class EditStatus : uint8_t { Pending, Applying, Applied, Failed };

class EditTool;
// EditTool()
// EditTool(std::string path)
```

**Key methods:** `set_file_path(sv)`, `set_old_text(sv)`, `set_new_text(sv)`,
`set_status(EditStatus)`, `set_elapsed(float)`, `set_expanded(bool)`,
`toggle()`.

```cpp
EditTool edit("src/render/canvas.cpp");
edit.set_old_text("damage_ = {0, 0, width_, height_};");
edit.set_new_text("damage_ = {0, 0, 0, 0};");
edit.set_status(EditStatus::Applied);
edit.set_expanded(true);
auto ui = edit.build();
```

---

### WriteTool

File creation/write card with content preview.

**Header:** `widget/write_tool.hpp`

```cpp
enum class WriteStatus : uint8_t { Pending, Writing, Written, Failed };

class WriteTool;
// WriteTool()
// WriteTool(std::string path)
```

**Key methods:** `set_file_path(sv)`, `set_content(sv)`,
`set_status(WriteStatus)`, `set_elapsed(float)`, `set_expanded(bool)`,
`toggle()`, `set_max_preview_lines(int)`.

```cpp
WriteTool wt("src/utils/helpers.ts");
wt.set_content("export function debounce(...) { ... }");
wt.set_status(WriteStatus::Written);
wt.set_expanded(true);
auto ui = wt.build();
```

---

### ReadTool

File content preview card with line numbers and truncation.

**Header:** `widget/read_tool.hpp`

```cpp
enum class ReadStatus : uint8_t { Pending, Reading, Success, Failed };

class ReadTool;
// ReadTool()
// ReadTool(std::string path)
```

**Key methods:** `set_file_path(sv)`, `set_content(sv)`,
`set_status(ReadStatus)`, `set_elapsed(float)`, `set_expanded(bool)`,
`toggle()`, `set_start_line(int)`, `set_max_lines(int)`,
`set_total_lines(int)`.

```cpp
ReadTool rt("src/main.cpp");
rt.set_content("#include <iostream>\nint main() { return 0; }");
rt.set_status(ReadStatus::Success);
rt.set_elapsed(0.2f);
rt.set_expanded(true);
auto ui = rt.build();
```

---

### FetchTool

Web fetch result card with URL, HTTP status, content type, and body preview.

**Header:** `widget/fetch_tool.hpp`

```cpp
enum class FetchStatus : uint8_t { Pending, Fetching, Done, Failed };

class FetchTool;
// FetchTool()
// FetchTool(std::string url)
```

**Key methods:** `set_url(sv)`, `set_body(sv)`, `set_content_type(sv)`,
`set_status_code(int)`, `set_status(FetchStatus)`, `set_elapsed(float)`,
`set_expanded(bool)`, `toggle()`, `set_max_body_lines(int)`.

```cpp
FetchTool ft("https://api.example.com/v1");
ft.set_status(FetchStatus::Done);
ft.set_status_code(200);
ft.set_content_type("application/json");
ft.set_elapsed(1.2f);
ft.set_body("{\"data\": [...]}");
ft.set_expanded(true);
auto ui = ft.build();
```

---

### SearchResult

Search results display (Grep/Glob) with grouped file matches. Width-aware:
long file paths truncate at the START (`…/dir/file.cpp` keeps the filename)
instead of the tail clipping off.

**Header:** `widget/search_result.hpp`

```cpp
enum class SearchKind : uint8_t { Grep, Glob };
enum class SearchStatus : uint8_t { Pending, Searching, Done, Failed };

struct SearchMatch { int line; std::string content; };
struct SearchFileGroup { std::string file_path; std::vector<SearchMatch> matches; };

class SearchResult;
// SearchResult()
// SearchResult(SearchKind kind, std::string pattern = "")
```

**Key methods:** `set_pattern(sv)`, `set_status(SearchStatus)`,
`set_elapsed(float)`, `set_expanded(bool)`, `toggle()`, `clear()`,
`add_group(SearchFileGroup)`, `total_matches()`, `file_count()`,
`set_max_matches_per_file(int)`.

```cpp
SearchResult sr(SearchKind::Grep, "TODO");
sr.add_group({"src/main.cpp", {{12, "// TODO: fix this"}, {45, "// TODO: refactor"}}});
sr.set_status(SearchStatus::Done);
sr.set_elapsed(0.4f);
sr.set_expanded(true);
auto ui = sr.build();
```

---

### AgentTool

Sub-agent card with nested tool calls, spinner, and model badge.

**Header:** `widget/agent_tool.hpp`

```cpp
enum class AgentStatus : uint8_t { Pending, Running, Completed, Failed };

class AgentTool;
// AgentTool()
// AgentTool(std::string description)
```

**Key methods:** `set_description(sv)`, `set_model(sv)`,
`set_status(AgentStatus)`, `set_elapsed(float)`, `set_expanded(bool)`,
`toggle()`, `advance(float dt)`, `add_tool(Element)`, `clear_tools()`.

```cpp
AgentTool agent("Exploring codebase structure");
agent.set_model("claude-sonnet-4");
agent.set_status(AgentStatus::Running);
agent.advance(dt);
agent.add_tool(read_tool.build());
agent.add_tool(grep_tool.build());
auto ui = agent.build();
```

---

### Permission

Tool permission prompt card with key hints for allow/deny.

**Header:** `widget/permission.hpp`

```cpp
enum class PermissionResult : uint8_t { Pending, Allow, AllowAlways, Deny };

class Permission;
// Permission(Config cfg)   — Config{tool_name, description, show_always_allow}
// Permission(std::string tool_name, std::string description)
```

**Key methods:** `result()`, `set_result(PermissionResult)`.

```cpp
Permission perm("bash", "rm -rf node_modules && npm install");
auto ui = perm.build();
// renders: [y] allow  [n] deny  key hints
```

---

## Status Bar Widgets

The streaming-status family renders the single-line status bar an agent shows
while a turn is in flight: a leading breadcrumb / phase chip on the left, the
live token-rate spark in the middle, and the model badge + context gauge on the
right. `StatusBar` composes them; the pieces are also usable standalone.

### StatusBar

Three-row status bar: a top/bottom `PhaseAccent` rail sandwiching a
hard-locked single-line activity row. The row is a MEASURED degradation
ladder — the real styled fragments are built at progressively leaner shapes
and the richest one whose measured width fits renders. Shed order: ctx token
counts · token-stream chip · phase elapsed · ctx bar · phase verb · ctx
gauge · model badge; the phase chip — *what's happening now* — is the last
survivor, and the breadcrumb is added last from the measured leftover
(capped at its config `max_chars`). There are NO width-threshold knobs to
tune — the old `*_min_width` Config fields are gone. When
`status_banner.text` is set the middle row becomes a full-width toast instead.

**Header:** `widget/status_bar.hpp`

```cpp
StatusBar::Config cfg;
cfg.phase_color = Color::cyan();
cfg.breadcrumb  = { .title = "maya › docs" };
cfg.phase       = { /* verb, elapsed, breathing */ };
cfg.token_stream = { .rate = 23.4f, .history = hist, .live = true,
                     .adaptive = true };
cfg.model_badge = ModelBadge("claude-opus-4").build();
cfg.context     = { .used = 42000, .max = 200000 };
auto ui = StatusBar{cfg}.build();
```

### TokenStreamSparkline

Compact `⚡ rate t/s ▁▂▃▄▅▆▇█ ` chip for the status bar: a fixed-width tok/s
rate, a right-pinned block-glyph sparkline of the rate history, coloured by
rate (red / yellow / green). Every segment is a fixed display width so ticking
numbers never shove the neighbouring chips leftward. When `live` is false the
rate + spark dim, signalling “frozen at last sample” during tool execution.

**Header:** `widget/token_stream_sparkline.hpp`

```cpp
struct Config {
    float              rate    = 0.0f;   // current tok/s
    int                total   = 0;
    std::vector<float> history;          // rate samples, newest last
    Color              color   = Color::cyan();
    bool               live    = false;  // false = dim (frozen)
    bool               adaptive        = false;
    int                max_spark_cells = 64;
};
```

```cpp
maya::TokenStreamSparkline{{
    .rate    = 23.4f,
    .history = rate_history,
    .color   = Color::cyan(),
    .live    = is_streaming,
}}.build();
```

**Adaptive width.** With `.adaptive = true` the widget returns a `grow(1)`
component that defers the spark-cell count to paint time and sizes the
sparkline to *fill* its allocated slot — the status bar hands it the dead space
between the phase chip and the model badge. The count is clamped to
`[8, max_spark_cells]` and right-pinned so the newest sample stays glued to the
chips on its right. With `.adaptive = false` (default) the layout is the legacy
fixed 16-cell chip, byte-identical to before.
