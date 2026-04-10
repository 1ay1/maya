# Runtime Content

maya's DSL is compile-time by default, but real applications need dynamic data —
values that change per frame, user input, API responses, computed strings. maya
provides several mechanisms to inject runtime content into the compile-time tree.

## text() — Runtime Text Node

`text()` creates a `RuntimeTextNode<S>` — a proper DSL node that accepts pipe
styling, just like compile-time `t<"...">`:

```cpp
text("hello")                          // RuntimeTextNode<string_view>
text(std::to_string(count))            // RuntimeTextNode<string>
text(42)                               // RuntimeTextNode<int>
text(3.14)                             // RuntimeTextNode<double>
text("Error!") | Bold | Fg<255,80,80>  // Styled — pipe works!
```

### Signatures

```cpp
template <typename S>
auto text(S&& content, Style s = {}) -> RuntimeTextNode<decay_t<S>>;

template <typename S>
auto text(S&& content, Style s, TextWrap w) -> RuntimeTextNode<decay_t<S>>;
```

### Numeric Content

`text()` handles integers and floats directly — no `std::to_string()` needed:

```cpp
text(42)         // Renders "42"
text(3.14)       // Renders "3.14" (2 decimal places)
text(-7)         // Renders "-7"
```

Internally it uses `std::to_chars()` for zero-allocation formatting.

### Text Wrapping

Control how text wraps when it exceeds the container width:

```cpp
text("Long text...", Style{}, TextWrap::Wrap)           // Word wrap (default)
text("Long text...", Style{}, TextWrap::TruncateEnd)    // "Long te..."
text("Long text...", Style{}, TextWrap::TruncateMiddle) // "Lon...xt"
text("Long text...", Style{}, TextWrap::TruncateStart)  // "...g text"
text("Long text...", Style{}, TextWrap::NoWrap)          // Overflow
```

### Piping Styles

`RuntimeTextNode` supports the same `|` pipe operator as `TextNode`:

```cpp
text("OK")    | Bold | Fg<80, 220, 120>    // Compile-time style tags
text("Error") | Bold | Fg<255, 80, 80>
text(count)   | Dim
```

The pipe merges the tag's `CTStyle` into the node's runtime `Style`:

```cpp
template <typename S, CTStyle V>
auto operator|(RuntimeTextNode<S> n, StyTag<V>) {
    n.style = n.style.merge(V.runtime());
    return n;
}
```

### Runtime Styles

For fully dynamic styling (colors computed at runtime), pass a `Style` object:

```cpp
auto color = is_ok ? Color::rgb(80, 220, 120) : Color::rgb(255, 80, 80);
text("Status", Style{}.with_bold().with_fg(color))
```

## dyn() — Runtime Escape Hatch

`dyn()` wraps a lambda that returns an `Element`. It's the general-purpose
escape from the compile-time tree into runtime logic:

```cpp
dyn([&] { return text("count = " + std::to_string(n)); })
```

### When to Use dyn()

Use `dyn()` when you need:

- **Conditional rendering** — show different elements based on state
- **Access to runtime variables** — capture by reference from the enclosing scope
- **Complex element construction** — build trees with runtime BoxBuilder
- **Theme-aware styling** — access `ctx.theme` colors

```cpp
// Conditional rendering
dyn([&] {
    if (loading) return text("Loading...") | Dim;
    return text("Ready") | Bold | Fg<80, 220, 120>;
})

// Complex runtime construction (vstack/hstack available from maya::dsl)
dyn([&] {
    return vstack()
        .border(BorderStyle::Round)
        .border_color(theme_border())
        .border_text("CPU", BorderTextPos::Top)
        .padding(0, 1, 0, 1)(
            text(sparkline(cpu_hist, width), Style{}.with_fg(accent)),
            text(fmt("%4.1f%%", cpu_avg), Style{}.with_bold().with_fg(gauge_color(cpu_avg)))
        );
})
```

### dyn() Returns a DynNode

```cpp
template <typename F>
struct DynNode {
    F fn;
    Element build() const { return fn(); }
};
```

The lambda is called during `.build()`. It must return something convertible to
`Element` — any node type works since they all have `operator Element()`.

### dyn() Inside Compile-Time Trees

The power is mixing `dyn()` with static DSL nodes:

```cpp
auto ui = v(
    t<"Dashboard"> | Bold | Fg<100, 180, 255>,   // compile-time
    sep,                                            // compile-time
    dyn([&] { return text(uptime_str); }),          // runtime
    dyn([&] { return text(cpu_text, cpu_style); }), // runtime
    space,                                          // compile-time
    t<"[q] quit"> | Dim                             // compile-time
) | border_<Round> | pad<1>;
```

## map() — Range Projection

`map()` projects a range of values into a list of elements:

```cpp
std::vector<std::string> items = {"alpha", "beta", "gamma"};

map(items, [](const auto& s) { return text(s) | Dim; })
```

### Signature

```cpp
template <std::ranges::range R, typename Proj>
auto map(R&& range, Proj&& proj) -> MapNode<decay_t<R>, decay_t<Proj>>;
```

### The Projection Function

