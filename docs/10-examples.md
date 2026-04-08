# Examples Walkthrough

maya ships with 10 examples that progressively demonstrate its features. This
guide walks through each one, explaining the patterns and techniques used.

## 1. counter.cpp — The Simplest App

**Mode**: Fullscreen (`run()`)
**Lines**: ~24
**Demonstrates**: Minimal setup, Signal, event handling, DSL basics

```cpp
Signal<int> count{0};

run(
    [&](const Event& ev) {
        on(ev, '+', '=', [&] { count.update([](int& n) { ++n; }); });
        on(ev, '-', '_', [&] { count.update([](int& n) { --n; }); });
        return !key(ev, 'q');
    },
    [&] {
        return (v(
            dyn([&] { return text("Counter: " + std::to_string(count.get()),
                                  Style{}.with_bold()); }),
            t<"[+/-] change  [q] quit"> | Dim
        ) | pad<1>).build();
    }
);
```

**Key patterns**:
- `Signal<int>` for reactive state
- `on(ev, '+', '=', fn)` handles two keys with one call
- `dyn()` bridges runtime state into the compile-time tree
- `t<"..."> | Dim` for static styled text
- `| pad<1>` for padding around the whole UI
- `.build()` converts DSL tree to `Element`

This is the template for almost every maya app.

## 2. demo.cpp — Feature Showcase

**Mode**: Fullscreen (`run()` with config)
**Demonstrates**: RunConfig, Ctx, theme colors, nested layouts, conditional styling

Builds on counter with:
- `{.title = "maya demo"}` — sets the terminal window title
- `const Ctx& ctx` — accesses `ctx.theme.primary`, `ctx.theme.success`, etc.
- Nested `h()` inside `v()` for two-column counter display
- Conditional color based on counter sign (`c >= 0 ? success : error`)
- Multiple Signals (`counter`, `message`)

**Key pattern — theme-aware styling**:
```cpp
[&](const Ctx& ctx) {
    return (v(
        dyn([&] {
            return text("Title", Style{}.with_bold().with_fg(ctx.theme.primary));
        }),
        // ...
    ) | border_<Round> | pad<1>).build();
}
```

## 3. dsl_demo.cpp — Compile-Time DSL

**Mode**: One-shot (`print()`)
**Demonstrates**: Fully constexpr UI, type-state safety, bcol after border

The only example that uses `print()` for one-shot output (no event loop).
Shows that entire UI trees can be `constexpr`:

```cpp
constexpr auto card = v(
    t<"System Status"> | Bold | Fg<220, 220, 240>,
    t<"">,
    h(t<"CPU:"> | Dim, t<" 42%"> | Bold | Fg<80, 220, 120>),
    h(t<"Mem:"> | Dim, t<" 8.2G"> | Bold | Fg<180, 130, 255>)
) | border_<Round> | bcol<60, 65, 80> | pad<1>;

print(ui.build());
```

**Key insight**: Everything before `.build()` is evaluated by the compiler.
The border color (`bcol<60, 65, 80>`) only compiles because `border_<Round>`
comes first — type-state enforcement.

Also demonstrates mixing constexpr and runtime with `dyn()`.

## 4. inline.cpp — Inline Mode

**Mode**: `run()` with `Mode::Inline`
**Demonstrates**: Inline rendering, live bar animation, theme cycling

Uses `run()` with `{.mode = Mode::Inline}` — renders in the terminal scrollback
instead of the alt screen. The output stays visible after exit.

**Key pattern — runtime bar construction**:
```cpp
int filled = std::abs(count) % 21;
std::string bar;
for (int i = 0; i < 20; ++i)
    bar += (i < filled) ? "█" : "░";
```

Shows that `text()` works with runtime strings for dynamic visualizations.

## 5. progress.cpp — Inline Progress Display

**Mode**: Inline (`live()`)
**Demonstrates**: Delta time, parallel progress bars, spinners, auto-quit

Uses `live({.fps = 30}, [](float dt) { ... })` — the delta-time
variant for smooth animation.

