# Event Handling

maya's event system gives you keyboard, mouse, paste, focus, and resize events
through a unified `Event` variant type and a set of ergonomic predicate/helper
functions.

## Program Apps: Declarative Event Handling

In the Program architecture (`run<P>(RunConfig)`), event handling is declarative.
You define a `subscribe()` function that returns a `Sub<Msg>` describing which
events map to which messages. The framework dispatches matched messages to your
`update()` function.

### key_map: Simple Key-to-Message Mapping

For straightforward key bindings, use `key_map<Msg>()`:

```cpp
static auto subscribe(const Model&) -> Sub<Msg> {
    return key_map<Msg>({
        {'q', Quit{}}, {'+', Increment{}},
        {SpecialKey::Up, MoveUp{}},
    });
}
```

### Sub::on_key: Complex Key Matching

For more complex matching logic, use `Sub<Msg>::on_key()`:

```cpp
Sub<Msg>::on_key([](const KeyEvent& k) -> std::optional<Msg> {
    if (key_is(k, 'q')) return Quit{};
    if (ctrl_is(k, 'c')) return Quit{};
    return std::nullopt;
})
```

The predicates `key_is()`, `ctrl_is()`, and `alt_is()` work on `KeyEvent&`
(not `Event&`) and are designed for use inside `Sub<Msg>::on_key()` filters.

## The Event Type

```cpp
using Event = std::variant<KeyEvent, MouseEvent, PasteEvent, FocusEvent, ResizeEvent>;
```

You never inspect the variant directly — instead, use the predicate functions.

## Keyboard Events

### Key Predicates

```cpp
key(ev, 'q')                   // Was 'q' pressed?
key(ev, '+')                   // Was '+' pressed?
key(ev, SpecialKey::Escape)    // Was Escape pressed?
key(ev, SpecialKey::Enter)     // Was Enter pressed?
key(ev, SpecialKey::Up)        // Was arrow up pressed?
```

### Modifier Keys

```cpp
ctrl(ev, 'c')    // Ctrl+C
ctrl(ev, 's')    // Ctrl+S
alt(ev, 'x')     // Alt+X
shift(ev, SpecialKey::Tab)  // Shift+Tab (BackTab)
```

### Any Key

```cpp
any_key(ev)      // Was any key pressed?
```

### Special Keys

```cpp
SpecialKey::Up, Down, Left, Right
SpecialKey::Home, End
SpecialKey::PageUp, PageDown
SpecialKey::Tab, BackTab
SpecialKey::Backspace, Delete, Insert
SpecialKey::Enter, Escape
SpecialKey::F1 ... F12
```

### Raw Key Access

```cpp
if (auto* ke = as_key(ev)) {
    // ke->key is the Key variant (CharKey or SpecialKey)
    // ke->mods has .ctrl, .alt, .shift, .super_
    // ke->raw_sequence is the raw ANSI bytes
}
```

## Fire-and-Forget Handlers (canvas_run / legacy callbacks)

The `on()` helper combines a predicate with an action — returns `bool` (true
if matched). This is used in `canvas_run()` event callbacks:

```cpp
// Single key
on(ev, 'q', [&] { should_quit = true; });

// Two keys (either matches)
on(ev, '+', '=', [&] { count++; });
on(ev, '-', '_', [&] { count--; });

// Special key
on(ev, SpecialKey::Enter, [&] { submit(); });
```

### on() in Practice

In `canvas_run()`, the event handler is a function that receives events. Use
`on()` for each action:

```cpp
[&](const Event& ev) {
    on(ev, '+', '=', [&] { count.update([](int& n) { ++n; }); });
    on(ev, '-', '_', [&] { count.update([](int& n) { --n; }); });
    on(ev, 'r',      [&] { count.set(0); });
    on(ev, 't',      [&] { theme = (theme + 1) % kThemeCount; });
    return !key(ev, 'q');  // false = quit
}
```

## Mouse Events

Enable mouse events in the config:

```cpp
run<P>({.mouse = true});
// or
canvas_run({.mouse = true}, ...);
```

### Mouse Predicates

```cpp
mouse_clicked(ev)                        // Left button clicked
mouse_clicked(ev, MouseButton::Right)    // Right button clicked
mouse_clicked(ev, MouseButton::Middle)   // Middle button clicked
mouse_released(ev)                       // Left button released
mouse_moved(ev)                          // Mouse moved
scrolled_up(ev)                          // Scroll wheel up
scrolled_down(ev)                        // Scroll wheel down
```

### Mouse Position

```cpp
if (auto pos = mouse_pos(ev)) {
    int col = pos->col;   // 1-based column
    int row = pos->row;   // 1-based row
}
```

### Raw Mouse Access

