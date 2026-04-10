# Examples Walkthrough

maya ships with 23 examples that progressively demonstrate its features. This
guide walks through the major ones, explaining the patterns and techniques used.

## 1. counter.cpp — The Simplest App

**Mode**: Fullscreen (`run<P>()`)
**Lines**: ~30
**Demonstrates**: Program concept, Model/Msg/update/view/subscribe, Cmd, key_map

```cpp
struct Counter {
    struct Model { int count = 0; };
    struct Increment {}; struct Decrement {}; struct Reset {}; struct Quit {};
    using Msg = std::variant<Increment, Decrement, Reset, Quit>;
    static Model init() { return {}; }
    static auto update(Model m, Msg msg) -> std::pair<Model, Cmd<Msg>> {
        return std::visit(overload{
            [&](Increment) { return std::pair{Model{m.count + 1}, Cmd<Msg>{}}; },
            [&](Decrement) { return std::pair{Model{m.count - 1}, Cmd<Msg>{}}; },
            [&](Reset)     { return std::pair{Model{0}, Cmd<Msg>{}}; },
            [](Quit)       { return std::pair{Model{}, Cmd<Msg>::quit()}; },
        }, msg);
    }
    static Element view(const Model& m) {
        return v(t<"Counter"> | Bold | Fg<100,180,255>, blank_, text(m.count) | Bold, blank_,
                 t<"+/- to change, r to reset, q to quit"> | Dim) | pad<1> | border_<Round>;
    }
    static auto subscribe(const Model&) -> Sub<Msg> {
        return key_map<Msg>({{'q', Quit{}}, {'+', Increment{}}, {'=', Increment{}},
                             {'-', Decrement{}}, {'r', Reset{}},
                             {SpecialKey::Up, Increment{}}, {SpecialKey::Down, Decrement{}}});
    }
};
int main() { run<Counter>({.title = "counter"}); }
```

**Key patterns**:
- **Program concept**: A struct with `Model`, `Msg`, `init`, `update`, `view`, and `subscribe`
- **Model as plain data**: `Model` is a simple struct holding the app state
- **`Cmd<Msg>{}`** for no side effects; `Cmd<Msg>::quit()` to exit
- **`key_map<Msg>`** for declarative key bindings — maps keys to messages
- **`view()` as pure function**: Takes `const Model&`, returns `Element`
- `t<"..."> | Dim` for static styled text
- `| pad<1> | border_<Round>` for padding and border around the whole UI

This is the canonical Program-style maya app.

## 2. demo.cpp — Feature Showcase

**Mode**: Fullscreen (`run<P>()`)
**Demonstrates**: RunConfig, Program architecture, theme colors, nested layouts, conditional styling

Builds on counter using the Program pattern with:
- `{.title = "maya demo"}` — sets the terminal window title via RunConfig
- Nested `h()` inside `v()` for two-column counter display
- Conditional color based on counter sign (`c >= 0 ? success : error`)
- Multiple message types for different interactions

**Key pattern — theme-aware styling**:
```cpp
static Element view(const Model& m) {
    return (v(
        text("Title", Style{}.with_bold().with_fg(theme::dark.primary)),
        // ...
    ) | border_<Round> | pad<1>).build();
}
```

## 3. dsl_demo.cpp — Compile-Time DSL

**Mode**: One-shot (`print()`)
**Demonstrates**: Fully constexpr UI, type-state safety, bcol after border

Uses `print()` for one-shot output (no event loop).
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

Uses simple `run()` with `{.mode = Mode::Inline}` — renders in the terminal scrollback
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

**Mode**: Fullscreen (`run()` with `fps = 30`, receives `Ctx`)
**Demonstrates**: Adaptive layout, multi-panel grid, process table, sparklines

Uses simple `run()` with a render function that takes `const Ctx&` for
terminal-size-aware layout. The most complex element-tree example. Renders a
full monitoring dashboard with:
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

## 8. chat.cpp — AI Agent Session

**Mode**: Inline (`run()` with `Mode::Inline`)
**Demonstrates**: Tool widgets, streaming markdown, activity bar, toast notifications