**Key patterns**:
- State advancement: `progress += speed * dt`
- Auto-quit: `if (g_done == kN) quit()`
- Spinner animation: `spin(g_time)` cycles through braille spinner frames
- Sub-cell progress: partial block characters `▏▎▍▌▋▊▉` for smooth bars
- Per-package state machine: waiting → downloading → done

**Rendering approach**: Builds a `vector<Element>` dynamically, then wraps it
in `v(rows)`. Each package gets a different row based on its state.

## 6. agent.cpp — AI Agent Simulation

**Mode**: Inline (`live()`)
**Demonstrates**: Streaming text, phase state machine, diff coloring, bordered output

Simulates an AI coding agent with token-by-token text streaming. Each "block"
(thinking, tool call, result, response) streams at a configurable speed.

**Key patterns**:
- **Streaming text**: Characters revealed over time via
  `shown = min(phase_t * speed, content.size())`
- **Phase state machine**: Advances through blocks sequentially with pause
  between phases
- **Diff rendering**: Detects `+` and `─` prefixes to color-code diff output
- **Bordered tool results**: `vstack().border(Round).border_color(...)(content)`
- **Spinner state**: `spin(total_t)` shows activity during thinking

This example uses `v(rows)` for dynamic child lists and `vstack().border()`
for runtime-styled bordered panels.

## 7. dashboard.cpp — System Monitoring Dashboard

**Mode**: Fullscreen (`run()` with `fps = 30`)
**Demonstrates**: Adaptive layout, multi-panel grid, process table, sparklines

The most complex element-tree example. Renders a full monitoring dashboard with:
- Header bar (uptime, load, summary)
- CPU panel with per-core sparklines
- Memory panel with gauge bar
- Network panel with rx/tx sparklines
- Disk I/O panel
- Process table with color-coded rows
- Status bar with theme selector

**Key patterns**:

**Adaptive layout based on terminal size**:
```cpp
[&](const Ctx& ctx) {
    int w = ctx.size.width.value;
    int left_inner  = std::max(20, w * 3 / 5 - 4);
    int right_inner = std::max(14, w * 2 / 5 - 4);
    // ...
}
```

**Flex grow for column proportions**:
```cpp
auto left_col  = vstack().grow(3)(cpu_panel, net_panel);
auto right_col = vstack().grow(2)(mem_panel, disk_panel);
auto grid = hstack()(left_col, right_col);
```

**Bordered panels with titles**:
```cpp
vstack()
    .border(BorderStyle::Round).border_color(theme_border())
    .border_text(spin() + " CPU", BorderTextPos::Top)
    .padding(0, 1, 0, 1)(rows)
```

**Alternating row backgrounds**:
```cpp
auto row_bg = (i % 2 == 1)
    ? Style{}.with_bg(Color::rgb(18, 18, 26))
    : Style{};
hstack().style(row_bg)(columns...)
```

**Theme cycling**:
```cpp
on(ev, 't', [] { g_theme = (g_theme + 1) % kThemeCount; });
```

## 8. matrix.cpp — Matrix Digital Rain

**Mode**: Canvas (`canvas_run()`)
**Demonstrates**: Direct canvas painting, per-cell animation, mouse interaction, shockwaves

A full Matrix-style rain effect with:
- Katakana + digit glyphs with random mutation
- Variable-speed drops with trails
- Mouse hover highlighting
- Click-to-shockwave with ripple propagation
- 7 color themes
- Pause, speed control, jolt all drops

**Key patterns**:

**Style pre-interning for gradients**:
```cpp
for (int d = 0; d < kMaxTrail; ++d) {
    float f = pow(1.0f - float(d) / float(kMaxTrail - 2), 1.7f);
    auto r = uint8_t(f * trail.r + (1-f) * 7);
    // ...
    styles.theme[t][d] = pool.intern(Style{}.with_fg(Color::rgb(r, g, b)));
}
```

