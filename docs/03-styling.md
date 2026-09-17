# Styling

maya has a layered styling system: compile-time style tags in the DSL,
runtime `Style` objects for dynamic styling, `Color` for precise color control,
and `Theme` for consistent palettes across your app.

## Compile-Time Style Tags (DSL)

In the DSL, styles are applied with the `|` pipe operator using zero-size tag
types. These compose at compile time with zero runtime cost:

```cpp
t<"Hello"> | Bold | Fg<100, 180, 255>
t<"Warning"> | Bold | Italic | Fg<255, 200, 60> | Bg<40, 35, 20>
```

### Available Tags

| Tag | Effect |
|-----|--------|
| `Bold` | Bold/bright weight |
| `Dim` | Dimmed/faint |
| `Italic` | Italic text |
| `Underline` | Underlined |
| `Strike` | Strikethrough |
| `Inverse` | Swap foreground/background |
| `Fg<R, G, B>` | 24-bit foreground color |
| `Bg<R, G, B>` | 24-bit background color |

Tags can be combined in any order:

```cpp
t<"OK"> | Fg<80, 220, 120> | Bold     // green bold
t<"OK"> | Bold | Fg<80, 220, 120>     // same result — order doesn't matter
```

### How Tags Work Internally

Each tag is a `StyTag<CTStyle V>` where `CTStyle` is a structural aggregate:

```cpp
struct CTStyle {
    bool has_fg = false, has_bg = false;
    uint8_t fg_r = 0, fg_g = 0, fg_b = 0;
    uint8_t bg_r = 0, bg_g = 0, bg_b = 0;
    bool bold_ = false, dim_ = false, italic_ = false;
    bool underline_ = false, strike_ = false, inverse_ = false;

    consteval CTStyle merge(CTStyle other) const;
    Style runtime() const;  // Convert to runtime Style
};
```

Because `CTStyle` is structural, it works as an NTTP (non-type template
parameter). The `|` operator returns a new node type with the merged style
baked in — the merge is evaluated by the compiler, not at runtime.

## Runtime Style Objects

For dynamic styling (colors based on state, theme-aware colors), use the
`Style` class:

```cpp
Style s = Style{}
    .with_fg(Color::rgb(100, 180, 255))
    .with_bg(Color::rgb(20, 20, 30))
    .with_bold()
    .with_italic();
```

### Style Builder Methods

Each method returns a new `Style` (immutable, functional style):

| Method | Description |
|--------|-------------|
| `.with_fg(Color)` | Set foreground color |
| `.with_bg(Color)` | Set background color |
| `.with_bold(bool v = true)` | Bold weight |
| `.with_dim(bool v = true)` | Dimmed/faint |
| `.with_italic(bool v = true)` | Italic |
| `.with_underline(bool v = true)` | Underline |
| `.with_strikethrough(bool v = true)` | Strikethrough |
| `.with_inverse(bool v = true)` | Swap fg/bg |

### Style Merging

Styles merge with `merge()` — the right-hand style's non-empty fields win:

```cpp
Style base = Style{}.with_fg(Color::red()).with_bold();
Style over = Style{}.with_fg(Color::blue());
Style merged = base.merge(over);
// Result: blue foreground (overridden), bold (preserved)
```

The `|` operator also merges: `style_a | style_b`.

### Using Runtime Styles

Pass styles to `text()`:

```cpp
dyn([&] {
    auto s = Style{}.with_bold().with_fg(gauge_color(cpu_pct));
    return text("CPU: " + std::to_string(cpu_pct) + "%", s);
})
```

Or to the `BoxBuilder`:

```cpp
dyn([&] {
    return vstack().style(my_style)(text("content"));
})
```

## Color

The `Color` class supports named colors, indexed (256), and 24-bit RGB:

### Named Colors

```cpp
Color::black()        Color::bright_black()    // (gray)
Color::red()          Color::bright_red()
Color::green()        Color::bright_green()
Color::yellow()       Color::bright_yellow()
Color::blue()         Color::bright_blue()
Color::magenta()      Color::bright_magenta()
Color::cyan()         Color::bright_cyan()
Color::white()        Color::bright_white()
Color::gray()         // alias for bright_black
```

### RGB Colors (24-bit TrueColor)

