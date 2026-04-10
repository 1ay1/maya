# The Compile-Time DSL

maya's DSL lets you define UI trees as C++26 template expressions. Text content,
styles, layout properties, and tree structure are all resolved at compile time.
The compiler itself enforces correctness — impossible states don't compile.

```cpp
#include <maya/maya.hpp>
using namespace maya::dsl;

constexpr auto ui = v(
    t<"Hello"> | Bold | Fg<100, 180, 255>,
    h(t<"A"> | Dim, t<"B">) | border_<Round> | pad<1>
);
```

Everything above — the tree shape, the text content, the styles, the border,
the padding — is a compile-time value. `.build()` converts it to a runtime
`Element` for rendering.

## Node Types

Every DSL construct is a **Node** — a type that satisfies:

```cpp
template <typename T>
concept Node = requires(const T& n) {
    { n.build() } -> std::convertible_to<Element>;
};
```

All nodes also have `operator Element()` for implicit conversion, so you can
pass them directly wherever an `Element` is expected.

### TextNode — Compile-Time Text

```cpp
t<"Hello World">                        // TextNode<"Hello World", CTStyle{}>
t<"Error"> | Bold | Fg<255, 80, 80>     // TextNode<"Error", {bold, red}>
```

`t<"...">` creates a `TextNode` parameterized on the string content (as an
NTTP). Pipe style tags to compose styling at compile time — no runtime cost.

The string is a `Str<N>` — a compile-time fixed string:

```cpp
template <std::size_t N>
struct Str {
    char data[N]{};
    consteval Str(const char (&s)[N]);
};
```

### BoxNode — Containers (v/h)

```cpp
v(child1, child2, child3)   // Vertical stack (column direction)
h(child1, child2, child3)   // Horizontal stack (row direction)
```

`v()` and `h()` create `BoxNode<Direction, Config, Children...>` — a variadic
template that captures the children by type. The config accumulates layout
modifiers applied via `|`. They accept any `DslChild` — either a `Node` or
an `ElementRange` (like `std::vector<Element>`).

```cpp
auto panel = v(
    t<"Title"> | Bold,
    t<"Body text"> | Dim
) | border_<Round> | pad<1, 2> | gap_<1>;
```

Children can be any mix of node types — `TextNode`, other `BoxNode`s,
`DynNode`, `RuntimeTextNode`, `SpacerNode`, etc. — **and** runtime collections
like `std::vector<Element>`:

```cpp
std::vector<Element> rows;
rows.push_back(text("Row 1") | Bold);
rows.push_back(text("Row 2") | Dim);

auto panel = v(
    t<"Header"> | Bold,
    rows,                     // vector<Element> flattened into children
    t<"Footer"> | Dim
) | border_<Round> | pad<1>;
```

Any type satisfying `ElementRange` (a range whose elements convert to `Element`)
works — `vector<Element>`, `span<Element>`, filtered views, etc.

### RuntimeTextNode — Dynamic Text

```cpp
text("hello")                               // RuntimeTextNode<string_view>
text(std::to_string(n))                     // RuntimeTextNode<string>
text(42)                                    // RuntimeTextNode<int>
text(3.14)                                  // RuntimeTextNode<double>
text("status: ok") | Bold | Fg<80,220,120> // Pipeable!
```

`text()` returns a `RuntimeTextNode<S>` — a proper Node that works in the DSL
tree and supports pipe-based styling just like compile-time nodes:

```cpp
template <typename S>
struct RuntimeTextNode {
    S content;
    Style style{};
    TextWrap wrap{TextWrap::Wrap};

    operator Element() const;
    Element build() const;
};
```

Use `text()` when content is known only at runtime. Use `t<"...">` when the
string is a compile-time literal.

### DynNode — Runtime Escape Hatch

```cpp
dyn([&] { return text("count = " + std::to_string(n)); })
dyn([&] { return some_element; })
```

`dyn()` wraps a lambda that returns an `Element` (or anything convertible to
`Element`). The lambda is called during `.build()` — it's the bridge between
the compile-time tree and runtime state:

```cpp
auto ui = v(
    t<"Static title"> | Bold,
    dyn([&] {
        // This runs at build time — access any runtime state here
        if (loading) return text("Loading...") | Dim;
        return text("Ready") | Bold | Fg<80, 220, 120>;
    })
);
```

The constraint is:

```cpp
template <typename F>
    requires std::invocable<F> && std::convertible_to<std::invoke_result_t<F>, Element>
auto dyn(F&& fn);
```