An animated simulation of a Claude Code AI agent session, streaming content
character-by-character. Showcases every major tool widget: `UserMessage`,
`ThinkingBlock`, `ReadTool`, `SearchResult`, `PlanView`, `EditTool`,
`WriteTool`, `Permission`, `BashTool`, `DiffView`, `AgentTool`, `FetchTool`,
`AssistantMessage` with `StreamingMarkdown`, `ActivityBar`, `ToastManager`,
`Badge`, and `Callout`.

```cpp
int main() {
    maya::run(
        {.fps = 30, .mode = Mode::Inline},
        [](const Event& ev) {
            return !(key(ev, 'q') || key(ev, SpecialKey::Escape));
        },
        [&] {
            tick(app);
            return build_ui(app);
        }
    );
}
```

## 9. deploy.cpp — CI/CD Deployment Pipeline

**Mode**: Fullscreen (`run()`)
**Demonstrates**: Multi-service pipeline, log streaming, environment switching

A real-time animated deployment pipeline dashboard showing multiple
microservices being built, tested, and deployed. Supports triggering new
deployment waves, rollback, force-deploy, and environment selection
(dev/staging/prod).

## 10. hacker.cpp — Cyberpunk Hacker Terminal

**Mode**: Fullscreen (`run()`)
**Demonstrates**: Rapid data scrolling, sparklines, heatmaps, badge widgets

A movie-style hacking terminal: rapid scrolling data, flashing alerts, network
intrusion simulation, hex dumps, progress bars, sparklines, and heatmaps. Pure
eye candy with three color themes (green, amber, cyan).

## 11. ide.cpp — Terminal IDE Layout

**Mode**: Fullscreen (`run()`)
**Demonstrates**: Multi-panel layout, syntax highlighting, file tree, diagnostics

A VS Code / Zed-inspired terminal IDE layout using the full maya widget
toolkit. Features a file tree, tabbed editor with syntax highlighting, code
outline, diagnostics panel, git status, terminal output panel, and status bar.
Panels toggle with number keys; `b` triggers a simulated build.

## 12. music.cpp — Terminal Music Player

**Mode**: Fullscreen (`run()` with `fps = 15`)
**Demonstrates**: Animated heatmap album art, sparkline visualizer, scrollable playlist

A Spotify/Apple-Music-inspired terminal music player with animated heatmap
album art, sparkline audio visualizer, progress bar, and scrollable playlist.
All data is simulated. Supports play/pause, next/previous, shuffle, repeat, and
volume control.

## 13. space.cpp — Mission Control Dashboard

**Mode**: Fullscreen (`run()` with `fps = 15`)
**Demonstrates**: Gauges, sparklines, heatmap, line chart, bar chart, physics simulation

A NASA-style mission control dashboard tracking a simulated spacecraft journey
to Mars. Features telemetry gauges, sparklines, a heatmap, line chart, bar
chart, crew status, subsystem health, random events, and physics simulation.

## 14. stocks.cpp — Live Stock Ticker

**Mode**: Inline (`run()` with `Mode::Inline`, `fps = 20`)
**Demonstrates**: Animated charts, sparklines, color-coded gains/losses, news feed

A visually rich terminal stock dashboard with animated price charts,
sparklines, color-coded gains/losses, portfolio summary, and a scrolling news
feed. Data uses correlated random walks. Output stays in scrollback after exit.

## 15. sysmon.cpp — System Monitor

**Mode**: Inline (`run()` with `Mode::Inline`, `fps = 15`)
**Demonstrates**: CPU sparklines, process table, activity log, sort modes

A fullscreen system monitor with fake-live telemetry: CPU cores with
sparklines, memory banks, network interfaces, process table, entropy pool, and
a scrolling activity log. Supports pause, log toggle, process sort, and speed
control.

## 16. matrix.cpp — Matrix Digital Rain

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

## 17. viz.cpp — Signal Charts + Heatmap

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

## 18. fireworks.cpp — Particle Physics

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

## 19. stopwatch.cpp — Stopwatch with Ticks

**Mode**: Fullscreen (`run<P>()`)
**Demonstrates**: `Sub::every()` for periodic ticks, `Cmd::after()` for delayed effects, conditional subscriptions, `when()` DSL

A stopwatch app that ticks every frame while running and supports lap timing
with a flash effect.

