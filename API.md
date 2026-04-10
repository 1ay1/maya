# maya API reference

maya is a C++26 TUI library. `run<P>()` call, zero boilerplate.

```cpp
#include <maya/maya.hpp>          // DSL, events, signals, styles, app lifecycle
#include <maya/widget/input.hpp>  // widgets included individually
using namespace maya;
```

---

## Table of contents

1. [Headers](#headers)
2. [Quick start](#quick-start)
3. [Program concept / run\<P\>()](#program-concept--runp)
4. [Cmd\<Msg\>](#cmdmsg)
5. [Sub\<Msg\>](#submsg)
6. [RunConfig](#runconfig)
7. [Ctx — render context](#ctx)
8. [Event helpers](#event-helpers)
9. [Element DSL](#element-dsl)
10. [Layout](#layout)
11. [Widgets](#widgets)
12. [Signals](#signals)
13. [Style](#style)
14. [Color](#color)
15. [Theme](#theme)
16. [Borders](#borders)
17. [Core types](#core-types)
18. [Error handling](#error-handling)
19. [Concepts](#concepts)
20. [Context system](#context-system)
21. [Advanced — low-level API](#advanced)

---

## Headers

maya separates its public API from internal implementation details. Downstream projects should only depend on the public headers.

### Public API — `<maya/maya.hpp>`

The main header. Includes:
- Compile-time DSL (`v()`, `h()`, `t<>`, `text()`, pipes)
- App lifecycle (`run<P>()`, `print()`, `live()`, `quit()`)
- Event predicates (`key()`, `ctrl()`, `mouse_clicked()`, ...)
- Reactive signals (`Signal`, `Computed`, `Effect`, `Batch`)
- Styling (`Color`, `Style`, `Border`, `Theme`)
- Elements (`Element`, `BoxElement`, `TextElement`)
- Core types (`Size`, `Rect`, `Columns`, `Rows`, `Dimension`)

Does **not** include widgets or rendering internals.

### Widgets — `<maya/widget/*.hpp>`

Each widget has its own header. Include only what you use:

```cpp
#include <maya/widget/markdown.hpp>
#include <maya/widget/input.hpp>
#include <maya/widget/scrollable.hpp>
#include <maya/widget/tool_call.hpp>
```

All 50+ widgets are in `include/maya/widget/`. They satisfy the `Node` concept and work directly in DSL expressions.

### Internal — `<maya/internal.hpp>`

For code that needs direct access to the rendering pipeline, canvas, diff engine, SIMD intrinsics, terminal I/O, or layout engine:

```cpp
#include <maya/internal.hpp>
```

This is used by maya's own canvas-based examples (doom fire, raymarcher, etc.) and is **not part of the stable API**. Internal headers may change across versions.

| Layer | Header | Stable? |
|-------|--------|---------|
| Public API | `<maya/maya.hpp>` | Yes |
| Widgets | `<maya/widget/*.hpp>` | Yes |
| Internals | `<maya/internal.hpp>` | No — may change |

---

## Quick start

```cpp
#include <maya/maya.hpp>
using namespace maya;
using namespace maya::dsl;

struct Counter {
    struct Model { int count = 0; };
    struct Inc {}; struct Dec {}; struct Quit {};
    using Msg = std::variant<Inc, Dec, Quit>;

    static Model init() { return {}; }

    static auto update(Model m, Msg msg) -> std::pair<Model, Cmd<Msg>> {
        return std::visit(overload{
            [&](Inc)  { return std::pair{Model{m.count + 1}, Cmd<Msg>{}}; },
            [&](Dec)  { return std::pair{Model{m.count - 1}, Cmd<Msg>{}}; },
            [](Quit)  { return std::pair{Model{}, Cmd<Msg>::quit()}; },
        }, msg);
    }

    static Element view(const Model& m) {
        auto color = m.count >= 0 ? Color::green() : Color::red();
        return (v(
            text("Count: " + std::to_string(m.count)) | Bold | fgc(color),
            text("[+/-] change  [q] quit") | Dim
        ) | padding(1)).build();
    }

    static auto subscribe(const Model&) -> Sub<Msg> {
        return key_map<Msg>({{'+', Inc{}}, {'-', Dec{}}, {'q', Quit{}}});
    }
};

int main() { run<Counter>({.title = "counter"}); }
```

That's a complete TUI application using the Elm-architecture Program model. The DSL (`v()`, `h()`, pipes) is the primary API — no builders, no boilerplate.

---

## Program concept / run\<P\>()

```cpp
template<Program P>
void run(RunConfig cfg = {});
```

Starts the application. Blocks until the app quits (via `Cmd<Msg>::quit()`). Errors are printed to stderr and the process exits — you never deal with terminal-init failure in application code.

### Program concept

A **Program** is any struct/class that defines these associated types and static functions:

```cpp
struct MyApp {
    // Required types
    struct Model { /* your application state */ };
    using Msg = std::variant</* your message types */>;

    // Required functions
    static Model init();                                           // or: static std::pair<Model, Cmd<Msg>> init();
    static auto update(Model m, Msg msg) -> std::pair<Model, Cmd<Msg>>;
    static Element view(const Model& m);

    // Optional
    static auto subscribe(const Model&) -> Sub<Msg>;              // event subscriptions
};
```

**`init()`** — returns the initial model. Can also return `std::pair<Model, Cmd<Msg>>` to fire commands on startup.

**`update(Model, Msg)`** — pure function. Takes the current model and a message, returns the new model and an optional command. The model is passed by value — mutate and return it.

**`view(const Model&)`** — renders the current model into an `Element` tree. Called every frame.

**`subscribe(const Model&)`** — optional. Returns a `Sub<Msg>` that maps terminal events (keys, mouse, resize, timers) to messages. Re-evaluated when the model changes.

### key_map\<Msg\>()

Convenience for mapping key presses to messages:

```cpp
static auto subscribe(const Model&) -> Sub<Msg> {
    return key_map<Msg>({
        {'+', Inc{}},
        {'-', Dec{}},
        {'q', Quit{}},
    });
}
```

### maya::quit()

```cpp
void quit() noexcept;
```

Schedules a clean exit. For `live()` and `canvas_run()` only. In Program apps, return `Cmd<Msg>::quit()` from `update()` instead.

---

## Cmd\<Msg\>

Commands are side effects returned from `update()`. They run asynchronously and may produce new messages.

```cpp
Cmd<Msg>{}                    // no-op (do nothing)
Cmd<Msg>::quit()              // exit the application
Cmd<Msg>::batch({cmd1, cmd2}) // run multiple commands
Cmd<Msg>::after(100ms, msg)   // deliver msg after a delay
Cmd<Msg>::task([]() -> Msg {  // run async work, return a message
    return SomeMsg{do_io()};
})
Cmd<Msg>::set_title("title")  // set terminal window title
Cmd<Msg>::write_clipboard(s)  // write string to system clipboard
```

---

## Sub\<Msg\>

Subscriptions map terminal events to messages. Returned from `subscribe()`.

```cpp
Sub<Msg>::none()                          // no subscriptions
Sub<Msg>::batch({sub1, sub2})             // combine multiple subscriptions
Sub<Msg>::on_key([](const KeyEvent& ke) -> std::optional<Msg> { ... })
Sub<Msg>::on_mouse([](const MouseEvent& me) -> std::optional<Msg> { ... })
Sub<Msg>::on_resize([](Size sz) -> std::optional<Msg> { ... })
Sub<Msg>::on_paste([](std::string text) -> std::optional<Msg> { ... })
Sub<Msg>::every(1s, [](auto now) -> Msg { return Tick{now}; })  // periodic timer
```

`key_map<Msg>()` is a convenience that builds a `Sub<Msg>::on_key()` subscription from a map of char-to-message pairs.

---

## RunConfig

```cpp
struct RunConfig {
    std::string_view title      = "";          // terminal window title (OSC 0)
    bool             mouse      = false;       // enable mouse event reporting
    Mode             mode       = Mode::Fullscreen; // rendering mode
    Theme            theme      = theme::dark; // colour theme
};
```

C++20 aggregate. Use designated initializers — only set what you need:

```cpp
run<MyApp>({.title = "my app"});
run<MyApp>({.title = "editor", .mouse = true});
run<MyApp>({.theme = theme::light});
run<MyApp>({});                              // all defaults
run<MyApp>();                                // also all defaults
```

**Stability guarantee**: new fields are always added with defaults. Existing call sites never need updating.

---

## Ctx

```cpp
struct Ctx {
    Size  size;   // current terminal dimensions
    Theme theme;  // active colour theme
};
```

In the Program architecture, `view()` takes `const Model&` rather than `Ctx`. Terminal size and theme are available through subscriptions (`Sub<Msg>::on_resize()`) and the runtime. `Ctx` is still used by `canvas_run()` and the low-level API.

```cpp
// Program app — store size in your Model via on_resize subscription
static Element view(const Model& m) {
    bool narrow = m.width < 80;
    return v(
        text("Hello") | fgc(Color::cyan()),
        narrow ? text("narrow") : text("wide layout")
    ).build();
}

// canvas_run / low-level — Ctx is passed directly
[&](const Ctx& ctx) {
    bool narrow = ctx.size.width.value < 80;
    return v(
        text("Hello") | fgc(ctx.theme.primary),
        narrow ? text("narrow") : text("wide layout")
    ).build();
}
```

`Size` fields:
```cpp
ctx.size.width.value   // int — columns
ctx.size.height.value  // int — rows
```

---

## Event helpers

Stable predicate functions over the `Event` variant. For Program apps, prefer `Sub<Msg>::on_key()` and `key_map<Msg>()` instead of manual event handling. These predicates are still useful for `canvas_run()`, `live()`, and inside `Sub<Msg>::on_key()` lambdas.

### Key predicates

```cpp
// Character keys
bool key(const Event& ev, char c);          // key(ev, 'q')
bool key(const Event& ev, char32_t cp);     // key(ev, U'é')

// Special keys
bool key(const Event& ev, SpecialKey sk);   // key(ev, SpecialKey::Up)

// Modifier combos
bool ctrl(const Event& ev, char c);         // ctrl(ev, 'c')  → Ctrl+C
bool alt(const Event& ev, char c);          // alt(ev, 'f')   → Alt+F
bool shift(const Event& ev, SpecialKey sk); // shift(ev, SpecialKey::Tab) → Shift+Tab

// Catch-all
bool any_key(const Event& ev);              // true for any key event
const KeyEvent* as_key(const Event& ev);    // raw access, or nullptr
```

`SpecialKey` values:
```
Up  Down  Left  Right
Home  End  PageUp  PageDown
Tab  BackTab  Backspace  Delete  Insert
Enter  Escape
F1 … F12
```

### Mouse predicates

```cpp
bool mouse_clicked(const Event& ev, MouseButton btn = MouseButton::Left);
bool mouse_released(const Event& ev, MouseButton btn = MouseButton::Left);
bool mouse_moved(const Event& ev);
bool scrolled_up(const Event& ev);
bool scrolled_down(const Event& ev);

std::optional<MousePos> mouse_pos(const Event& ev);  // nullopt for non-mouse events
const MouseEvent* as_mouse(const Event& ev);          // raw access, or nullptr
```

`MousePos`:
```cpp
struct MousePos { int col, row; };  // 0-based terminal cell coordinates
```

`MouseButton` values: `Left`, `Right`, `Middle`, `ScrollUp`, `ScrollDown`, `None`

Mouse requires `.mouse = true` in `RunConfig`. For hover (motion without button), send `\x1b[?1003h` manually after terminal setup (low-level path only; the high-level `run<P>()` enables button-press/release tracking by default).

### Other predicates

```cpp
bool resized(const Event& ev, int* w = nullptr, int* h = nullptr);
bool pasted(const Event& ev, std::string* content = nullptr);
bool focused(const Event& ev);
bool unfocused(const Event& ev);
```

```cpp
// Low-level usage (canvas_run / live)
[&](const Event& ev) {
    int w, h;
    if (resized(ev, &w, &h)) relayout(w, h);

    std::string text;
    if (pasted(ev, &text)) handle_paste(text);

    return true;
}

// Program app equivalent — use subscriptions instead
static auto subscribe(const Model&) -> Sub<Msg> {
    return Sub<Msg>::batch({
        Sub<Msg>::on_resize([](Size sz) -> std::optional<Msg> {
            return Resized{sz.width.value, sz.height.value};
        }),
        Sub<Msg>::on_paste([](std::string text) -> std::optional<Msg> {
            return Pasted{std::move(text)};
        }),
    });
}
```

---

## Element DSL

Elements are immutable value types built with the compile-time DSL. The render function returns one root `Element` per frame — maya diffs it against the previous frame.

```cpp
using namespace maya::dsl;
```

### Text

```cpp
// Compile-time text (zero runtime cost)
t<"Hello World"> | Bold | Fg<255, 100, 80>

// Runtime text (dynamic content, still pipeable)
text("hello") | Bold | fgc(Color::red())
text(42) | Dim                    // numbers auto-format
text(3.14)                        // floats too
```

`TextWrap` pipes:
```cpp
text("long...") | clip     // TruncateEnd
text("long...") | nowrap   // NoWrap
```

### Containers: v() and h()

```cpp
v(child1, child2, child3)   // vertical stack (column)
h(child1, child2, child3)   // horizontal stack (row)
```

Children can be any `Node`, `Element`, or `ElementRange` (like `vector<Element>`):

```cpp
std::vector<Element> rows = { text("a"), text("b") };
v(t<"Header"> | Bold, rows, t<"Footer"> | Dim) | border(Round) | padding(1)
```

### Compile-time pipes

Applied to `v()`/`h()` results. Values must be compile-time constants:

```cpp
v(...) | pad<1>                    // padding (all sides)
v(...) | pad<1, 2, 3, 4>          // top, right, bottom, left
v(...) | gap_<1>                   // gap between children
v(...) | grow_<1>                  // flex grow
v(...) | w_<40>                    // fixed width
v(...) | h_<10>                    // fixed height
v(...) | border_<Round>            // border style
v(...) | border_<Round> | bcol<50,54,62>  // border + color
v(...) | bordered<Round, 0x323746> // border + color shorthand
```

### Runtime pipes

Same `|` syntax but accept runtime values (variables, theme colors, etc.):

```cpp
Color c = theme.border;
v(...) | padding(1)                // padding(all), padding(v,h), padding(t,r,b,l)
v(...) | gap(n)                    // runtime gap
v(...) | grow(1.0f)                // runtime grow
v(...) | width(40) | height(10)    // runtime dimensions
v(...) | border(Round)             // runtime border style
v(...) | bcolor(c)                 // runtime border color
v(...) | btext("Title", Top, Start) // border text label
v(...) | fgc(c) | bgc(c)          // runtime colors
v(...) | margin(1)                 // runtime margin
v(...) | align(Align::Center)      // align items
v(...) | justify(Justify::End)     // justify content
v(...) | overflow(Overflow::Hidden) // overflow behavior
```

### Dynamic content

```cpp
// Runtime escape hatch — lambda returns Element
dyn([&] { return text("count = " + std::to_string(n)); })

// Conditional rendering
when(is_loading, spinner, content)   // if/else
when(show_debug, debug_panel)        // if only (else → blank)

// Range projection
map(items, [](const auto& s) { return text(s) | Bold; })

// Visibility toggle
text("debug info") | visible(debug_mode)
```

### Utility nodes

```cpp
space          // flex-growing spacer (pushes siblings apart)
spacer()       // same, function form
sep            // horizontal separator line
vsep           // vertical separator line
blank_         // empty line
blank()        // same, function form
```

### Hex colors and style presets

```cpp
fg<0xFF4444>                         // hex foreground
bg<0x1A1A2E>                         // hex background
constexpr auto heading = Bold | fg<0xFFFFFF>;   // reusable preset
"Hello"_t | heading                  // UDL syntax
```

### Element introspection

```cpp
bool is_box(const Element& e);
bool is_text(const Element& e);
bool is_list(const Element& e);

BoxElement*  as_box(Element& e);   // nullptr if not a box
TextElement* as_text(Element& e);  // nullptr if not a text
ElementList* as_list(Element& e);  // nullptr if not a list

void visit_element(const Element& e, auto&& visitor);  // depth-first walk
```

---

## Layout

Layout is flexbox-inspired. Use DSL pipes to set properties on containers.

### Direction

```cpp
v(...)   // children stacked top-to-bottom (column)
h(...)   // children side by side left-to-right (row)
```

### Spacing

```cpp
v(...) | pad<1>               // compile-time: 1 cell all sides
v(...) | pad<1, 2>            // compile-time: vertical=1, horizontal=2
v(...) | pad<1, 2, 3, 4>     // compile-time: top, right, bottom, left
v(...) | padding(1, 2, 3, 4) // runtime: same with variables
v(...) | gap_<1>              // compile-time gap between children
v(...) | gap(n)               // runtime gap
v(...) | margin(1)            // runtime margin (same variants as padding)
```

### Sizing

```cpp
v(...) | w_<40>              // compile-time fixed width
v(...) | h_<10>              // compile-time fixed height
v(...) | width(40)           // runtime fixed width
v(...) | height(10)          // runtime fixed height
v(...) | grow_<1>            // compile-time grow factor
v(...) | grow(1.0f)          // runtime grow factor
```

For advanced sizing (percent, min/max), use the `box()` builder:
```cpp
box().width(Dimension::percent(50)).min_width(Dimension::px(20))(...)
```

### Alignment

```cpp
v(...) | align(Align::Center)         // cross-axis alignment
v(...) | justify(Justify::SpaceBetween) // main-axis distribution
```

`Align` values: `Start`, `Center`, `End`, `Stretch`, `Baseline`, `Auto`
`Justify` values: `Start`, `End`, `Center`, `SpaceBetween`, `SpaceAround`, `SpaceEvenly`

### Overflow

```cpp
v(...) | overflow(Overflow::Hidden)  // clip at boundary
```

`Overflow` values: `Visible` (default), `Hidden`, `Scroll`

### Styling a container

```cpp
v(...) | fgc(Color::white())               // foreground color
v(...) | bgc(Color::rgb(30, 30, 40))       // background fill
v(...) | Fg<255,255,255>                    // compile-time fg
v(...) | Bg<30,30,40>                       // compile-time bg
```

### Complete example

```cpp
using namespace maya::dsl;

auto ui = v(
    // Header row: title left, status right
    h(
        text("My App") | Bold,
        space,
        text("READY") | fgc(Color::green())
    ),
    sep,
    // Content area grows to fill remaining height
    h(
        // Sidebar: fixed width
        v(text("Nav item 1"), text("Nav item 2")) | width(20) | padding(1),
        vsep,
        // Main panel: stretches
        v(text("Content here")) | grow(1) | padding(1)
    ) | grow(1),
    sep,
    text("[q] quit") | Dim
) | padding(1) | gap(1);
```

---

## Widgets

maya includes 50+ ready-to-use widgets. Each has its own header — include what you need:

```cpp
#include <maya/widget/badge.hpp>
#include <maya/widget/progress.hpp>
#include <maya/widget/table.hpp>
```

All widgets satisfy the `Node` concept and work directly in DSL expressions.

### Input widgets

| Widget | Description |
|--------|-------------|
| `Input<Cfg>` | Single-line text input with cursor, placeholder, password mode |
| `TextArea` | Multi-line text editor with line numbers |
| `Checkbox` / `ToggleSwitch` | Toggle controls |
| `Radio<N>` | Radio button group |
| `Button` | Clickable button (default / filled variants) |
| `Select` | Dropdown/list selector with filtering |
| `Slider` | Numeric slider with range |

### Data display

| Widget | Description |
|--------|-------------|
| `Table` | Column-aligned data table with headers and borders |
| `List<Cfg>` | Scrollable list with selection and search |
| `Tree<T>` | Tree view with expand/collapse |
| `Sparkline` | Inline sparkline chart |

### Navigation

| Widget | Description |
|--------|-------------|
| `Tabs` | Tab bar with selection indicator |
| `Breadcrumb` | Path breadcrumb trail |
| `Menu` | Vertical menu with keyboard navigation |
| `CommandPalette` | Fuzzy search command palette |

### Display

| Widget | Description |
|--------|-------------|
| `Badge` | Inline colored label — `Badge::success("ok")`, `Badge::error("fail")` |
| `Callout` | Severity-based alert — `Callout::error("title", "desc")` |
| `ToastManager` | Notification stack with auto-dismiss |
| `FileRef` | File path with dimmed directory, underlined filename |
| `UserMessage` / `AssistantMessage` | Chat message bubbles |
| `ProgressBar` | Progress bar with dynamic width |
| `ActivityBar` | Activity status indicator |
| `Divider` | Labeled horizontal divider |

### Overlay

| Widget | Description |
|--------|-------------|
| `Modal` | Dialog with title, body, action buttons |
| `Popup` | Tooltip/popup box |
| `ScrollableView` | Scrollable viewport with scrollbar |

### Visualization

| Widget | Description |
|--------|-------------|
| `BarChart` | Horizontal/vertical bar chart |
| `LineChart` | Braille line chart |
| `Gauge` | Linear or circular gauge |
| `Heatmap` | 2D heatmap grid |
| `PixelCanvas` | Freeform half-block pixel drawing |

### Specialized

| Widget | Description |
|--------|-------------|
| `Markdown` | Full markdown renderer (headers, code, lists, tables, links) |
| `DiffView` | Unified diff display with add/remove highlighting |
| `PlanView` | Step-by-step plan viewer |
| `LogViewer` | Log viewer with severity filtering |
| `Calendar` | Month calendar grid |
| `KeyHelp` | Keyboard shortcut help panel |
| `Disclosure` | Expand/collapse section |
| `ThinkingBlock` | AI thinking/streaming indicator |

### Tool widgets (for AI agent UIs)

| Widget | Description |
|--------|-------------|
| `ToolCall` | Generic tool invocation display |
| `BashTool` | Shell command execution |
| `EditTool` / `WriteTool` / `ReadTool` | File operation displays |
| `FetchTool` | URL fetch display |
| `SearchResult` | Search result display |
| `AgentTool` | Sub-agent invocation display |
| `Permission` | Permission request dialog |

### Usage example

```cpp
#include <maya/maya.hpp>
#include <maya/widget/badge.hpp>
#include <maya/widget/callout.hpp>
#include <maya/widget/progress.hpp>
#include <maya/widget/table.hpp>

using namespace maya::dsl;

// Widgets work directly in DSL expressions
auto ui = v(
    Badge::info("status"),
    Callout::success("Build passed", "All 42 tests green"),
    ProgressBar{.value = 0.75f, .label = "Progress"},
    Table({
        .headers = {"Name", "Value"},
        .rows = {{"fps", "60"}, {"mem", "128MB"}},
    })
) | padding(1);
```

---

## Signals

Fine-grained reactivity. Signals are the state layer — the render function reads them, and the framework re-renders automatically when they change.

### Signal\<T\>

```cpp
Signal<int>         count{0};
Signal<std::string> name{"alice"};
Signal<bool>        active{true};
Signal<MyStruct>    data{MyStruct{}};
```

```cpp
count.get()          // read (auto-tracks in reactive scopes)
count()              // shorthand for get()
count.set(42)        // write (notifies dependents if value changed)
count.update([](int& n) { n *= 2; })   // in-place mutation
count.version()      // monotonically increasing change counter
```

`set()` is a no-op if the new value equals the current value (for equality-comparable types). `update()` always notifies.

### computed()

```cpp
auto area = computed([&] { return width.get() * height.get(); });
area.get()   // re-evaluates only when width or height changed
```

Memoized. Dependencies are tracked automatically — no explicit dependency list. Chaining works:

```cpp
auto doubled  = computed([&] { return count.get() * 2; });
auto greeting = computed([&] { return "Value: " + std::to_string(doubled.get()); });
```

`Signal::map()` is a convenience shorthand:
```cpp
auto doubled = count.map([](int n) { return n * 2; });
```

### Effect / effect()

Class form — RAII, alive as long as the object exists:

```cpp
Effect e([&] {
    // Runs immediately, then re-runs whenever any signal read inside changes.
    std::println("count is now {}", count.get());
});
// e destroyed here → unsubscribed
```

Free function form — returns an `Effect` (same RAII semantics, more concise):

```cpp
auto e = effect([&] { title.set("App — " + std::to_string(count.get())); });
e.dispose();  // explicit early cleanup (optional)
```

Use effects for synchronizing signals to external state. Don't use them to trigger renders — the render function already reads signals directly.

### Batch / batch()

Prevents multiple re-renders when setting several signals at once.

RAII scope form:
```cpp
{
    Batch b;
    x.set(10);   // deferred
    y.set(20);   // deferred
    z.set(30);   // deferred
}
// Subscribers notified once here, not three times
```

Functional form:
```cpp
batch([&] {
    x.set(10);
    y.set(20);
});
```

---

## Style

```cpp
struct Style {
    std::optional<Color> fg{};
    std::optional<Color> bg{};
    bool bold          = false;
    bool dim           = false;
    bool italic        = false;
    bool underline     = false;
    bool strikethrough = false;
    bool inverse       = false;
};
```

Build with fluent methods — each returns a new `Style` (immutable):

```cpp
Style{}                          // empty — inherits terminal defaults
Style{}.with_bold()
Style{}.with_dim()
Style{}.with_italic()
Style{}.with_underline()
Style{}.with_strikethrough()
Style{}.with_inverse()
Style{}.with_fg(Color::red())
Style{}.with_bg(Color::rgb(30, 30, 40))
Style{}.with_bold().with_fg(Color::hex(0x7AA2F7))
```

Compose styles with `operator|` — right-hand side wins on conflicts:
```cpp
auto heading = bold_style | Style{}.with_fg(Color::cyan());
```

### Predefined styles

```cpp
bold_style       // Style{}.with_bold()
dim_style        // Style{}.with_dim()
italic_style     // Style{}.with_italic()
underline_style  // Style{}.with_underline()
```

---

## Color

```cpp
// ANSI 16 named colors
Color::black()         Color::bright_black()    // (gray)
Color::red()           Color::bright_red()
Color::green()         Color::bright_green()
Color::yellow()        Color::bright_yellow()
Color::blue()          Color::bright_blue()
Color::magenta()       Color::bright_magenta()
Color::cyan()          Color::bright_cyan()
Color::white()         Color::bright_white()

// 24-bit truecolor
Color::rgb(255, 128, 0)          // r, g, b — each 0–255
Color::hex(0xFF8000)             // compile-time hex literal (consteval)

// ANSI 256-color palette
Color::indexed(214)              // 256-color index

// HSL (perceptually uniform)
Color::hsl(30.0f, 1.0f, 0.5f)   // hue (0–360), saturation (0–1), lightness (0–1)
```

`Color` is a value type — trivially copyable, constexpr-constructible, zero-overhead.

### Color accessors

```cpp
uint8_t r() const;   // red channel (0–255) — only meaningful for RGB colors
uint8_t g() const;   // green channel
uint8_t b() const;   // blue channel
auto    kind() const; // ColorKind: Ansi16, Ansi256, Rgb, or Hsl
```

---

## Theme

```cpp
struct Theme {
    // Primary palette
    Color primary, secondary, accent;

    // Status
    Color success, error, warning, info;

    // Text
    Color text, inverse_text, muted;

    // Surfaces
    Color surface, background, border;

    // Diff highlighting
    Color diff_added, diff_removed, diff_changed;

    // Extras
    Color highlight, selection, cursor, link, placeholder, shadow, overlay;
};
```

### Built-in themes

```cpp
theme::dark        // default — truecolor dark (Tokyo Night inspired)
theme::light       // truecolor light
theme::dark_ansi   // 16-color dark — works on any terminal
theme::light_ansi  // 16-color light
```

Pass via `RunConfig`:
```cpp
run<MyApp>({.theme = theme::light});
```

Access in `canvas_run` via `ctx.theme`. In Program apps, the theme is available through the runtime:
```cpp
// canvas_run / low-level
[&](const Ctx& ctx) {
    return text("Status", Style{}.with_fg(ctx.theme.success));
}
```

### Custom themes

`Theme::derive()` creates a compile-time override:

```cpp
constexpr auto my_theme = Theme::derive(
    theme::dark,
    [](Theme& t) { t.primary = Color::hex(0xFF6B6B); },
    [](Theme& t) { t.accent  = Color::hex(0xFFE66D); }
);

run<MyApp>({.theme = my_theme});
```

---

## Borders

```cpp
enum class BorderStyle {
    None,          // no border
    Single,        // ┌─┐│└─┘  standard box drawing
    Double,        // ╔═╗║╚═╝  double lines
    Round,         // ╭─╮│╰─╯  rounded corners
    Bold,          // ┏━┓┃┗━┛  heavy lines
    SingleDouble,  // mixed horizontal/vertical
    DoubleSingle,  // mixed
    Classic,       // +-+|+-+   ASCII fallback
    Arrow,         // ↑→↓←
};
```

Apply via DSL pipes:

```cpp
// Compile-time border
v(text("content")) | border_<Round>
v(text("content")) | border_<Round> | bcol<50, 54, 62>
v(text("content")) | bordered<Round, 0x323746>   // shorthand

// Runtime border (dynamic color/style)
v(text("content")) | border(Round) | bcolor(ctx.theme.border)
```

### Border title

```cpp
v(text("content")) | border(Round) | btext("My Panel")
v(text("content")) | border(Round) | btext("Status", BorderTextPos::Bottom, BorderTextAlign::Right)
```

`BorderTextPos`: `Top`, `Bottom`
`BorderTextAlign`: `Left`, `Center`, `Right`

### Selective sides

```cpp
box().border(BorderStyle::Single)
     .border_sides(BorderSides::all())         // default — all four sides
     .border_sides(BorderSides::horizontal())  // top + bottom only
     .border_sides(BorderSides::vertical())    // left + right only
     .border_sides(BorderSides::top())         // single side
     .border_sides(BorderSides::bottom())
     .border_sides(BorderSides::left())
     .border_sides(BorderSides::right())
```

Combine sides with `operator|`:
```cpp
.border_sides(BorderSides::top() | BorderSides::bottom())
```

---

## Core types

### Columns / Rows

Strong integer wrappers that prevent mixing column and row counts:

```cpp
Columns w{80};    // column count
Rows    h{24};    // row count

w.value           // int — the raw value
w.raw()           // same as .value

// Arithmetic
Columns a{10}, b{5};
a + b   // Columns{15}
a - b   // Columns{5}
a * 2   // Columns{20}
a == b  // bool
```

### Size

```cpp
struct Size {
    Columns width;
    Rows    height;
};

Size s{80, 24};
s.width.value    // 80
s.height.value   // 24
s.is_zero()      // true if width==0 && height==0
s.area()         // width.value * height.value
```

### Position

```cpp
struct Position {
    Columns x;
    Rows    y;
};

Position p{10, 5};
p.x.value         // 10
p.y.value         // 5
Position::origin() // {0, 0}
```

### Rect

```cpp
struct Rect {
    Position origin;  // top-left corner
    Size     size;
};

Rect r{{0,0}, {80,24}};
r.left()          // Columns — left edge
r.top()           // Rows    — top edge
r.right()         // Columns — right edge (exclusive)
r.bottom()        // Rows    — bottom edge (exclusive)
r.contains(pos)   // bool — is Position inside this rect?
r.intersect(other)// std::optional<Rect> — overlap region
r.unite(other)    // Rect — bounding box of both
```

### Edges\<T\>

Uniform padding/margin type:

```cpp
Edges<int> e1{2};          // all sides = 2
Edges<int> e2{1, 2};       // vertical=1, horizontal=2
Edges<int> e3{1, 2, 3, 4}; // top, right, bottom, left

e3.top, e3.right, e3.bottom, e3.left   // individual sides
e3.horizontal()  // left + right
e3.vertical()    // top + bottom
```

### Dimension

Used for flex sizing properties:

```cpp
Dimension::auto_()          // let layout decide (default)
Dimension::fixed(40)        // exactly N cells (alias: px)
Dimension::px(40)           // same as fixed
Dimension::percent(50.0f)   // percentage of parent

// UDL shorthand
using namespace maya::literals;
50_pct              // same as Dimension::percent(50)

// Inspection
d.is_auto()         // bool
d.is_fixed()        // bool
d.is_percent()      // bool
d.resolve(int parent_size)  // int — computes pixel value given parent size
```

### Strong\<Tag, T\>

Base template for all strong-typed wrappers (`Columns`, `Rows`). You don't use this directly, but it defines the behavior:

```cpp
template<typename Tag, typename T>
struct Strong {
    T value{};
    T raw() const;        // alias for value
    // arithmetic: +, -, *, /, %, ==, !=, <, >, <=, >=
    // all return Strong<Tag, T>
};
```

### MoveOnly

Utility base class that deletes copy operations. Inherit to make your type move-only:

```cpp
class MyResource : MoveOnly {
    // copy-constructor and copy-assignment are deleted
    // move-constructor and move-assignment are defaulted
};
```

---

## Error handling

Used by the low-level API (terminal setup, I/O). `run<P>()` handles all errors internally. You only need this when using `canvas_run()` or the type-state terminal directly.

```cpp
enum class ErrorKind {
    Io,          // OS-level I/O error (errno set)
    ParseError,  // malformed data
    NotFound,
    PermissionDenied,
    InvalidInput,
    Timeout,
    Unknown,
};

struct Error {
    ErrorKind   kind;
    std::string message;

    static Error from_errno(std::string_view context);   // wraps current errno
};
```

### Result\<T\>

`expected`-based return type — either a value or an error:

```cpp
Result<int>    // either int or Error
Status         // Result<void> — either success or Error
```

```cpp
// Construct
Result<int> r = ok(42);
Status      s = ok();
Result<int> e = err<int>(Error{ErrorKind::Io, "read failed"});

// Inspect
if (r)           { int v = *r; }         // operator bool, operator*
if (r.has_value()) { ... }
r.value()        // throws if error
r.value_or(0)    // default on error
r.error()        // Error — only valid if !r
```

### MAYA_TRY / MAYA_TRY_VOID

Propagate errors from `Result`-returning functions:

```cpp
Result<Terminal<Raw>> setup() {
    auto cooked = Terminal<Cooked>::create();
    MAYA_TRY(raw, std::move(*cooked).enable_raw_mode());
    // raw is Terminal<Raw> here
    return ok(std::move(raw));
}

Status init_only() {
    MAYA_TRY_VOID(some_result_returning_fn());
    return ok();
}
```

Both macros early-return the enclosing function with the error on failure.

---

## Concepts

C++20 concepts that the library uses internally and exposes for generic component authoring:

```cpp
// Has measure(int max_width) -> Size
template<typename T> concept Measurable;

// Has render(Canvas&, Rect) -> void
template<typename T> concept Renderable;

// Has a render() method (React-style component)
template<typename T> concept Component;

// Has on_key(const KeyEvent&) -> bool
template<typename T> concept InputHandler;

// Has fg(), bg(), bold() properties
template<typename T> concept Styleable;

// Range whose value_type converts to T
template<typename R, typename T> concept RangeLike;

// Convertible to std::string_view
template<typename T> concept StringLike;

// Measurable + Renderable
template<typename T> concept Widget;

// Widget + InputHandler
template<typename T> concept InteractiveWidget;

// Widget + Styleable
template<typename T> concept StyledWidget;
```

---

## Context system

React-style context — pass values down the element tree without threading them through every intermediate component.

### Context\<T\>

Zero-sized typed key:

```cpp
inline constexpr Context<Theme>     kThemeCtx{};
inline constexpr Context<AppState>  kStateCtx{};
```

### ContextMap

```cpp
class ContextMap {
public:
    template<typename T>
    void set(T value);              // store a value

    template<typename T>
    T* get();                       // retrieve, or nullptr

    template<typename T>
    T get_or(T fallback);           // retrieve with default

    template<typename T>
    bool has() const;               // check presence

    template<typename T>
    void remove();                  // erase

    ContextMap fork() const;        // child context (copy-on-write)

    void clear();
    std::size_t size() const;
    bool empty() const;
};
```

Copy-on-write: `fork()` creates a child context that shares the parent's data until a write occurs, making it cheap to pass down a tree.

---

## Advanced

The sections above cover the stable public API (`<maya/maya.hpp>` + `<maya/widget/*.hpp>`). Everything below is the lower-level machinery, accessible via `<maya/internal.hpp>`. These interfaces may change across versions.

### canvas_run — low-level escape hatch

If you need capabilities beyond `run<P>()` — custom event loop timing, direct canvas access, non-blocking animations — use `canvas_run()`:

```cpp
canvas_run(
    {.title = "my app", .mouse = true},
    [&](const Event& ev) -> bool { /* handle events, return false to quit */ },
    [&](const Ctx& ctx) -> Element { /* render */ }
);
```

This is the callback-based API used by maya's canvas examples (doom fire, raymarcher, etc.).

### Type-state terminal

```cpp
auto cooked = Terminal<Cooked>::create(STDIN_FILENO);
auto raw    = std::move(*cooked).enable_raw_mode();
auto term   = std::move(*raw).enter_alt_screen();
// RAII: destructor restores all terminal state in reverse order
```

State types: `Cooked` → `Raw` → `AltScreen`. Invalid transitions are compile errors. `write()`, `read_raw()`, `size()`, `fd()` are available on `Raw` and `AltScreen`.

### RawGuard

Lightweight RAII wrapper for raw mode without the full type-state Terminal:

```cpp
auto guard_result = RawGuard::create(STDIN_FILENO);
if (!guard_result) { /* handle error */ }
auto guard = std::move(*guard_result);
// terminal is in raw mode

guard.restore();        // explicit restore (also called by destructor)
guard.is_active()       // bool — whether raw mode is currently active
// destructor calls restore() automatically
```

`RawGuard` is move-only. Useful when you need raw mode for a short window without the overhead of a full `Terminal` state machine.

### Canvas / diff / StylePool

Low-level 2D rendering surface:

```cpp
StylePool pool;
Canvas front(W, H, &pool), back(W, H, &pool);

// Intern styles once — returns uint16_t ID
uint16_t sid = pool.intern(Style{}.with_bold().with_fg(Color::green()));

// Paint cells into back
back.set(col, row, U'A', sid);
back.mark_all_damaged();

// Diff and emit ANSI
std::string out;
diff(front, back, pool, out);
term.write(out);
std::swap(front, back);
```

On resize, clear and re-intern:
```cpp
pool.clear();
front.resize(new_w, new_h);
back.resize(new_w, new_h);
// re-intern all styles and re-paint
```

### FrameBuffer

Higher-level double-buffered renderer used by `App` internally:

```cpp
FrameBuffer fb(width, height);
fb.resize(new_w, new_h);
fb.invalidate();  // force full repaint next frame

// set_cursor for cursor-aware rendering
fb.set_cursor(Position{10, 5});
fb.set_cursor_visible(true);

// render() runs layout, paints, diffs, returns complete ANSI string
const std::string& ansi = fb.render(element_tree, theme::dark);
// reference valid until the next render() call

// Access internals
StylePool& pool = fb.style_pool();
Frame& front    = fb.front();
Frame& back     = fb.back();
```

### RenderPipeline (type-state)

```cpp
RenderPipeline<stage::Idle>::start(back_canvas, pool, theme, out_buf)
    .clear()                        // Idle    → Cleared
    .paint(element)                 // Cleared → Painted
    .open_frame()                   // Painted → Opened (sync_start + hide_cursor)
    .write_diff(front_canvas)       // Opened  → Opened (ANSI diff bytes)
    .apply_cursor(back_cur, front_cur, back_vis, front_vis)
    .close_frame();                 // Opened  → Closed (reset + sync_end)
```

Illegal stage transitions (e.g., calling `paint()` from `Idle`) are compile errors, not runtime errors.

Stage tags: `stage::Idle`, `stage::Cleared`, `stage::Painted`, `stage::Opened`, `stage::Closed`.

### render_tree()

Walks the element tree and paints into a Canvas:

```cpp
void render_tree(const Element& root, Canvas& canvas,
                 StylePool& pool, const Theme& theme);
```

Called automatically by `RenderPipeline::paint()`. Use directly when you need frame-by-frame control without the full pipeline.

### Writer

Buffered, optimized terminal output with peephole optimization:

```cpp
Writer w;

// Queue operations
w.write_text("hello");
w.move_cursor(dx, dy);            // relative move
w.move_to_col(col);               // move to absolute column
w.set_style(style);               // emit SGR
w.begin_hyperlink(url);           // OSC 8
w.end_hyperlink();
w.show_cursor();
w.hide_cursor();
w.clear_line();
w.clear_screen();

// Flush: optimizes, serializes, wraps in DEC 2026 sync markers,
// then writes atomically to the terminal fd
w.flush(fd);
```

The peephole optimizer collapses consecutive writes, cancels redundant cursor hide/show pairs, and merges adjacent SGR sequences.

### ansi:: namespace

Low-level ANSI escape sequence generation:

```cpp
// Pre-built string constants
ansi::show_cursor          // "\x1b[?25h"
ansi::hide_cursor          // "\x1b[?25l"
ansi::alt_screen_enter     // "\x1b[?1049h"
ansi::alt_screen_leave     // "\x1b[?1049l"
ansi::sync_start           // "\x1b[?2026h"
ansi::sync_end             // "\x1b[?2026l"
ansi::reset                // "\x1b[0m"
ansi::mouse_on             // button tracking
ansi::mouse_off
ansi::mouse_motion_on      // all-motion tracking (\x1b[?1003h)
ansi::mouse_motion_off
ansi::focus_on             // focus events
ansi::focus_off
ansi::bracketed_paste_on
ansi::bracketed_paste_off

// Cursor movement — return std::string
ansi::move_up(n)
ansi::move_down(n)
ansi::move_right(n)
ansi::move_left(n)
ansi::move_to(col, row)    // 1-based
ansi::move_to_col(col)
ansi::save_pos()
ansi::restore_pos()
ansi::home()

// Screen clearing
ansi::clear_screen()
ansi::clear_line()
ansi::clear_to_end()
ansi::clear_to_start()

// Color
ansi::fg(Color c)          // foreground SGR sequence
ansi::bg(Color c)          // background SGR sequence

// OSC
ansi::set_title(std::string_view title)
ansi::hyperlink_start(std::string_view url)
ansi::hyperlink_end()
ansi::set_clipboard(std::string_view text)

// Zero-allocation variant — appends directly to a string
ansi::write_move_to(std::string& out, int col, int row);
```

### StyleApplier

Generates minimal SGR transitions between styles:

```cpp
ansi::StyleApplier sa;

// Full style from scratch
sa.apply(style)             // -> std::string

// Minimal delta (no redundant attributes)
sa.transition(from, to)     // -> std::string

// Zero-allocation — append to string directly
sa.apply_to(style, out);
sa.transition_to(from, to, out);
```

`transition()` is used by `diff()` internally to keep frame output minimal.