Since all nodes have `operator Element()`, your lambda can return any node type.

### MapNode — Range Projection

```cpp
std::vector<std::string> items = {"alpha", "beta", "gamma"};

auto list = v(
    t<"Items:"> | Bold,
    map(items, [](const auto& s) { return text(s) | Dim; })
);
```

`map()` projects a range through a function, producing an `ElementList`:

```cpp
template <std::ranges::range R, typename Proj>
auto map(R&& range, Proj&& proj) -> MapNode<decay_t<R>, decay_t<Proj>>;
```

The projection can return any Node — it will be `.build()`'d automatically.

### SpacerNode — Flexible Space

```cpp
h(t<"Left">, space, t<"Right">)   // space pushes "Right" to the far edge
h(t<"Left">, spacer(), t<"Right">) // same thing, function form
```

`space` (or `spacer()`) is a zero-content box with `grow: 1` — it absorbs all
remaining space in a flex container. Useful for pushing items apart:

```cpp
auto header = h(
    t<"maya"> | Bold,
    space,
    t<"v0.1"> | Dim
) | pad<0, 1>;
```

### SepNode / VSepNode — Separators

```cpp
v(t<"Above">, sep, t<"Below">)     // Horizontal line between items
h(t<"Left">, vsep, t<"Right">)     // Vertical line between items
```

`sep` draws a horizontal border line. `vsep` draws a vertical one. Both use
`BorderStyle::Single` with `BorderSides::horizontal()` or `vertical()`.

### BlankNode — Empty Line

```cpp
v(t<"Title">, blank_, t<"Body">)   // Empty line between title and body
```

`blank_` is an empty `TextElement` — a visual spacer that takes one line.

## Style Pipe Operators

Styles compose left-to-right with `|`:

```cpp
t<"Hello"> | Bold | Italic | Fg<255, 100, 80> | Bg<20, 20, 30>
```

### Text Attributes

| Tag | Effect |
|-----|--------|
| `Bold` | Bold weight |
| `Dim` | Dimmed/faint |
| `Italic` | Italic |
| `Underline` | Underlined |
| `Strike` | Strikethrough |
| `Inverse` | Swap fg/bg |

### Colors

```cpp
Fg<R, G, B>    // Foreground color (24-bit RGB)
Bg<R, G, B>    // Background color (24-bit RGB)
```

### How It Works

Each style tag is a `StyTag<CTStyle{...}>` — a zero-size type carrying a
compile-time style value. The `|` operator merges the tag's style into the
node's accumulated style. For `TextNode`, this produces a new `TextNode` type
with the merged style as a template parameter — zero runtime cost:

```cpp
template <Str S, CTStyle Sty, CTStyle V>
constexpr auto operator|(TextNode<S, Sty>, StyTag<V>) {
    return TextNode<S, Sty.merge(V)>{};  // New type, merged at compile time
}
```

For `RuntimeTextNode`, the merge happens at the `Style` object level:

```cpp
template <typename S, CTStyle V>
auto operator|(RuntimeTextNode<S> n, StyTag<V>) {
    n.style = n.style.merge(V.runtime());
    return n;
}
```

## Layout Pipe Operators

Layout modifiers apply only to `BoxNode` (i.e., to `v()` and `h()` results):

### Padding

```cpp
v(...) | pad<1>           // 1 cell on all sides
v(...) | pad<1, 2>        // 1 vertical, 2 horizontal
v(...) | pad<1, 2, 3, 4>  // top, right, bottom, left
```

Compile-time validated: negative values won't compile.

### Gap

```cpp
v(...) | gap_<1>    // 1 cell gap between children
```

### Grow

```cpp
h(
    v(...) | grow_<3>,   // Takes 3/5 of available space
    v(...) | grow_<2>    // Takes 2/5 of available space
)
```

### Border

```cpp
v(...) | border_<Round>     // Round corners: ╭ ╮ ╰ ╯
v(...) | border_<Single>    // Single lines: ┌ ┐ └ ┘
v(...) | border_<Double>    // Double lines: ╔ ╗ ╚ ╝
v(...) | border_<Thick>     // Bold lines: ┏ ┓ ┗ ┛
```

### Border Color (Type-State Safety)

```cpp
v(...) | border_<Round> | bcol<60, 65, 80>    // OK: border declared first
v(...) | bcol<60, 65, 80>                      // COMPILE ERROR: no border!
```

