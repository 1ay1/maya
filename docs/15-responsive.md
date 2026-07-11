# Responsive Layouts

A layout is *responsive* when it looks right at **every** terminal size — from a
40-column phone SSH session to a 300-column ultrawide — without you writing a
different UI for each one. maya makes that the default, not a project.

The flexbox engine ([Layout](04-layout.md)) already handles most of it: `grow`,
`gap`, `align`, and `wrap` reflow automatically as the window changes. This page
is about the harder 10% — the places where the **structure** of the UI has to
change with the size:

- A status bar that shows `host · kernel · uptime · battery · 132 procs` on a
  wide terminal but sheds down to just the app name when it's narrow.
- A process table that hides the `PORT` and `THREADS` columns before it lets the
  `NAME` column get crushed.
- A graph panel that must fill *exactly* the height left over after the meters
  above it — no more, no less, at any window height.

The old way to do all of this was **arithmetic**: count the bytes in each label,
pick breakpoints (`if (width >= 92) show_port = true;`), thread a hand-computed
row budget down from the parent. That arithmetic lives in a different place than
the thing it measures, so it drifts the moment either side changes — and it
drifts *silently*, only showing up as a ragged table at one specific width.

maya's answer is one idea in a handful of shapes: **measure, don't estimate.**

---

## The bug class this kills

Here is a responsive header written the arithmetic way. It looks reasonable:

```cpp
// The OLD way — a byte budget, hand-summed, drifting.
int need = pulse.size() + 1 + app.size() + 1 + tag.size();
std::vector<Element> items = { pulse_el, app_el, tag_el };

if (need + 2 + host.size() <= width) { items.push_back(host_el);   need += 2 + host.size(); }
if (need + 2 + kern.size() <= width) { items.push_back(kern_el);   need += 2 + kern.size(); }
items.push_back(space);
if (need + battery.size() + 12 <= width) items.push_back(battery_el);   // +12 for uptime?
// ...
```

Every one of those `+ 1`, `+ 2`, `+ 12` is a guess about how wide a *styled*
fragment renders. The moment someone adds an icon to `host_el`, or the pulse
glyph becomes two cells wide, or a gap changes, the sum is wrong — and nothing
tells you. `host.size()` counts **bytes**, but the terminal lays out **columns**;
for anything non-ASCII those already disagree.

The responsive way states the same intent declaratively and never counts a byte:

```cpp
// The NEW way — measured, self-correcting.
fit_row({
    {pulse_el},                   // essential (never drops)
    {app_el},
    {tag_el},
    {host_el,    5},              // keep-rank 5: drops late
    {kernel_el,  4},
    {Element{space}},             // grow spacer — pushes the rest right
    {battery_el, 3},
    {uptime_el,  2},
    {procs_el,   1},              // keep-rank 1: first to go
})
```

`fit_row` measures each *real* fragment, drops the lowest-ranked ones until the
rest fit, and lays out what remains. Change a label, add an icon, swap a font —
the measurement follows, because the thing measured **is** the thing rendered.

---

## The toolkit at a glance

Seven primitives, each answering one question. All are in `maya::dsl` (or
`maya::`); no extra include beyond `<maya/maya.hpp>` — except `solve_columns`,
which lives in the dependency-free header `<maya/layout/columns.hpp>`.