The projection receives each element of the range and returns a Node or Element:

```cpp
// Return a styled RuntimeTextNode (it's a Node)
map(names, [](const auto& name) { return text(name) | Bold; })

// Return a compound element via DSL
map(items, [](const auto& item) {
    return h(
        text(item.label) | Dim,
        text(item.value) | Bold
    );
})

// Return a raw Element
map(items, [](const auto& item) {
    return hstack()(text(item.name), text(item.value));
})
```

### map() With Indices

Use `std::views` for indexed iteration:

```cpp
auto indexed = items | std::views::enumerate;
map(indexed, [](const auto& pair) {
    auto [i, item] = pair;
    return h(text(std::to_string(i) + ". "), text(item));
});
```

### Inside a Layout

```cpp
auto list = v(
    t<"Users:"> | Bold,
    map(users, [](const User& u) {
        return h(
            text(u.name) | Fg<100, 180, 255>,
            space,
            text(u.status) | Dim
        );
    })
) | border_<Round> | pad<1>;
```

## Dynamic Child Lists — vector\<Element\> in v()/h()

`v()` and `h()` accept `std::vector<Element>` (or any `ElementRange`) directly
as children. The vector's elements are flattened into the container — no wrapper
needed:

```cpp
std::vector<Element> rows;
for (const auto& item : data) {
    rows.push_back(text(item.name) | Bold);
}

// Vector flattened alongside static nodes:
auto ui = v(
    t<"Items"> | Bold,
    rows,                  // each Element becomes a child
    t<"---"> | Dim
) | border_<Round>;
```

### Implicit Conversion — All Nodes are Elements

Every DSL node type has `operator Element()`, so you can freely push DSL nodes
into a `vector<Element>`:

```cpp
std::vector<Element> panels;
panels.push_back(t<"Static text">);
panels.push_back(text("Dynamic") | Bold);
panels.push_back(h(t<"A">, t<"B">) | border_<Single>);
panels.push_back(v(inner_rows) | pad<1>);

auto grid = h(panels);
```

### When to Use What

| Pattern | When to use |
|---------|-------------|
| `v(node1, node2)` | Fixed children known at compile time |
| `v(vector)` | Dynamic child list built at runtime |
| `v(node1, vector, node2)` | Mix of static header/footer with dynamic body |
| `map(range, proj)` | Transform a data collection into elements |
| `vstack().border(...)(...) ` | Runtime border colors, titles, or advanced layout |

## Mixing Static and Dynamic Patterns

All patterns in this section work in both `run()` and `run<P>()`. In simple
`run()`, these trees are returned from the render closure. In Program apps,
they are returned from `view(const Model&)`. The mixing of static and dynamic
nodes is the same either way.

### Pattern: Static Shell + Dynamic Body

```cpp
auto ui = v(
    t<"App Title"> | Bold | Fg<100, 180, 255>,    // static
    sep,                                             // static
    dyn([&] { return build_body(); }),               // dynamic body
    space,                                           // static
    t<"[q]quit [r]refresh"> | Dim                    // static
) | border_<Round> | pad<1>;
```

### Pattern: Dynamic Data Table

```cpp
auto table = v(
    t<"PID     USER     CPU%  MEM%"> | Bold,
    sep,
    map(processes, [](const Proc& p) {
        return h(
            text(std::to_string(p.pid)),
            text(p.user),
            text(std::to_string(p.cpu), Style{}.with_fg(gauge_color(p.cpu))),
            text(std::to_string(p.mem))
        );
    })
) | border_<Round> | pad<0, 1>;
```

### Pattern: Conditional Panels

```cpp
auto ui = v(
    t<"Status"> | Bold,
    dyn([&] -> Element {
        if (state == State::Loading) {
            return v(
                text(spinner(frame)) | Bold,
                text("Please wait...") | Dim
            );  // implicit conversion via operator Element()
        }
        return v(
            text("Complete") | Bold | Fg<80, 220, 120>,
            text(result_message)
        );
    })
) | border_<Round> | pad<1>;
```

### Pattern: Live Sparklines

```cpp
dyn([&] {
    std::string spark;
    for (float v : history)
        spark += block_char(v);  // Unicode block elements
    return text(spark, Style{}.with_fg(Color::rgb(80, 200, 255)));
})
```

## Performance Notes

- **Compile-time nodes** (`t<>`, `v()`, `h()`, style pipes): zero runtime
  overhead. Template parameters encode everything; `.build()` is a direct
  construction.

- **text()**: One heap allocation for the string content (or zero if the source
  is a `string_view`). Style is a small value type.

- **dyn()**: Lambda call + whatever the lambda does. Keep lambdas lightweight —
  avoid building huge trees inside a single `dyn()`.

- **map()**: Allocates a `vector<Element>` sized to the range. If the range is
  `sized_range`, it reserves upfront. The projection is called once per element.

- **Rebuild per frame**: In continuous rendering (`fps > 0`), the entire tree
  is rebuilt each frame — this applies to both simple `run()` and `run<P>()`.
  maya's diff engine ensures only changed cells are written to the terminal.
  Don't worry about rebuilding — it's designed for it.