This is enforced via a `requires` clause:

```cpp
template <FlexDirection Dir, BoxCfg Cfg, typename... Cs, uint8_t R, uint8_t G, uint8_t B>
    requires (Cfg.has_border)  // <-- type-state check
constexpr auto operator|(BoxNode<Dir, Cfg, Cs...> n, BColTag<R, G, B>);
```

## Available BorderStyle Aliases

| Alias | BorderStyle | Characters |
|-------|------------|------------|
| `Round` | `BorderStyle::Round` | `╭ ─ ╮ │ ╯ ─ ╰ │` |
| `Single` | `BorderStyle::Single` | `┌ ─ ┐ │ ┘ ─ └ │` |
| `Double` | `BorderStyle::Double` | `╔ ═ ╗ ║ ╝ ═ ╚ ║` |
| `Thick` | `BorderStyle::Bold` | `┏ ━ ┓ ┃ ┛ ━ ┗ ┃` |

## Constexpr Everything

Because all DSL nodes are structural types with NTTP-safe fields, entire UI
trees can be `constexpr`:

```cpp
constexpr auto sidebar = v(
    t<"Navigation"> | Bold,
    t<"Home">    | Fg<100, 180, 255>,
    t<"Settings"> | Dim,
    t<"About">   | Dim
) | border_<Single> | pad<0, 1>;
```

The compiler evaluates the template instantiations at compile time. At runtime,
`.build()` just reads the template parameters and constructs the `Element` tree
— effectively a direct assignment with no parsing, no branching, and no
allocation beyond the element nodes themselves.

## Mixing Static and Dynamic

The real power is combining compile-time structure with runtime content:

```cpp
auto ui = v(
    // Compile-time: structure, static text, styles
    t<"Dashboard"> | Bold | Fg<100, 180, 255>,
    sep,

    // Runtime: dynamic data via dyn()
    dyn([&] { return text("Users: " + std::to_string(count)); }),

    // Runtime: list from data via map()
    map(items, [](const auto& item) {
        return h(
            text(item.name) | Bold,
            text(item.value) | Dim
        );
    }),

    // Compile-time: fixed footer
    space,
    t<"[q] quit"> | Dim
) | border_<Round> | pad<1>;
```

The tree structure, borders, padding, and static text are fully resolved at
compile time. Only the `dyn()` and `map()` nodes execute at runtime.

## Runtime Pipes

The compile-time DSL pipes (`pad<1>`, `border_<Round>`, `bcol<50,54,62>`) only
work with compile-time values. Runtime pipes fill the gap — same `|` syntax, but
accepting runtime values (variables, function results, theme colors, etc.).

All runtime pipe factories live in the `maya::dsl` namespace.

### Layout

```cpp
padding(int all)                        // uniform padding
padding(int v, int h)                   // vertical, horizontal
padding(int t, int r, int b, int l)     // top, right, bottom, left

gap(int g)                              // gap between children

margin(int all)                         // uniform margin
margin(int v, int h)                    // vertical, horizontal
margin(int t, int r, int b, int l)      // top, right, bottom, left

grow(float g = 1.0f)                    // flex grow factor
width(int w)                            // fixed width
height(int h)                           // fixed height
```

### Border

```cpp
border(BorderStyle bs)                  // runtime border style
bcolor(Color c)                         // runtime border color
btext(string s,                         // border text label
      BorderTextPos pos = Top,
      BorderTextAlign align = Start)
```

### Style

```cpp
fgc(Color c)                            // runtime foreground color
bgc(Color c)                            // runtime background color
```

### Alignment

```cpp
align(Align a)                          // align items (cross-axis)
justify(Justify j)                      // justify content (main-axis)
overflow(Overflow o)                     // overflow behavior
```

### Compile-Time vs Runtime Comparison

| Compile-time | Runtime | Purpose |
|---|---|---|
| `pad<1>` | `padding(1)` | Padding |
| `gap_<1>` | `gap(1)` | Gap |
| `border_<Round>` | `border(Round)` | Border style |
| `bcol<50,54,62>` | `bcolor(c)` | Border color |
| — | `btext("Title")` | Border text |
| `grow_<1>` | `grow(1.0f)` | Flex grow |
| `w_<40>` | `width(40)` | Fixed width |
| `h_<10>` | `height(10)` | Fixed height |
| `Fg<R,G,B>` | `fgc(color)` | Foreground |
| `Bg<R,G,B>` | `bgc(color)` | Background |
| — | `margin(1)` | Margin |
| — | `align(Center)` | Align items |
| — | `justify(End)` | Justify content |
| — | `overflow(Hidden)` | Overflow |

