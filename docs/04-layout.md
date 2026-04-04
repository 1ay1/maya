# Layout

maya uses a **flexbox layout engine** — the same model as CSS Flexbox and React
Native. Every `BoxElement` (created by `v()`, `h()`, or the `BoxBuilder`) is a
flex container. Children are laid out along a main axis with configurable
alignment, spacing, sizing, and wrapping.

## Direction

The main axis determines how children are stacked:

```cpp
v(child1, child2, child3)    // Column: top to bottom (↓)
h(child1, child2, child3)    // Row: left to right (→)
```

Under the hood, these correspond to `FlexDirection::Column` and
`FlexDirection::Row`. There are also reverse variants available through the
runtime `BoxBuilder`:

| Direction | Flow | DSL |
|-----------|------|-----|
| `Column` | Top → Bottom | `v(...)` |
| `Row` | Left → Right | `h(...)` |
| `ColumnReverse` | Bottom → Top | (BoxBuilder only) |
| `RowReverse` | Right → Left | (BoxBuilder only) |

## Padding

Padding adds space **inside** the box, between the border and the content:

```cpp
v(...) | pad<1>              // 1 cell on all four sides
v(...) | pad<1, 2>           // 1 vertical (top/bottom), 2 horizontal (left/right)
v(...) | pad<1, 2, 3, 4>    // top=1, right=2, bottom=3, left=4
v(...) | pad<0, 1>           // 0 vertical, 1 horizontal (common for tight borders)
```

Compile-time validated — negative values produce a static_assert failure.

## Gap

Gap adds space **between** children (not before the first or after the last):

```cpp
v(t<"A">, t<"B">, t<"C">) | gap_<1>
// Renders as:
//   A
//           ← 1 cell gap
//   B
//           ← 1 cell gap
//   C
```

Gap works in both directions — vertical gap for `v()`, horizontal gap for `h()`.

## Grow

Grow controls how children share remaining space after fixed-size items are
placed. A child's share is proportional to its grow factor:

```cpp
h(
    v(t<"Sidebar">) | grow_<1>,    // Gets 1/4 of space
    v(t<"Main">)    | grow_<3>     // Gets 3/4 of space
)
```

Without `grow_`, children only take their natural width/height. With `grow_`,
they expand to fill all available space.

```cpp
// Push items apart:
h(t<"Left">, space, t<"Right">)
// `space` is SpacerNode with grow=1, pushing "Right" to the far edge
```

## Borders

Borders draw a frame around the box. The border line is 1 cell wide on each
active side.

### Border Styles

```cpp
v(...) | border_<Round>     // ╭──────╮
                            // │      │
                            // ╰──────╯

v(...) | border_<Single>    // ┌──────┐
                            // │      │
                            // └──────┘

v(...) | border_<Double>    // ╔══════╗
                            // ║      ║
                            // ╚══════╝

v(...) | border_<Thick>     // ┏━━━━━━┓
                            // ┃      ┃
                            // ┗━━━━━━┛
```

Additional styles available via the runtime `BoxBuilder`:

| Style | Characters | Description |
|-------|------------|-------------|
| `Single` | `┌ ─ ┐ │` | Standard single line |
| `Double` | `╔ ═ ╗ ║` | Double line |
| `Round` | `╭ ─ ╮ │` | Rounded corners |
| `Bold` | `┏ ━ ┓ ┃` | Thick/bold line |
| `Classic` | `+ - \| +` | ASCII-only |
| `Arrow` | Arrow characters | Directional |

### Border Color (Type-State Safety)

Border color can only be set **after** a border style — enforced at compile time:

```cpp
v(...) | border_<Round> | bcol<60, 65, 80>    // OK
v(...) | bcol<60, 65, 80>                      // COMPILE ERROR
```

This is type-state safety: the `bcol` operator has a `requires (Cfg.has_border)`
constraint that only matches BoxNodes that already have a border configured.

### Border Title (Runtime API)

Border titles are available through the `BoxBuilder`:

```cpp
dyn([&] {
    return vstack()
        .border(BorderStyle::Round)
        .border_color(Color::rgb(60, 65, 80))
        .border_text("Panel Title", BorderTextPos::Top)
        .padding(0, 1, 0, 1)(
            text("content")
        );
})
```

Title positioning:

| Position | Placement |
|----------|-----------|
| `BorderTextPos::Top` | In the top border line |
| `BorderTextPos::Bottom` | In the bottom border line |

Title alignment:

| Alignment | Effect |
|-----------|--------|
| `BorderTextAlign::Start` | Left-aligned (default) |
| `BorderTextAlign::Center` | Centered |
| `BorderTextAlign::End` | Right-aligned |

### Border Sides

Control which sides have borders:

```cpp
// Runtime BoxBuilder only:
box()
    .border(BorderStyle::Single)
    .border_sides(BorderSides::horizontal())  // Top and bottom only
    // ...

box()
    .border(BorderStyle::Single)
    .border_sides(BorderSides{.top = true, .bottom = true})
    // ...
```