**Key patterns**:
- **Conditional subscriptions**: `Sub::every()` only active when the stopwatch
  is running — tick messages stop when paused
- **`Cmd::after()`** for flash timeout: triggers a message after a delay to
  clear a visual flash indicator
- **`when()` DSL** for conditional rendering based on model state

## 20. breakout.cpp — Breakout / Arkanoid Clone

**Mode**: Canvas (`canvas_run()`)
**Demonstrates**: Game loop, half-block pixel rendering, comet trail, power-ups

A Breakout/Arkanoid clone using canvas half-block rendering. Features multi-hit
bricks, a comet trail on the ball, particles, power-ups, and level progression.
Controls: left/right or h/l for paddle, space to launch/pause, r to restart.

## 21. snake.cpp — Snake

**Mode**: Canvas (`canvas_run()`)
**Demonstrates**: Half-block pixels, gradient body, particle effects, ghost trails

A Snake game using canvas half-block rendering with a gradient-colored body,
particle effects on food collection, and ghost trails. Arrow keys / WASD / hjkl
to move, space to pause, W to toggle wrap mode.

## 22–23. Additional Canvas Demos

The remaining examples are all `canvas_run()` physics and visual demos:

- **doom_fire.cpp** — Doom PSX fire effect with 3 palettes, ember particles,
  wind, intensity control
- **fluid.cpp** — 2D Navier-Stokes fluid/smoke simulation with half-block rendering
- **fps.cpp** — Wolfenstein-style DDA raycaster with procedural textures,
  lighting, and enemy sprites
- **life.cpp** — Conway's Game of Life with heat-gradient aging and half-block
  double vertical resolution
- **mandelbrot.cpp** — Real-time Mandelbrot explorer with smooth coloring,
  auto-zoom, and 6 palettes
- **particles.cpp** — Particle physics system with 5 modes (fireworks, galaxy,
  fountain, vortex, starfield)
- **raymarch.cpp** — Real-time SDF raymarcher with reflective surfaces, sunset
  sky, and colored lights
- **sorts.cpp** — Four sorting algorithms racing side-by-side with animated
  colored bars
- **spectrum.cpp** — Simulated audio spectrum analyzer with 4 visualization
  modes and beat detection

## Example Classification

| Example | Mode | Interactive | Uses DSL | Uses Canvas | Animation |
|---------|------|------------|----------|-------------|-----------|
| counter | run\<P\>() | Yes | Yes | No | No |
| demo | run\<P\>() | Yes | Yes | No | No |
| dsl_demo | print() | No | Yes | No | No |
| inline | run() inline | Yes | Yes | No | Yes |
| progress | live() | No | Partial | No | Yes |
| agent | live() | No | Partial | No | Yes |
| dashboard | run() | Yes | Partial | No | Yes |
| chat | run() inline | Yes | Partial | No | Yes |
| deploy | run() | Yes | Partial | No | Yes |
| hacker | run() | Yes | Partial | No | Yes |
| ide | run() | Yes | Partial | No | No |
| music | run() | Yes | Partial | No | Yes |
| space | run() | Yes | Partial | No | Yes |
| stocks | run() inline | Yes | Partial | No | Yes |
| sysmon | run() inline | Yes | Partial | No | Yes |
| matrix | canvas_run() | Yes | No | Yes | Yes |
| viz | canvas_run() | Yes | No | Yes | Yes |
| fireworks | canvas_run() | Yes | No | Yes | Yes |
| stopwatch | run\<P\>() | Yes | Yes | No | Yes |
| breakout | canvas_run() | Yes | No | Yes | Yes |
| snake | canvas_run() | Yes | No | Yes | Yes |
| doom_fire | canvas_run() | Yes | No | Yes | Yes |
| fluid | canvas_run() | Yes | No | Yes | Yes |
| fps | canvas_run() | Yes | No | Yes | Yes |
| life | canvas_run() | Yes | No | Yes | Yes |
| mandelbrot | canvas_run() | Yes | No | Yes | Yes |
| particles | canvas_run() | Yes | No | Yes | Yes |
| raymarch | canvas_run() | Yes | No | Yes | Yes |
| sorts | canvas_run() | Yes | No | Yes | Yes |
| spectrum | canvas_run() | Yes | No | Yes | Yes |