### Design

- Runtime pipes wrap any `Node` in a `WrappedNode` that applies `BoxBuilder`
  settings at build time.
- Chaining multiple runtime pipes reuses the same `WrappedNode` — no extra
  nesting per pipe.
- Compile-time pipes still work after a runtime pipe.
- `WrappedNode` satisfies `Node` — it can be a child of `v()` / `h()`.
- Both pipe types compose freely in the same expression.

### Examples

```cpp
// Runtime values from variables/theme
Color border_color = theme.border;
auto ui = v(
    t<"Status"> | Bold,
    text(message)
) | border(Round) | bcolor(border_color) | btext("Info") | padding(0, 1);

// Mix compile-time and runtime pipes
auto panel = v(children)
    | pad<1>                    // compile-time padding
    | border(user_border_style) // runtime border (from config)
    | bcolor(status_color)      // runtime color (from state)
    | grow(1.0f);               // runtime grow
```

## Hex Color Shorthand

For compile-time colors, hex literals are more convenient than separate RGB
components:

```cpp
fg<0xFF4444>   // hex foreground — shorthand for Fg<0xFF, 0x44, 0x44>
bg<0x1A1A2E>   // hex background
```

These are compile-time pipe tags just like `Fg` / `Bg`, but take a single
24-bit hex value instead of three separate `uint8_t` parameters.

## Style Presets

Compose style tags into reusable presets with `constexpr`:

```cpp
constexpr auto heading = Bold | fg<0xFFFFFF>;
constexpr auto error = Bold | fg<0xFF4444>;

t<"Title"> | heading
text(msg) | error
```

Because style tags are zero-size types, preset composition is resolved entirely
at compile time.

## User-Defined Literal

The `_t` literal creates a `TextNode` from a string literal, equivalent to
`t<"...">` but usable inline without angle brackets:

```cpp
"Hello"_t | Bold | fg<0x64B4FF>   // equivalent to t<"Hello">
```

## bordered<> — Combined Border + Color

`bordered<>` applies a border style and optional border color in a single pipe,
avoiding the two-step `border_<> | bcol<>` chain:

```cpp
v(...) | bordered<Round, 0x323746>   // border + color in one pipe
v(...) | bordered<Single>            // border only, no color
```

The color argument is a 24-bit hex value, same format as `fg<>` / `bg<>`.

## when() — Conditional Rendering

`when()` selects between two elements (or omits content entirely) based on a
runtime boolean:

```cpp
when(is_loading, spinner, content)     // show spinner or content
when(show_debug, debug_panel)          // omit else -> blank
```

When the condition is `false` and no else branch is provided, `when()` produces
a blank element that takes no space.

## visible() — Runtime Visibility

`visible()` is a runtime pipe that controls whether a node is rendered. Unlike
`when()`, it keeps the node in the tree — it just suppresses its output:

```cpp
text("debug") | visible(debug_mode)   // hides without removing from tree
```

When `debug_mode` is `false`, the node still exists in the element tree but
renders as zero-size.

## Using DSL Trees in Applications

DSL trees integrate the same way regardless of which application API you use —
the render side always produces an `Element` via `.build()`.

### Simple `run()`

The render closure returns an `Element` directly. Just build the tree:

```cpp
maya::run(cfg,
    [](const maya::Event& e) { /* handle input */ },
    [] {
        return v(
            t<"Hello"> | Bold,
            text("frame " + std::to_string(frame))
        ) | border_<Round> | pad<1>;
    }
);
```

### Program `run<P>()`

The `view` function receives the model and returns an `Element`. Same pattern:

```cpp
struct MyProgram {
    // ...
    static Element view(const Model& m) {
        return v(
            t<"Counter"> | Bold,
            text(m.count)
        ) | border_<Round> | pad<1>;
    }
};

maya::run<MyProgram>(cfg);
```

### `live()` and `print()`

`live()` works like simple `run()` — the render closure returns an `Element`.
`print()` renders once and exits; just call `.build()` directly:

```cpp
// live(): continuous rendering
maya::live([] {
    return v(t<"Status"> | Bold, text(current_status));
});

// print(): one-shot
maya::print(v(t<"Report"> | Bold, map(rows, row_fn)).build());
```

In all cases the DSL tree is identical — only the surrounding application
structure differs.