| Factory | Active Sides |
|---------|-------------|
| `BorderSides::all()` | Top, Right, Bottom, Left |
| `BorderSides::none()` | None |
| `BorderSides::horizontal()` | Top and Bottom |
| `BorderSides::vertical()` | Left and Right |

## Alignment

### Align Items (Cross-Axis)

Controls how children are positioned on the cross axis:

```cpp
// In a row (h), cross axis is vertical:
h(small, tall) // align_items controls vertical alignment

// In a column (v), cross axis is horizontal:
v(narrow, wide) // align_items controls horizontal alignment
```

Available via the runtime `BoxBuilder`:

```cpp
hstack().align_items(Align::Center)(child1, child2)
vstack().align_items(Align::Stretch)(child1, child2)
```

| Value | Effect |
|-------|--------|
| `Align::Start` | Pack to start of cross axis |
| `Align::Center` | Center on cross axis |
| `Align::End` | Pack to end of cross axis |
| `Align::Stretch` | Stretch to fill cross axis |
| `Align::Baseline` | Align by text baseline |

### Justify Content (Main Axis)

Controls spacing along the main axis:

```cpp
hstack().justify(Justify::SpaceBetween)(a, b, c)
// a          b          c
//  ↑ equal space between ↑
```

| Value | Effect |
|-------|--------|
| `Justify::Start` | Pack to start |
| `Justify::Center` | Center items |
| `Justify::End` | Pack to end |
| `Justify::SpaceBetween` | Even space between items |
| `Justify::SpaceAround` | Even space around items |
| `Justify::SpaceEvenly` | Even space including edges |

## Size Constraints

The `BoxBuilder` supports explicit sizing:

```cpp
box()
    .width(Dimension::fixed(40))       // Exactly 40 columns
    .height(Dimension::percent(50))     // 50% of parent height
    .min_width(Dimension::fixed(20))    // At least 20 columns
    .max_height(Dimension::fixed(10))   // At most 10 rows
```

### Dimension Types

```cpp
Dimension::auto_()       // Size to content (default)
Dimension::fixed(40)     // Exactly 40 cells
Dimension::percent(50)   // 50% of parent's size
50_pct                   // User-defined literal for percent
```

## Overflow

Controls what happens when children exceed the container's bounds:

```cpp
box().overflow(Overflow::Hidden)(children...)
```

| Value | Effect |
|-------|--------|
| `Overflow::Visible` | Content flows past edges (default) |
| `Overflow::Hidden` | Content is clipped at edges |

`Overflow::Hidden` pushes a clip rectangle onto the canvas — child content
outside the box's bounds is not drawn.

## Flex Shrink and Basis

### Shrink

When children overflow the container, `shrink` controls how much each child
contracts. Default is 1 (all children shrink equally):

```cpp
box().shrink(0)(child)    // This child won't shrink
box().shrink(2)(child)    // This child shrinks twice as fast
```

### Basis

The initial size before grow/shrink is applied:

```cpp
box().basis(Dimension::fixed(30))(child)  // Start at 30 cells
```

## Margin

Margin adds space **outside** the box:

```cpp
// Runtime BoxBuilder only:
box().margin(1)(child)           // 1 cell on all sides
box().margin(1, 2)(child)        // 1 vertical, 2 horizontal
box().margin(1, 2, 3, 4)(child)  // top, right, bottom, left
```

Margin creates space between sibling elements. Unlike padding (inside the
border), margin is outside.

## Wrap

When children exceed the container's main axis, wrapping flows them to the next
line:

```cpp
hstack().wrap(FlexWrap::Wrap)(many, children, here, ...)
```

| Value | Effect |
|-------|--------|
| `FlexWrap::NoWrap` | Single line, overflow (default) |
| `FlexWrap::Wrap` | Wrap to next line |
| `FlexWrap::WrapReverse` | Wrap in reverse direction |

## Layout Shortcuts

The DSL namespace provides pre-configured runtime builders for containers
that need runtime-configured borders, colors, or titles:

```cpp
vstack()   // BoxBuilder with direction=Column
hstack()   // BoxBuilder with direction=Row
center()   // BoxBuilder with justify=Center, align_items=Center, grow=1
```

## Common Layout Patterns

### Sidebar + Main

```cpp
h(
    v(t<"Nav">, t<"Home">, t<"About">) | border_<Single> | grow_<1>,
    v(t<"Content">) | grow_<3>
)
```

### Header / Body / Footer

```cpp
v(
    h(t<"App Title"> | Bold, space, t<"v1.0"> | Dim),
    v(t<"Main content">) | grow_<1>,
    h(t<"Status: OK"> | Dim)
) | pad<1>
```

### Centered content

```cpp
// Using the center() builder:
dyn([&] {
    return center()(
        text("Loading...", Style{}.with_bold())
    );
})
```

### Card

```cpp
v(
    t<"Card Title"> | Bold,
    t<"">,
    t<"Card body text goes here."> | Dim
) | border_<Round> | bcol<60, 65, 80> | pad<1>
```

### Equal-width columns

```cpp
h(
    v(t<"Col 1">) | grow_<1>,
    v(t<"Col 2">) | grow_<1>,
    v(t<"Col 3">) | grow_<1>
)
```