```cpp
if (auto* me = as_mouse(ev)) {
    // me->button: MouseButton enum
    // me->kind: MouseEventKind (Press, Release, Move)
    // me->x: Columns, me->y: Rows
    // me->mods: Modifiers (.ctrl, .alt, .shift)
}
```

### Mouse Example (canvas_run callback)

```cpp
canvas_run(
    {.mouse = true},
    on_resize,
    [&](const Event& ev) {
        if (mouse_clicked(ev)) {
            auto pos = mouse_pos(ev);
            if (pos) {
                click_x = pos->col;
                click_y = pos->row;
            }
        }
        if (scrolled_up(ev))   zoom_in();
        if (scrolled_down(ev)) zoom_out();
        return !key(ev, 'q');
    },
    on_paint
);
```

For Program apps, mouse events are handled via `Sub<Msg>::on_mouse()` in `subscribe()`.

## Resize Events

```cpp
if (resized(ev)) {
    // Terminal was resized
}

int w, h;
if (resized(ev, &w, &h)) {
    // w and h now hold the new terminal size
}
```

In `run()` with `Ctx`, the context's `size` field is automatically updated on
resize, so you usually don't need to handle this yourself.

## Paste Events

If the terminal supports bracketed paste:

```cpp
std::string pasted_text;
if (pasted(ev, &pasted_text)) {
    // pasted_text contains the clipboard content
}
```

## Focus Events

```cpp
if (focused(ev))   { /* Terminal gained focus */ }
if (unfocused(ev)) { /* Terminal lost focus */ }
```

## Event Handler Patterns

### Program Pattern: Declarative subscribe()

For `run<P>()` apps, define a `subscribe()` function:

```cpp
static auto subscribe(const Model&) -> Sub<Msg> {
    return key_map<Msg>({
        {'q', Quit{}}, {'+', Increment{}}, {'-', Decrement{}},
        {'r', Reset{}}, {'t', CycleTheme{}},
    });
}

static auto update(Model model, Msg msg) -> std::pair<Model, Cmd<Msg>> {
    return std::visit(overload{
        [&](Quit)       { return std::pair{model, Cmd<Msg>::quit()}; },
        [&](Increment)  { model.count++; return std::pair{model, Cmd<Msg>::none()}; },
        // ...
    }, msg);
}
```

### canvas_run() Callback Patterns

These patterns apply to `canvas_run()` event callbacks.

#### Pattern 1: Bool Return (Quit Control)

```cpp
// Return false to quit, true to continue
[&](const Event& ev) -> bool {
    on(ev, '+', [&] { count++; });
    on(ev, '-', [&] { count--; });
    if (key(ev, 'q') || key(ev, SpecialKey::Escape)) return false;
    return true;
}
```

#### Pattern 2: Void Return (Use quit())

```cpp
// Call maya::quit() to exit
[&](const Event& ev) {
    on(ev, '+', [&] { count++; });
    on(ev, '-', [&] { count--; });
    on(ev, 'q', [] { quit(); });
}
```

#### Pattern 3: Complex Event Dispatch

```cpp
[&](const Event& ev) {
    // Key shortcuts
    on(ev, 't', [&] { theme = (theme + 1) % kThemeCount; });
    on(ev, 'T', [&] { theme = (theme + 1) % kThemeCount; });
    on(ev, 'p', [&] { paused = !paused; });
    on(ev, ' ', [&] { trigger_action(); });

    // Speed control
    on(ev, '+', '=', [&] { speed = std::min(5.0f, speed + 0.25f); });
    on(ev, '-', '_', [&] { speed = std::max(0.1f, speed - 0.25f); });

    // Mouse
    if (auto pos = mouse_pos(ev)) {
        hover_col = pos->col - 1;
    }
    if (mouse_clicked(ev)) {
        auto pos = mouse_pos(ev);
        if (pos) do_click(pos->col, pos->row);
    }
    if (scrolled_up(ev))   speed *= 1.25f;
    if (scrolled_down(ev)) speed *= 0.8f;

    // Quit
    return !(key(ev, 'q') || key(ev, SpecialKey::Escape));
}
```

## Input Parsing Internals

maya's `InputParser` is a state-machine that parses raw terminal bytes into
structured events:

- **Ground** → Normal character input
- **Escape** → Start of escape sequence
- **CSI** → Control Sequence Introducer (`ESC [`)
- **SS3** → Single Shift 3 (`ESC O`) — some function keys
- **OSC** → Operating System Command
- **BracketedPaste** → Paste content between markers

The parser handles:
- UTF-8 multi-byte sequences
- CSI parameter parsing (cursor keys, function keys, modifiers)
- SGR mouse reports (click, release, move, scroll with position)
- Bracketed paste (start/end markers)
- Focus events (in/out)
- Ambiguous Escape (50ms timeout to distinguish ESC key from escape sequence)

You never interact with `InputParser` directly — the framework handles it.