| Primitive | The question it answers | Kind |
|-----------|-------------------------|------|
| [`grid`](#the-grid-one-declaration-every-width) | "Lay out the whole page, **correct at every width**." | component |
| [`measure_element`](#measure_element-ask-dont-estimate) | "How wide/tall does *this* fragment actually render?" | function |
| [`fill`](#fill-size-to-the-slot) | "Make this content **fill** the space flex gives it." | component |
| [`adapt`](#adapt-restructure-by-width) | "Build a **different tree** depending on the width I get." | component |
| [`fit_row`](#fit_row-a-row-that-sheds-items) | "Lay out this row, **dropping** low-priority items when tight." | component |
| [`responsive`](#responsive-named-width-breakpoints) | "Switch whole **layouts** at named width breakpoints." | component |
| [`place`](#place-pin-content-anywhere-in-a-slot) | "**Pin** this child to a corner/edge/center of its slot." | element |
| [`solve_columns`](#solve_columns-one-plan-for-a-table) | "Give me **one** width plan a whole table shares." | function |

!!! note "Components are first-class nodes"
    Everything returning a component (`component`, `fill`, `adapt`, `fit_row`,
    `responsive`, `gradient_rule`) satisfies the `Node` concept: use them
    directly as children of `v()` / `h()` and pipe runtime modifiers onto them
    (`fill(…) | grow(1)`, `gradient_rule(a, b) | width(40)`) like any other
    node. The builder's own methods (`.grow()`, `.width(Dimension)`) also work
    and avoid the wrapper box a pipe introduces.

The mental model:

- **`grid`** is the page-level answer — the Bootstrap grid for terminal cells.
  Start here; reach for the narrower primitives when a piece needs bespoke
  behavior inside a cell.
- **`measure_element`** is the primitive everything else is built on — it runs a
  real layout pass and hands back the natural size.
- **`fill`** responds to the size it is *given* (height and width).
- **`adapt`** and **`fit_row`** respond to the *width* they are given by choosing
  a different structure.
- **`solve_columns`** is pure arithmetic for the one case flex can't express: a
  grid whose header row and body rows must agree on column widths.

---

## The grid — one declaration, every width

```cpp
enum class Bk { XS, SM, MD, LG, XL, XXL };
struct Breaks { int sm = 60, md = 90, lg = 120, xl = 160, xxl = 200; };
struct GridOpts { int columns = 12; int gap_x = 1; int gap_y = 0;
                  bool grow_rows = false; Breaks breaks{}; };

GridCol col(Element el);                       // .span/.xs/.sm/.md/.lg/.xl/.xxl/.order
auto grid(std::vector<GridCol> cols, GridOpts opts = {}) -> ComponentBuilder;
```

The hand-rolled tier switch — `if (w >= 200) wide(); else if (w >= 84)
classic(); else narrow();` with three separately-maintained layouts — is the
last big piece of width arithmetic left in a typical TUI. The grid kills it
the way Bootstrap killed it on the web: **one declaration** where each column
states its width *per breakpoint*, and the framework re-solves the packing at
every resize.

A system dashboard — the full rockbottom shape — in its entirety:

```cpp
// The stat panels: full width on a phone, 2×2 from md up.
auto stats = grid({
    col(cpu_panel).md(6),
    col(mem_panel).md(6),
    col(net_panel).md(6),
    col(disk_panel).md(6),
});

// The page: stats above the table — until xxl, where they become a
// fixed 42-cell sidebar and the process table takes everything else.
auto body = grid({
    col(stats).xxl(Columns{42}),
    col(proc_table),
});
```

That is the whole layout. At `< 90` columns everything stacks; at `≥ 90` the
panels pair up above the full-width table; at `≥ 200` the stats compress into
a sidebar. No widths computed, no tiers switched, nothing to drift.

### Mobile-first spans

Every column defaults to **span 12** — full width — so an undecorated grid
stacks everything in one column on the narrowest terminal, automatically. A
value set at a tier applies to that tier **and every tier above it** until
overridden, exactly like Bootstrap's `col-12 col-md-6 col-xl-3`:

```cpp
col(panel)               // 12 everywhere: always full width
col(panel).md(6)         // 12 below md, half from md up
col(panel).md(6).xl(3)   // …and a quarter from xl up
```

Spans are grid units out of `GridOpts::columns` (default 12). Columns pack
greedily into rows in declaration order; a column that would overflow the
row's 12 units starts a new row. When a row's spans sum to 12 its widths
solve **exactly** — largest-remainder distribution, no ragged right edge from
integer division.

### Fixed-cell columns — the TUI `col-auto`

Terminals have a pattern the web grid never needed: the **fixed-width
sidebar** (a 40-cell stats rail, a 3-cell gutter). Pass `Columns{n}` instead
of a span:

```cpp
col(sidebar).xxl(Columns{42})    // spans below xxl, exactly 42 cells at xxl+
col(rail).span(Columns{3})       // fixed 3 cells at every tier
```

Fixed columns consume no grid units — the row's span columns share whatever
width remains after the fixed ones (and the gaps) are taken off the top.

### Hiding and ordering

```cpp
col(debug_pane).span(0).lg(12)   // hidden until lg, full-width row from lg
col(cpu).span(0).md(4)           // Bootstrap d-none d-md-block
col(proc_table).order(-1)        // pack first at every tier
```

Span `0` (or `Columns{0}`) hides the column at that tier and above until a
higher tier resets it. `.order(n)` stable-sorts columns before packing — put
the table first on a phone and third on an ultrawide.

### Slot-width tiers — grids nest and re-solve independently

Breakpoints key on the width of the **slot the grid sits in**, not the
terminal. That is what makes the dashboard above work: on a 230-column
terminal the outer grid is at `xxl`, but the stats grid inside the 42-cell
sidebar sees 42 columns — below `md` — so its panels restack 1-across *inside
the sidebar* while the rest of the screen stays wide. Composition is free;
nothing needs to know where it will be mounted.

### Heights

Rows are natural-height; every cell in a row is stretched to the tallest
(flex cross-stretch). Two knobs for full-screen dashboards:

- `GridOpts{.grow_rows = true}` — rows share surplus height equally when the
  grid fills a definite-height slot.
- A cell whose content should fill its cell vertically uses the usual grow
  chain: `col(proc_table | grow(1))` inside a definite band.

See [`examples/grid.cpp`](https://github.com/1ay1/maya/blob/master/examples/grid.cpp)
for the complete dashboard — resize it and watch all three shapes.

---

## `measure_element` — ask, don't estimate

```cpp
[[nodiscard]] Size measure_element(const Element& elem,
                                   int max_width,
                                   int max_height = 1 << 20);
```

Runs the **same layout engine the renderer uses** over `elem` and returns the
size it would occupy inside `max_width × max_height`. This is the primitive that
makes hand-written width arithmetic obsolete — never estimate `2 + host.size()`
cells for a styled fragment again. Build the fragment and ask:

```cpp
Element chip = h(icon, text(" "), text(hostname)) | Bold | fgc(theme.accent);

Size s = measure_element(chip, /*max_width=*/1 << 14);
int columns = s.width.value;   // the REAL rendered width, gaps/icons/wide-chars and all
int rows    = s.height.value;
```

Because the measurement and the eventual paint share one source of truth, the
whole family of bytes-vs-columns bugs, forgotten-gap bugs, and drifting-estimate
bugs dies here.

`max_width` bounds wrapping: pass a large value (`1 << 14`) to get the natural
one-line width, or a real width to learn how many rows the fragment wraps to
there. Cost is one layout pass over the fragment — trivial for the small pieces
(header segments, table cells) this is for. Fine to call every frame.

!!! tip "You rarely call this directly"
    `fit_row` and `adapt` measure for you. Reach for `measure_element` when you
    need a raw number — e.g. to size a sibling, decide a breakpoint by hand, or
    right-align something to a measured column.

---

## `fill` — size to the slot

```cpp
auto fill(std::function<Element(int w, int h)> render_fn,
          int min_w = 0, int min_h = 1) -> ComponentBuilder;
```

`component()` sizes to its **content** — its natural height is whatever its
render produces. `fill()` is the inverse: it sizes to its **slot** — it grows to
consume the space its flex container gives it, and its callback receives the
**real allocated `(w, h)`** at paint time. Size your graph, canvas, or gauge to
`h` and it always fits the box exactly.

This is the height-responsive counterpart to everything else on this page. No
hand-computed row budget threaded down from the parent, so the estimate can
never drift from what the layout engine actually allocated.

```cpp
// A graph that always fills the space left after the meters below it:
v(
    fill([data](int w, int h) {          // fills the slack
        if (h < 2) return blank().build();   // collapse gracefully when tiny
        return area_chart(*data, w, h);
    }, /*min_w=*/0, /*min_h=*/2),
    meter_row(),                         // natural height
    meter_row()
) | height(N)                            // definite parent → fill grows into the slack
```

### Why it needs to be a primitive

A plain `component().grow(1)` with no measure gets auto-measured by rendering at
an *unbounded* height and counting rows — a fill render emits ~2²⁰ rows there and
reports a nonsense basis, so `grow` can never expand it. `fill()` installs a
`measure` that reports a small fixed minimum (`min_w × min_h`) instead, giving
`grow` a finite basis to expand from, and sets `grow(1)` so it claims the free
space.

### The one requirement

The container must be **definite on the fill axis** for `grow` to distribute —
same rule as any grow child. That means either:

- an explicit `height()` / `width()` on an ancestor, or
- **cross-axis stretch** inherited from a definite-size parent (a row with a
  definite height stretches its auto-height children to that height, which makes
  *their* children's column definite — the stretch chains down).

An auto-sized container has no free space to hand out, so a `fill` inside it just
renders at `min_h`. When the slot is legitimately too small, the callback simply
receives a small `h` (down to `min_h`) — collapse gracefully there (e.g. return
`blank()` below a floor, as above).

---

## `adapt` — restructure by width

```cpp
auto adapt(std::function<Element(int w)> render_fn) -> ComponentBuilder;
```

The width-responsive counterpart to `fill`. Where `fill` is for content that
*sizes itself* to the slot (graphs, canvases), `adapt` is for content whose
*structure* depends on the slot — drop a column below 60 cells, collapse labels,
switch a whole layout. The callback receives the **real allocated width** at
paint time and returns the tree for exactly that width:

```cpp
adapt([=](int w) {
    if (w >= 100) return three_column_layout();
    if (w >= 60)  return two_column_layout();
    return single_column_layout();
})
```

Natural height is auto-measured by the framework from what the callback returns —
and because measurement literally *runs the callback*, measure and paint can
never disagree. There is no separate "how tall will the narrow layout be?"
estimate to get wrong.

!!! note "Prefer `fit_row` for the common case"
    The most frequent use of `adapt` is "drop trailing items when the row gets
    tight." `fit_row` is that, pre-built and measured for you — reach for raw
    `adapt` only when you're switching between genuinely different structures.

---

## `responsive` — named width breakpoints

```cpp
struct Bp {
    int min_width = 0;
    std::function<Element(int w)> build;
};

auto responsive(std::vector<Bp> tiers) -> ComponentBuilder;
```

Tier sugar over `adapt` for the "phone / laptop / ultrawide" case: list your
breakpoints once, and the **widest tier whose `min_width` the slot satisfies**
builds the view. The builder still receives the real width, so tiers can fine-
tune within their band:

```cpp
responsive({
    { 0,   [](int)   { return compact_view(); } },   // < 80 cols
    { 80,  [](int)   { return two_pane(); } },        // 80..119
    { 120, [](int w) { return three_pane(w); } },     // >= 120
})
```

If the slot is narrower than every tier, the smallest tier is used — there is
always something to draw. Unlike a global "screen size" switch, `responsive`
keys on the width of the **slot it sits in**, so a sidebar can collapse to its
compact tier while the main pane stays wide.

---

## `place` — pin content anywhere in a slot

```cpp
enum class HAlign { Left, Center, Right };
enum class VAlign { Top, Middle, Bottom };

Element place(Element child,
              HAlign h = HAlign::Center,
              VAlign v = VAlign::Middle);
```

The declarative answer to "center this", "pin it bottom-right". `place()`
fills the space its parent allocates and positions the child on both axes:

```cpp
place(spinner)                                   // dead center (default)
place(hint, HAlign::Right, VAlign::Bottom)       // status corner
place(logo, HAlign::Center, VAlign::Top)
```

Because it *fills* (like a grow child), it tracks every resize with zero
arithmetic — the child just stays put at its corner / edge / center of an
ever-changing box. Same definiteness rule as `fill`: the slot must come from
an explicit ancestor size or the default cross-stretch.

---

## `fit_row` — a row that sheds items

```cpp
struct FitItem {
    Element el;
    int     keep = kKeepAlways;   // importance; lower ranks drop first
};

auto fit_row(std::vector<FitItem> items, int gap = 0) -> ComponentBuilder;
```

The declarative kill for the "responsive header" bug class. You list the row's
items once, tag the optional ones with a `keep` rank, and the row **re-solves
itself at every width**: items are dropped **lowest-`keep` first** (ties drop the
rightmost) until what remains fits. Widths come from `measure_element` over the
real styled fragments — no hand-summed cell estimates to drift.

```cpp
fit_row({
    {logo},                        // kKeepAlways (default) — essential
    {hostname_chip, 5},
    {kernel_chip,   4},
    {Element{space}},              // grow spacer: measures 0, always kept
    {battery_chip,  3},
    {uptime_chip,   2},
    {proc_counts,   1},            // first to go
})
```

As the terminal narrows, this sheds `proc_counts` first, then `uptime`, then
`battery`, then `kernel`, then `hostname` — and never the logo. Widen it and they
come back in reverse. All from one declaration.

### Rules

- **An item is atomic.** Group an icon + label + value cluster into a single
  `Element` (`h(...)`) so it appears and disappears as a unit.
- **`gap`** inserts uniform spacing between *kept* items only. Alternatively, bake
  a leading separator into each item so the separator vanishes with it.
- **Grow spacers** (`dsl::space`) measure 0 wide and still expand at layout, so
  your left/right clusters keep working — put a `{Element{space}}` where you want
  the row to split.
- **Essentials never drop.** If even the `kKeepAlways` items overflow, they are
  all emitted and the row overflows — downstream clip/shrink deals with it.
  `fit_row` will not sacrifice an essential to make a nice-to-have fit.

### `keep` ranks

`keep` is just an `int` priority. Higher survives longer.

| Value | Meaning |
|-------|---------|
| `kKeepAlways` (default) | Never dropped. The row overflows before this goes. |
| any lower `int` | Dropped in ascending order; ties drop the **rightmost**. |

Pick ranks that read like priorities — `1` = "first to go", larger = "more
important." You don't need them contiguous.

---

## `solve_columns` — one plan for a table

```cpp
#include <maya/layout/columns.hpp>

struct ColSpec {
    int   min    = 1;             // minimum content width (cells)
    int   max    = 0;             // growth cap; 0 = unbounded (weight > 0 only)
    float weight = 0.0f;          // share of surplus; 0 = fixed at min
    int   keep   = kKeepAlways;   // drop order: LOWER dropped first
};

struct ColPlan {
    std::vector<int> width;       // solved widths; 0 = dropped
    int gap = 0;

    bool has(size_t i) const;     // is column i visible?
    int  at(size_t i) const;      // solved width (0 when dropped)
    int  used() const;            // total cells: visible widths + gaps
};

ColPlan solve_columns(std::span<const ColSpec>, int avail, int gap = 1);
```

`fit_row` is for a **row of independent chunks**. A **table** is different: its
header row and its body rows are separate code paths that must nonetheless agree
on where every column starts. The classic bug is exactly this desync — someone
edits "show `PORT` when `w >= 92`" in the row renderer but not the header, and the
header drifts off the value rail at one specific width.

`solve_columns` makes that desync **structurally impossible**. Describe every
column *once* as a `ColSpec`, solve the set against the real width *once* per
frame, and render the header and every row from the same `ColPlan`:

```cpp
using namespace maya;
enum Col { PID, NAME, PORT, CPU, MEM, N };

std::array<ColSpec, N> spec{{
    {.min = 8},                              // PID   — fixed, essential
    {.min = 8, .weight = 3},                 // NAME  — takes the slack
    {.min = 9, .keep = 1},                   // PORT  — first to drop
    {.min = 6},                              // CPU   — fixed, essential
    {.min = 8, .keep = 5},                   // MEM   — drops late
}};

ColPlan plan = solve_columns(spec, view.width, /*gap=*/1);

// header AND every row read the SAME plan:
auto cell = [&](Col c, Element e) {
    return plan.has(c) ? (e | width(plan.at(c))) : nothing();
};
```

### How it solves

1. **Drop phase.** If the visible minimums don't fit, drop columns lowest-`keep`
   first (ties → rightmost) until they do. `kKeepAlways` columns never drop — if
   only essentials remain and they still overflow, they're kept and the row
   overflows (downstream flex-shrink / clipping handles it).
2. **Waterfill phase.** Distribute the surplus to `weight > 0` columns in
   proportion to weight, clamped at `max` (`0` = unbounded). A round-robin pass
   mops up integer-rounding residue. With at least one unbounded weighted column
   the plan fills `avail` **exactly**, so right-aligned rails stay pinned.

A dropped column solves to width `0` → `has()` is false; emit nothing for it
(including its gap) and every consumer stays aligned.

### Continuous over binary

Notice `NAME` has `weight` instead of a breakpoint. Old tables toggled a column
between two fixed widths at a threshold — a visible *cliff* as you resize. A
weighted column **breathes**: it grows one cell at a time as the window widens,
so there is no snap. Reserve `keep`/drop for columns that genuinely have to
vanish; use `weight` for the ones that should just flex.

!!! tip "Scrollbar gutters"
    When a body is scrolled, its rows are a couple cells narrower than the header
    (the scrollbar lives in that gutter). Model the gutter as a trailing
    zero-or-N column (`{.min = gutter_w}`) so the *one* shared plan stays correct
    for both header and body — no second code path.

`solve_columns` is pure arithmetic — no `Element` types, no layout engine — so
it's cheap enough to solve every frame and trivial to unit-test in isolation.

---

## Choosing a primitive

```text
A whole page correct at every width?         → grid
Need a raw size number?                      → measure_element
Content should FILL the space it's given?    → fill
Child should be PINNED in its slot?          → place
Structure changes with the WIDTH it's given? → adapt
  …named phone/laptop/ultrawide tiers?       → responsive
  …"drop trailing items when tight"?         → fit_row
A header + rows that must share columns?     → solve_columns
```

They compose freely. A real panel might use all of them: a `fit_row` title bar, a
`solve_columns` table body, and a `fill` graph footer — each solving its own axis,
none of them counting a byte.

---

## A worked example: a system-monitor panel

This is the shape [rockbottom](https://github.com/1ay1/bottom) (built on maya)
uses. One column, three stacked panels, at any terminal height:

```cpp
// The container is given a definite height (band_h) by its parent row.
v(
    // Title bar sheds detail as the panel narrows.
    fit_row({
        {t<"CPU">, kKeepAlways},
        {Element{space}},
        {load_avg_chip, 3},
        {core_count_chip, 1},
    }) | pad<0, 1>,

    // The graph fills whatever height is left after the meters.
    fill([samples](int w, int h) {
        if (h < 2) return blank().build();
        return cpu_area_chart(*samples, w, h);
    }, 0, 2) | grow(1),

    // Per-core meters — natural height, drawn below the graph.
    core_meter_rows()
) | border_<Round> | height(band_h);
```

- The **title** never has to know how wide the panel is — `fit_row` measures.
- The **graph** never has to know how tall the panel is — `fill` receives `h`.
- The panel has a definite `height`, so the `fill`'s `grow(1)` has real slack to
  claim (the meters and title take their natural rows; the graph takes the rest).

Resize the terminal to any width and any height and every piece re-solves. There
is not a single hand-computed breakpoint or row budget in it.

---

## See also

- [Layout](04-layout.md) — the flexbox model these primitives build on
  (`grow`, `gap`, `align`, `wrap`, `Dimension`).
- [Styling › Gradients](03-styling.md#gradients) — `gradient()`, `rainbow()`,
  and `gradient_rule()`, the pretty counterpart to this toolkit (a
  `gradient_rule` is itself responsive: it re-tiles to its slot width).
- [Runtime Content](05-runtime-content.md) — `component()`, `dyn()`, and the
  measure-aware render callbacks these extend.
- [API Reference › Responsive layout](11-api-reference.md#responsive-layout) —
  the exact signatures.