```cpp
Color::rgb(100, 180, 255)    // Sky blue
Color::rgb(255, 80, 80)      // Red
Color::rgb(80, 220, 120)     // Green
```

All `Color::rgb()` calls are `constexpr` — they work in compile-time contexts.

### Hex Colors

```cpp
Color::hex(0xFF5050)    // consteval — must be a compile-time constant
```

### HSL Colors

```cpp
Color::hsl(210.0f, 0.8f, 0.6f)   // Perceptually uniform, constexpr
```

### Indexed Colors (256-color palette)

```cpp
Color::indexed(196)    // Bright red in the 256-color palette
```

### Color Adjustment

```cpp
auto c = Color::rgb(100, 180, 255);
auto lighter = c.lighten(0.2f);   // 20% lighter
auto darker  = c.darken(0.3f);    // 30% darker
auto rgb     = Color::cyan().to_rgb();   // resolve Named/Indexed → true RGB
```

`to_rgb()` projects any color kind onto real RGB channels (Named through the
ANSI-16 palette, Indexed through the xterm-256 table). Use it whenever you need
to do channel math on a color you didn't create — the raw `r()/g()/b()`
accessors return the *palette index* for Named/Indexed colors, not channels.

## Gradients

Multi-color text is a one-liner. `gradient()` sweeps a color across a string —
each character gets its own interpolated color by horizontal position — and
`rainbow()` does a full hue sweep:

```cpp
gradient("MAYA", Color::hex(0xFF5F6D), Color::hex(0xFFC371))   // two-stop
gradient("status", Gradient{{sky, teal, gold}})                 // multi-stop
rainbow("party mode")                                            // full spectrum
```

Under the hood this is ONE `TextElement` carrying per-codepoint `StyledRun`s —
not a per-character element explosion — so gradient text wraps, truncates, and
measures exactly like plain `text()`, and costs the same to lay out.

Style **attributes** (bold/italic/underline) ride along via the `base`
parameter — pipes can't reach inside the runs:

```cpp
gradient("HEADING", a, b, Style{}.with_bold())
```

### The Gradient type

`Gradient` is an ordered list of stops sampled by `t ∈ [0, 1]` — useful on its
own for any value-driven color (gauges, heatmaps, load bars):

```cpp
Gradient health{{ Color::hex(0x2ECC71),    // green
                  Color::hex(0xF1C40F),    // amber
                  Color::hex(0xE74C3C) }}; // red
Color c = health.at(load);                 // load ∈ 0..1
```

Stops of any color kind work — Named/Indexed stops are resolved through
`to_rgb()` before blending, so interpolation is always channel-correct.

### Gradient rules

`gradient_rule()` is a full-width divider that tracks its pane — it receives
its real allocated width at paint time and tiles a glyph across it:

```cpp
v(
    header,
    gradient_rule(Color::hex(0x7F5AF0), Color::hex(0x2CB67D)),   // spans the pane
    body,
    gradient_rule(Gradient{{a, b, c}}, U'━')                      // custom glyph
)
```

Resize the terminal and the rule re-tiles — no width to compute, ever.

## Themes

A `Theme` is a struct of 23 named color slots. Using themes keeps your app
visually consistent and lets users switch palettes:

```cpp
struct Theme {
    LitColor primary, secondary, accent;
    LitColor success, error, warning, info;
    LitColor text, inverse_text, muted;
    LitColor surface, background, border;
    LitColor diff_added, diff_removed, diff_changed;
    LitColor highlight, selection, cursor, link;
    LitColor placeholder, shadow, overlay;
};
```

### Symbolic vs. literal colour

A colour is indexed by whether a theme slot can still be hiding inside it:

| Type | Alias for | Can hold a slot? | Use it for |
|------|-----------|------------------|------------|
| `Color` | `BasicColor<Res::Sym>` | yes | widget `Config` defaults, host overrides, anything *authored* |
| `LitColor` | `BasicColor<Res::Lit>` | no | `Theme` fields, gradients, anything *painted* |

`Color::slot(...)` exists only on `Color`; the channel accessors (`r()`,
`to_rgb()`, `degrade()`, `append_fg_sgr()`) exist only on `LitColor`. A
literal converts to symbolic implicitly — so `Color::rgb(1,2,3)` drops into
either — but the only way back down is `Theme::resolve()`:

```cpp
Color    authored = Color::slot(ThemeSlot::Accent);  // what the widget wrote
LitColor painted  = theme::live().resolve(authored); // what the terminal gets

authored.r();        // compile error: a slot has no red channel
painted.r();         // compiles — but see below
```

That is why `resolve()` is total: `Theme` holds `LitColor`, so a slot cannot
resolve to another slot, and no consumer has to handle a case it has no
answer for. If you need the raw payload bytes for a **cache key** rather than
a colour, use `raw_r()` / `raw_g()` / `raw_b()` — they read the authored value
without pretending a slot enum is a channel.

### Paintable is not numeric

Resolving buys you a colour the terminal can render. It does **not** buy you
a colour you can do arithmetic on, and conflating those is agentty #45:

| Kind | `r()` / `g()` / `b()` hold |
|------|---------------------------|
| `Rgb` | real channels |
| `Named` | a **palette index** in `r_`; `g_`/`b_` are zero |
| `Indexed` | a **palette index** in `r_`; `g_`/`b_` are zero |
| `Default` | nothing — it means SGR 39/49 |

So `bright_black` is `Named(8)`, and blending it read 8 as a red channel:
`rgb(8,0,0)`, near-black, invisible on a dark terminal and undetectable to
anyone running a scheme instead of `theme::native`.

Ask **`has_channels()`** before arithmetic, and make the failure case *no
effect* rather than a computed wrong answer:

```cpp
LitColor lerp(LitColor a, LitColor b, double t) {
    if (!a.has_channels() || !b.has_channels())
        return t < 0.5 ? a : b;          // snap; never invent a triple
    /* … mix channels … */
}
```

Do **not** reach for `to_rgb()` to "fix" it. That substitutes the standard
xterm table for whatever the user actually remapped, and on `Default` it
guesses white — which is the grey-on-grey failure on a light terminal.

The same asymmetry decides what ink goes on a filled band. For a truecolor
band you can measure it (`ink_for()`); for a palette band you cannot, because
the terminal owns those 16 entries and will not say what they are. Use
`on_band()`, which measures the first case and falls back to **reverse video**
(SGR 7) for the second — letting the terminal swap its own pair is strictly
better than guessing at a palette you cannot read.

### Adding a slot

Slots are declared once, in `MAYA_THEME_SLOTS` (`style/theme.hpp`). That one
list generates the `ThemeSlot` enum, `Theme`'s fields, `resolve()`'s switch
and `slot_field_name()`, so those four can never disagree:

```cpp
#define MAYA_THEME_SLOTS(X)     \
    X(primary,   Primary)       \
    X(secondary, Secondary)     \
    /* … */
```

`Theme` stays a plain aggregate, so `Theme{.primary = …}` and `derive()` are
unchanged. What the list cannot generate is the 615 built-in schemes, and a
designated initializer that omits a field is legal C++ — it value-initializes
it. So a default-constructed `Color` is `ColorKind::Unset`, a state no
deliberate choice produces:

```cpp
Color c;                       // Unset — "nobody said", not white
c.is_set();                    // false
theme.complete();              // did every slot get a colour?
*theme.first_unset();          // …and if not, which one
```

`schemes.hpp` `static_assert`s that every scheme is complete, so adding a slot
and forgetting to fill it in is a **build failure naming the field**, not an
unreadable element someone reports months later. An `Unset` colour paints as
inherit (SGR 39/49), so even if one escaped, the failure mode is "looks
unstyled", never a colour maya invented.

### Projected palettes

Most code should read the theme directly — `Color::slot(...)` resolved at
paint time is the whole design. But a few subsystems genuinely cannot:
markdown keeps a flat palette because its render path is hot and its parse
worker runs off-thread with no way to reach a `Theme`.

A derived palette has to be refreshed when the theme moves. Declare *how* to
compute it and maya owns the rest:

```cpp
struct MyPalette {
    using type = MyColors;
    static type project(const Theme& t) {
        return { .body = t.text, .rule = t.border, /* … */ };
    }
};

const MyColors& c = theme::projected<MyPalette>();   // always current
```

This is a **pull, not a push**, and that is the point. The old design was
`on_theme_changed(fn)` with each subsystem registering once — and a
subsystem that forgot was invisible, because a palette that never re-derives
looks exactly like a palette whose theme never changed. Here, deriving *is*
the read path: there is no registration to omit.

