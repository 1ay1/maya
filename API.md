# maya API reference

maya is a C++26 TUI library. One header, one `run()` call, zero boilerplate.

```cpp
#include <maya/maya.hpp>
using namespace maya;
```

---

## Table of contents

1. [Quick start](#quick-start)
2. [run()](#run)
3. [RunConfig](#runconfig)
4. [Ctx — render context](#ctx)
5. [Event helpers](#event-helpers)
6. [Element DSL](#element-dsl)
7. [Layout](#layout)
8. [Signals](#signals)
9. [Style](#style)
10. [Color](#color)
11. [Theme](#theme)
12. [Borders](#borders)
13. [Core types](#core-types)
14. [Error handling](#error-handling)
15. [Concepts](#concepts)
16. [Context system](#context-system)
17. [Advanced — low-level API](#advanced)

---

## Quick start

```cpp
#include <maya/maya.hpp>
using namespace maya;

int main() {
    Signal<int> count{0};

    run(
        {.title = "counter"},
        [&](const Event& ev) {
            if (key(ev, '+')) count.update([](int& n) { ++n; });
            if (key(ev, '-')) count.update([](int& n) { --n; });
            return !key(ev, 'q');        // false = quit
        },
        [&](const Ctx& ctx) {
            auto color = count.get() >= 0 ? ctx.theme.success : ctx.theme.error;
            return box().direction(Column).padding(1)(
                text("Count: " + std::to_string(count.get()),
                     Style{}.with_bold().with_fg(color)),
                text("[+/-] change  [q] quit", dim_style)
            );
        }
    );
}
```

That's a complete, reactive TUI application. No error handling ceremony, no object management, no headers beyond `maya/maya.hpp`.

---

## run()

```cpp
// With config
template<AnyEventFn E, AnyRenderFn R>
void run(RunConfig cfg, E&& event_fn, R&& render_fn);

// Without config (uses all defaults)
template<AnyEventFn E, AnyRenderFn R>
void run(E&& event_fn, R&& render_fn);
```

Starts the application. Blocks until the app quits. Errors are printed to stderr and the process exits — you never deal with terminal-init failure in application code.

**`event_fn`** signature — either of:
```cpp
[&](const Event& ev) -> bool { ... }   // false = quit
[&](const Event& ev) -> void { ... }   // use maya::quit() to exit
```

**`render_fn`** signature — either of:
```cpp
[&]() -> Element { ... }               // no context
[&](const Ctx& ctx) -> Element { ... } // with live size + theme
```

Both variants are detected automatically at compile time via concepts. Mix and match:

```cpp
// void event handler + context-aware render
run(
    [&](const Event& ev) { if (key(ev, 'q')) quit(); },
    [&](const Ctx& ctx)  { return text("w=" + std::to_string(ctx.size.width.value)); }
);
```

### maya::quit()

```cpp
void quit() noexcept;
```

Schedules a clean exit. Safe to call from anywhere — Effect lambdas, signal handlers, render functions, async callbacks. The exit happens after the current frame completes.

```cpp
Signal<bool> should_quit{false};
Effect e([&] { if (should_quit.get()) quit(); });
```

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
run({.title = "my app"},                    handler, render);
run({.title = "editor", .mouse = true},     handler, render);
run({.theme = theme::light},                handler, render);
run({},                                     handler, render);  // all defaults
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

Passed to the render function when it declares a `(const Ctx&)` parameter. Updated automatically on resize.

```cpp
[&](const Ctx& ctx) {
    // Responsive layout based on width
    bool narrow = ctx.size.width.value < 80;

    return box().direction(Column)(
        text("Hello", Style{}.with_fg(ctx.theme.primary)),
        narrow ? text("narrow") : text("wide layout")
    );
}
```

`Size` fields:
```cpp
ctx.size.width.value   // int — columns
ctx.size.height.value  // int — rows
```

---

## Event helpers

Stable predicate functions over the `Event` variant. Use these instead of `std::get_if` — the internal `Event` representation can change without breaking your code.

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

Mouse requires `.mouse = true` in `RunConfig`. For hover (motion without button), send `\x1b[?1003h` manually after terminal setup (low-level path only; the high-level `run()` enables button-press/release tracking by default).

### Other predicates

```cpp
bool resized(const Event& ev, int* w = nullptr, int* h = nullptr);
bool pasted(const Event& ev, std::string* content = nullptr);
bool focused(const Event& ev);
bool unfocused(const Event& ev);
```

```cpp
// Typical usage
[&](const Event& ev) {
    int w, h;
    if (resized(ev, &w, &h)) relayout(w, h);

    std::string text;
    if (pasted(ev, &text)) handle_paste(text);

    return true;
}
```

---

## Element DSL

Elements are immutable value types. Build them with factory functions and the fluent `box()` builder. The render function returns one root `Element` per frame — maya diffs it against the previous frame.

### text()

```cpp
Element text(std::string_view content, Style s = {});
Element text(std::string_view content, Style s, TextWrap wrap);
```

```cpp
text("Hello, world!")
text("Error", Style{}.with_bold().with_fg(Color::red()))
text("Long line…", dim_style, TextWrap::Wrap)
```

`TextWrap` values:
```cpp
TextWrap::Wrap           // word-wrap at box boundary (default)
TextWrap::TruncateEnd    // clip at end with "…"
TextWrap::TruncateMiddle // clip in middle with "…"
TextWrap::TruncateStart  // clip at start with "…"
TextWrap::NoWrap         // no wrapping, no truncation
```

String children passed directly to `box()` are auto-wrapped in `text()`:
```cpp
box().direction(Column)(
    "This is auto-wrapped",        // same as text("...")
    text("This is explicit")
)
```

Full-width and CJK characters are measured correctly — `string_width()` returns terminal column count, not byte count.

### box()

```cpp
BoxBuilder box();
```

Returns a builder. Chain property methods, then call it like a function with children to finalize:

```cpp
box().direction(Column).padding(1)(
    text("child 1"),
    text("child 2"),
)
```

A `BoxBuilder` converts implicitly to `Element` for the no-children case:
```cpp
Element spacer_elem = box().grow(1.0f);  // implicit conversion
```

### spacer()

```cpp
Element spacer();
```

A flex-growing box that absorbs remaining space. Pushes siblings to opposite ends:

```cpp
box().direction(Row)(
    text("Left"),
    spacer(),
    text("Right")   // pushed to the far right
)
```

### separator() / vseparator()

```cpp
Element separator(BorderStyle bs = BorderStyle::Single);   // horizontal line
Element vseparator(BorderStyle bs = BorderStyle::Single);  // vertical line
```

```cpp
box().direction(Column)(
    text("Section A"),
    separator(),
    text("Section B"),
    separator(BorderStyle::Double)
)
```

### map_elements()

```cpp
template<std::ranges::range R, typename Proj>
ElementList map_elements(R&& range, Proj proj);
```

Maps a collection to child elements:

```cpp
auto items = std::vector<std::string>{"alpha", "beta", "gamma"};
box().direction(Column)(
    map_elements(items, [](const std::string& s) {
        return text("• " + s);
    })
)
```

### ElementList

`map_elements()` returns an `ElementList` — a transparent fragment type. When passed to `box()` as a child, its elements are spliced directly into the parent rather than wrapped in an extra box:

```cpp
ElementList list = map_elements(items, [](auto& s) { return text(s); });
box()(list, text("footer"))   // items appear flat, not nested
```

### Element introspection

For building generic components or element transformers:

```cpp
bool is_box(const Element& e);
bool is_text(const Element& e);
bool is_list(const Element& e);

BoxElement*  as_box(Element& e);   // nullptr if not a box
TextElement* as_text(Element& e);  // nullptr if not a text
ElementList* as_list(Element& e);  // nullptr if not a list

// Walk the tree depth-first
void visit_element(const Element& e, auto&& visitor);
```

---

## Layout

Layout is flexbox-inspired. Set properties on `BoxBuilder` via method chaining.

### Direction

```cpp
box().direction(Column)         // children stacked top-to-bottom (default)
box().direction(Row)            // children side by side left-to-right
box().direction(ColumnReverse)  // bottom-to-top
box().direction(RowReverse)     // right-to-left
```

### Spacing

```cpp
.padding(1)              // 1 cell on all sides
.padding(1, 2)           // vertical=1, horizontal=2
.padding(1, 2, 1, 2)    // top, right, bottom, left

.margin(1)               // outer margin (same variants)

.gap(1)                  // space between children
```

### Sizing

```cpp
.width(Dimension::px(40))        // exactly 40 columns
.width(Dimension::percent(50))   // 50% of parent
.width(Dimension::auto_())       // let layout decide (default)
.height(Dimension::px(10))
.min_width(Dimension::px(20))
.max_width(Dimension::px(80))
.grow(1.0f)   // flex-grow: absorb remaining space proportionally
.shrink(1.0f) // flex-shrink
.basis(Dimension::px(20))
```

`_pct` UDL shorthand:
```cpp
using namespace maya::literals;
.width(50_pct)   // same as Dimension::percent(50)
```

### Alignment

```cpp
// Cross-axis alignment of children
.align_items(Align::Start)        // default
.align_items(Align::Center)
.align_items(Align::End)
.align_items(Align::Stretch)
.align_items(Align::Baseline)
.align_items(Align::Auto)

// Self alignment within parent
.align_self(Align::Center)

// Main-axis distribution
.justify(Justify::Start)          // default
.justify(Justify::End)
.justify(Justify::Center)
.justify(Justify::SpaceBetween)
.justify(Justify::SpaceAround)
.justify(Justify::SpaceEvenly)
```

### Overflow

```cpp
.overflow(Overflow::Visible)   // default — children can exceed bounds
.overflow(Overflow::Hidden)    // clip children at box boundary
.overflow(Overflow::Scroll)    // clip + allow scroll offset
```

### Wrapping

```cpp
.wrap(FlexWrap::NoWrap)      // default
.wrap(FlexWrap::Wrap)        // wrap children onto next line/column
.wrap(FlexWrap::WrapReverse) // wrap in reverse direction
```

### Display

```cpp
.display(Display::Flex)   // default — participates in layout
.display(Display::None)   // removed from layout entirely (not hidden, not measured)
```

### Styling a box

```cpp
.fg(Color::white())               // foreground color for the box region
.bg(Color::rgb(30, 30, 40))       // background fill
.style(Style{}.with_dim())        // full style override
```

### Complete example

```cpp
box().direction(Column).padding(1).gap(1)(
    // Header row: title left, status right
    box().direction(Row)(
        text("My App", bold_style),
        spacer(),
        text("READY", Style{}.with_fg(Color::green()))
    ),
    separator(),
    // Content area grows to fill remaining height
    box().direction(Row).grow(1)(
        // Sidebar: fixed width
        box().direction(Column).width(Dimension::px(20)).padding(1)(
            text("Nav item 1"),
            text("Nav item 2")
        ),
        vseparator(),
        // Main panel: stretches
        box().direction(Column).grow(1).padding(1)(
            text("Content here")
        )
    ),
    separator(),
    text("[q] quit", dim_style)
)
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
run({.theme = theme::light}, handler, render);
```

Access in the render function via `ctx.theme`:
```cpp
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

run({.theme = my_theme}, handler, render);
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

Apply to a box:

```cpp
box().border(BorderStyle::Round)(
    text("content")
)

// With color
box().border(BorderStyle::Single, Color::hex(0x3B4261))(...)

// Color set separately (useful with theme colors)
box().border(BorderStyle::Round).border_color(ctx.theme.primary)(...)
```

### Border title

```cpp
box().border(BorderStyle::Round)
     .border_text("My Panel")
     .border_text("My Panel", BorderTextPos::Bottom, BorderTextAlign::Right)(
    text("content")
)
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

Used by the low-level API (terminal setup, I/O). `run()` handles all errors internally. You only need this when using `App::builder()` or the type-state terminal directly.

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

The sections above cover the stable public API — what you should use in applications. Everything below is the lower-level machinery. It's fully accessible but may change across versions.

### Direct App access

If you need capabilities beyond `run()` — custom event loop timing, multiple concurrent render targets, non-blocking animations — use `App` directly:

```cpp
auto result = App::builder()
    .title("my app")
    .mouse(true)
    .theme(theme::dark)
    .build();

if (!result) { /* handle error */ return 1; }

auto app = std::move(*result);
app.on_key([&](const KeyEvent& ke) -> bool { ... });
app.on_mouse([&](const MouseEvent& me) -> bool { ... });
app.on_resize([&](Size sz) { ... });

auto status = app.run([&] { return text("hello"); });
```

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