**Direct canvas painting** (no element tree):
```cpp
[&](Canvas& canvas, int W, int H) {
    rain->paint(canvas, styles, theme, hover_col, glitching);
    paint_bar(canvas, styles, W, H, ...);
}
```

**Mouse interaction**:
```cpp
if (auto pos = mouse_pos(ev)) hover_col = pos->col - 1;
if (mouse_clicked(ev)) rain->shockwave(pos->col, pos->row);
```

## 9. viz.cpp — Signal Charts + Heatmap

**Mode**: Canvas (`canvas_run()`)
**Demonstrates**: Braille sub-cell graphics, 2D heatmap, fast math, split panels

Two visualization techniques side by side:
- Left: Three braille area charts (CPU, MEM, NET) with sub-cell resolution
- Right: 2D wave interference heatmap with 5 animated point sources

**Key patterns**:

**Braille area charts** — 2x4 sub-cell resolution per terminal cell:
```cpp
// Each terminal cell = 2px wide × 4px tall
// Fill mask lookup table for O(1) per cell:
static constexpr uint8_t kFill[2][5] = {
    {0x0F, 0x0E, 0x0C, 0x08, 0x00},  // left column
    {0xF0, 0xE0, 0xC0, 0x80, 0x00},  // right column
};
uint8_t mask = kFill[0][threshold_left] | kFill[1][threshold_right];
canvas.set(x, y, char32_t(0x2800 + mask), gradient_style);
```

**Half-block heatmap** — 2x vertical resolution:
```cpp
canvas.set(cx, cy, U'▀', styles.heat[fg_index][bg_index]);
// fg = top pixel color, bg = bottom pixel color
```

**Fast math approximations**:
```cpp
inline float fast_sin(float x) noexcept { /* polynomial approx */ }
inline float fast_sqrt(float x) noexcept { /* Quake III rsqrt */ }
```

## 10. fireworks.cpp — Particle Physics

**Mode**: Canvas (`canvas_run()`)
**Demonstrates**: Physics simulation, particle lifecycle, auto-launch, mouse click-to-launch

Renders fireworks with:
- Rocket launch → ascent → explosion
- 80-160 particles per burst with 4 explosion shapes
- Gravity, drag, color aging
- Background star field with twinkling
- Multiple color palettes
- Auto-launch with random timing + click-to-launch

**Key patterns**:

**Particle lifecycle**:
```cpp
for (auto& p : particles) {
    p.vx *= kDrag;
    p.vy *= kDrag;
    p.vy += kGravity * kDt;
    p.x += p.vx * kDt;
    p.y += p.vy * kDt;
    p.life -= p.decay;
}
std::erase_if(particles, [](const Particle& p) {
    return p.life <= 0.f;
});
```

**Color aging** — palette interpolation based on particle life:
```cpp
RGB palette_color(int pal, float life) {
    if (life > 0.85f) return lerp(bright, white, t);   // Flash
    if (life > 0.45f) return lerp(mid, bright, t);      // Bright
    return lerp(dim, mid, t);                             // Fade
}
```

**Glyph aging** — larger characters for young particles, smaller for old:
```cpp
char32_t kBurst[] = {U'✦', U'✦', U'●', U'•', U'·', U'·'};
int ci = int((1.f - p.life) * kGlyphs);
canvas.set(cx, cy, kBurst[ci], gradient_style);
```

## Example Classification

| Example | Mode | Interactive | Uses DSL | Uses Canvas | Animation |
|---------|------|------------|----------|-------------|-----------|
| counter | run() | Yes | Yes | No | No |
| demo | run() | Yes | Yes | No | No |
| dsl_demo | print() | No | Yes | No | No |
| inline | run(inline) | Yes | Yes | No | Yes |
| progress | live() | No | Partial | No | Yes |
| agent | live() | No | Partial | No | Yes |
| dashboard | run() | Yes | Partial | No | Yes |
| matrix | canvas_run() | Yes | No | Yes | Yes |
| viz | canvas_run() | Yes | No | Yes | Yes |
| fireworks | canvas_run() | Yes | No | Yes | Yes |