Cost in the steady state is an acquire load and an integer compare against
`theme::live_epoch()`. Each snapshot is immutable and published by pointer
swap, so an off-thread reader gets a frozen object with no lock — and a
reader holding an older snapshot keeps reading valid memory.

`theme::override_projection<P>(v)` forces an explicit value; it stands until
the next theme change, which re-derives. The theme is the source of truth and
an override is a deliberate exception to it.

### Built-in Themes

```cpp
maya::theme::dark         // Modern dark theme (truecolor)
maya::theme::light        // Light theme (truecolor)
maya::theme::dark_ansi    // 16-color dark (wide terminal support)
maya::theme::light_ansi   // 16-color light
```

### Using Themes in Your App

Pass a theme via `RunConfig`. In Program apps, access theme colors in `view()`:

```cpp
// Theme is accessible via the runtime — use it in view() for styling.
// For compile-time colors, use Fg<R,G,B> directly.
// For theme-aware colors, build Style objects with theme values.

struct MyApp {
    struct Model { /* ... */ };
    // ...
    static Element view(const Model& m) {
        // Use compile-time colors when the color is fixed:
        return v(
            text("Hello") | Bold | Fg<100, 180, 255>,
            text("Muted note") | Dim
        ) | pad<1>;
    }
};
run<MyApp>({.theme = theme::dark});
```

### Custom Themes

Create a custom theme by deriving from an existing one:

```cpp
constexpr auto my_theme = Theme::derive(theme::dark, [](Theme& t) {
    t.primary = Color::rgb(255, 120, 80);
    t.accent  = Color::rgb(80, 255, 200);
    t.border  = Color::rgb(60, 50, 70);
});
```

## Style Interning (Performance)

Under the hood, maya interns all `Style` objects into a `StylePool`. Each unique
style gets a compact `uint16_t` ID. Canvas cells store this 16-bit ID instead
of the full style object, reducing cell size from ~48 bytes to 8 bytes. This
is critical for SIMD-accelerated frame diffing.

You don't interact with the `StylePool` directly in element-based rendering —
the framework handles it. In `canvas_run()` mode, you intern styles explicitly:

```cpp
canvas_run(config,
    [&](StylePool& pool, int w, int h) {
        // Called on resize — pool is cleared, re-intern everything
        my_style_id = pool.intern(Style{}.with_bold().with_fg(Color::green()));
    },
    [&](const Event& ev) { return true; },
    [&](Canvas& canvas, int w, int h) {
        canvas.set(0, 0, U'*', my_style_id);  // Use the interned ID
    }
);
```

## Predefined Runtime Styles

For convenience, the `style` module provides common styles:

```cpp
Style{}.with_bold()
Style{}.with_dim()
Style{}.with_italic()
Style{}.with_underline()
Style{}.with_strikethrough()
Style{}.with_inverse()
```

These are runtime equivalents of the compile-time DSL tags (`Bold`, `Dim`,
etc.) — use them inside `dyn()` lambdas or with `text()`.

## Style Composition Patterns

### Conditional styling

```cpp
dyn([&] {
    auto color = is_error ? Color::rgb(255, 80, 80)
                          : Color::rgb(80, 220, 120);
    return text(message, Style{}.with_fg(color));
})
```

### Gradient/computed colors

For threshold-band colors, a small function still reads well:

```cpp
Color gauge_color(float pct) {
    if (pct < 50) return Color::rgb(80, 220, 120);   // green
    if (pct < 80) return Color::rgb(240, 200, 60);    // yellow
    return Color::rgb(240, 80, 80);                    // red
}
```

For smooth blends, use the real thing — [`Gradient`](#gradients):

```cpp
Gradient heat{{Color::rgb(80,220,120), Color::rgb(240,200,60), Color::rgb(240,80,80)}};
text("42%", Style{}.with_fg(heat.at(pct / 100.0f)))
```

### Style presets

```cpp
static const Style sLabel = Style{}.with_fg(Color::rgb(110, 110, 130));
static const Style sValue = Style{}.with_bold().with_fg(Color::rgb(210, 210, 225));
static const Style sMuted = Style{}.with_fg(Color::rgb(55, 55, 70));
```

Then use them everywhere:

```cpp
text("CPU:", sLabel)
text("42%", sValue)
```
