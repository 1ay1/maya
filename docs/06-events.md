# Event Handling

A maya program never receives events directly. The terminal's keyboard,
mouse, paste, focus, and resize input reach it through **`on()` overloads**,
one per event kind it cares about. Each one reads the model as it is when the
event arrives and turns the event into one of your `Msg` alternatives (or
ignores it), and jaal hands that message to the matching `update()` overload.
One event gives at most one message; a kind with no overload is dropped.

| Event type | Carries |
|------------|---------|
| `KeyEvent` | `key` (`CharKey` or `SpecialKey`), `mods`, `raw_sequence` |
| `MouseEvent` | `button`, `kind` (`Press`/`Release`/`Move`), `x`, `y` (1-based), `mods` |
| `PasteEvent` | `content` (the pasted bytes) |
| `FocusEvent` | `focused` (gained / lost) |
| `ResizeEvent` | `width`, `height` |

Together they are `maya::terminal_events` (the same type as `maya::Event`).

!!! note "What changed"
    The callback loops (`run(cfg, event_fn, render_fn)`, `canvas_run`),
    `key_map<Msg>`, the `on_key`/`on_mouse`/… subscription sources and
    `maya::set_mouse()` are gone. Events now reach a program only through its
    `on()` overloads, and changing the terminal (mouse capture, clipboard) is
    a `Cmd` returned from `update()`. The `Event` helpers (`key(ev, …)`,
    `mouse_clicked(ev)`, …) remain for widgets, whose `handle()` takes an
    event.

## One Overload Per Kind

Write an `on(const Model&, const E&)` overload for each event kind the
program handles. No `Sub` and no `subscribe()` are needed for input:

```cpp
#include <maya/host/run.hpp>
using namespace maya;
using namespace maya::dsl;

struct Editor {
    struct Model { std::string text; int w = 0, h = 0; };
    struct Typed { KeyEvent key; };
    struct Pasted { std::string text; };
    struct Resized { int w, h; };
    struct Quit {};
    using Msg = std::variant<Typed, Pasted, Resized, Quit>;

    using Cmd = jaal::Cmd<Msg>;

    static Cmd update(Model& m, Typed t);
    static Cmd update(Model& m, Pasted p)  { m.text += p.text; return {}; }
    static Cmd update(Model& m, Resized r) { m.w = r.w; m.h = r.h; return {}; }
    static Cmd update(Model&, Quit)        { return Cmd::quit(0); }

    static Element view(const Model& m);

    static std::optional<Msg> on(const Model&, const KeyEvent& k) {
        if (ctrl_is(k, 'c')) return Quit{};
        return Typed{k};
    }
    static std::optional<Msg> on(const Model&, const PasteEvent& p) {
        return Pasted{p.content};
    }
    static std::optional<Msg> on(const Model&, const ResizeEvent& r) {
        return Resized{r.width.value, r.height.value};
    }
};

int main() { return run<Editor>({.title = "editor"}); }
```

Return `std::nullopt` to ignore an event. Mouse and focus events go nowhere
here because there's no overload for them.

The model is the live one, so state-dependent input (a modal's keys, a
drag-in-progress) is just an `if` on `m` inside `on()`.

A program may also declare `using Event = std::variant<KeyEvent, ResizeEvent>;`
(or `maya::terminal_events`). `jaal::headless<App>` / `jaal::sim<App>` pick it
up, and a listed kind with no `on()` overload is a compile error.

## Keyboard Events

### keymap\<Msg\>: Simple Key-to-Message Mapping

For straightforward bindings, `keymap<Msg>` is a table. Keep it as a
function-local static and call it with the event:

```cpp
static std::optional<Msg> on(const Model&, const KeyEvent& k) {
    static const keymap<Msg> keys{
        {'q', Quit{}},
        {'+', Increment{}}, {'-', Decrement{}},
        {SpecialKey::Up, MoveUp{}}, {SpecialKey::Escape, Quit{}},
    };
    return keys(k);
}
```

Each entry matches a **plain** key with no modifiers, so `'q'` doesn't also
fire on Alt+Q. Lookup is O(1).

A key that could never match won't compile: `'\t'`, `'\r'`, `'\x1b'` and
`'\x7f'` arrive as `SpecialKey::Tab`, `Enter`, `Escape` and `Backspace`, and
other control bytes arrive as Ctrl+letter (match those with `ctrl_is`). A key
listed twice throws, since the second entry could never fire.

**Ctrl+C.** Raw mode delivers ^C as a key, not SIGINT. If your `on()` doesn't
take it, the program stops with exit 130 and the terminal restored, the same
as an unhandled SIGINT. Handle it (`if (ctrl_is(k, 'c')) return Quit{};`) to
decide yourself, or run with `run<App>({}, {.default_signal_exit = false})`
to make it plain input.

Timers are separate. They still go in `subscribe()`:

```cpp
using Sub = jaal::Sub<Msg>;
static Sub subscribe(const Model&) { return Sub::every(16ms, Tick{}); }
```

### Key Predicates

For anything richer than a table (modifiers, ranges, a fallback), write it
out in `on()` and use the predicates. They take a `KeyEvent&`:

```cpp
key_is(k, 'q')                  // plain 'q' (no modifiers)
key_is(k, SpecialKey::Enter)    // Enter
key_is(k, SpecialKey::Up)       // arrow up
ctrl_is(k, 'c')                 // Ctrl+C
alt_is(k, 'x')                  // Alt+X
```

```cpp
static std::optional<Msg> on(const Model&, const KeyEvent& k) {
    if (key_is(k, 'q') || ctrl_is(k, 'c')) return Quit{};
    if (key_is(k, SpecialKey::Tab))        return NextTab{};
    for (char c = '1'; c <= '8'; ++c)
        if (key_is(k, c)) return GotoTab{c - '1'};
    return Scroll{k};              // everything else goes to the scroll view
}
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

A `KeyEvent` is a plain struct; pass it through in a message when `update()`
needs the whole thing (a text input, a scroll view):

```cpp
struct KeyEvent {
    Key         key;           // std::variant<CharKey, SpecialKey>
    Modifiers   mods;          // .ctrl, .alt, .shift, .super_
    std::string raw_sequence;  // the bytes that produced it
};

if (auto* ch = std::get_if<CharKey>(&k.key)) {
    // ch->codepoint is a char32_t
}
```

`Options::enhanced_keyboard` (on by default) negotiates the kitty keyboard
protocol where supported, so chords legacy encoding can't express
(Ctrl+/, Ctrl+Tab, Shift+Enter) and a bare Esc arrive unambiguously.
Terminals that don't support it ignore the request.

## Mouse Events

Mouse reporting is off by default. Turn it on with `.mouse = true` in
`Options` and add a `MouseEvent` overload:

```cpp
static std::optional<Msg> on(const Model&, const KeyEvent& k) {
    static const keymap<Msg> keys{{'q', Quit{}}};
    return keys(k);
}
static std::optional<Msg> on(const Model&, const MouseEvent& me) {
    if (me.button == MouseButton::ScrollUp)   return ZoomIn{};
    if (me.button == MouseButton::ScrollDown) return ZoomOut{};
    if (me.kind == MouseEventKind::Press && me.button == MouseButton::Left)
        return Click{me.x.value, me.y.value};      // 1-based
    return std::nullopt;
}

int main() { return run<Mandel>({.title = "mandelbrot", .mouse = true}); }
```

```cpp
struct MouseEvent {
    MouseButton    button;  // Left, Right, Middle, ScrollUp, ScrollDown,
                            // ScrollLeft, ScrollRight, None
    MouseEventKind kind;    // Press, Release, Move
    Columns        x;       // x.value: 1-based column
    Rows           y;       // y.value: 1-based row
    Modifiers      mods;    // .ctrl, .alt, .shift
};
```

Motion without a button held is only reported when `.hover_motion = true`.

### Mouse Capture vs. Native Terminal Scroll

While mouse reporting is on, the terminal delivers the scroll **wheel** to your
program (as mouse buttons), so the terminal's own scrollback and text
selection stop working until the program exits. This is the terminal mouse
protocol, not maya — no program can have in-app clicks *and* native scrollback
at the same time. Capture is always released on exit.

To switch at runtime, list the `set_mouse` terminal effect in your `Cmd` and
return `SetMouse{bool}` from `update()`:

```cpp
using Cmd = jaal::Cmd<Msg, set_mouse>;

static Cmd update(Model& m, ToggleMouse) {
    m.mouse = !m.mouse;
    return Cmd(SetMouse{m.mouse});   // false: terminal scrolls/selects again
}
```

Capture starts as `Options::mouse` says. If a program doesn't need the mouse,
leave `mouse = false` (the default) and native terminal scroll works untouched.

## Scroll Views

Scrollable regions keep their offset in a `ScrollState` that lives **in the
model**, marked `mutable` because the renderer writes the measured content
extent (`max_y`) back into it after layout — that is how scrolling clamps
with no code of yours.

- **Mouse:** the Screen forwards wheel and scrollbar drag to every painted
  `ScrollState` automatically. You don't need a `MouseEvent` overload for it
  (just run with `.mouse = true`).
- **Keys:** keys are the program's. Route them with a message and call
  `state.handle(key, viewport_h)` in `update()`.

```cpp
constexpr int kViewportH = 8;

struct ScrollClip {
    struct Model { mutable ScrollState state; };
    struct Scroll { KeyEvent key; };
    struct Quit {};
    using Msg = std::variant<Scroll, Quit>;
    using Cmd = jaal::Cmd<Msg>;

    static Cmd update(Model& m, Scroll s) { (void)m.state.handle(s.key, kViewportH); return {}; }
    static Cmd update(Model&, Quit)       { return Cmd::quit(0); }

    static Element view(const Model& m) {
        return h(
            content() | scroll(m.state, kViewportH) | grow_<1>,
            scrollbar_y(m.state, kViewportH));
    }

    static std::optional<Msg> on(const Model&, const KeyEvent& k) {
        if (key_is(k, 'q')) return Quit{};
        return Scroll{k};
    }
};

int main() { return run<ScrollClip>({.title = "scroll", .mouse = true}); }
```

`handle(key, viewport_h, viewport_w)` understands arrows, PageUp/PageDown
(one viewport), Home/End, and Ctrl+Home/Ctrl+End, and returns whether it
consumed the key.

## Resize Events

```cpp
static std::optional<Msg> on(const Model&, const ResizeEvent& r) {
    return Resized{r.width.value, r.height.value};
}
```

Layout already adapts to the terminal size on its own; handle
`ResizeEvent` only when the **model** depends on the size (a simulation grid,
a pixel buffer sized to the screen).

## Paste Events

If the terminal supports bracketed paste, a paste arrives as one
`PasteEvent` instead of a flood of key events:

```cpp
static std::optional<Msg> on(const Model&, const PasteEvent& p) {
    return Pasted{p.content};
}
```

### Clipboard Reads and Image Paste over SSH

The `query_clipboard` terminal effect asks the terminal to send its clipboard
back, and the reply arrives as a **`PasteEvent`** — so add a `PasteEvent`
overload to receive it. This works across SSH with **no remote clipboard
tool**, because the request and reply travel in-band over the terminal escape
channel.

```cpp
using Cmd = jaal::Cmd<Msg, query_clipboard>;

static Cmd update(Model&, AskClipboard) { return Cmd(QueryClipboard{}); }
```

Maya picks the read protocol from the host terminal:

- **OSC 52** (the portable default) — a *text-only* protocol. Its reply can
  never carry image bytes, so on a plain terminal the query returns text.
- **OSC 5522** (kitty's multi-format clipboard read) — the only in-band escape
  path that can carry **image** bytes. When maya detects a kitty host it sends
  this instead, reassembles the chunked `status=OK` / `DATA` / `DONE` reply,
  and delivers the decoded bytes as one `PasteEvent`. This is what makes
  **screenshot paste work over SSH**: the image never touches a local file or
  a remote clipboard helper.

Detection is by environment, not a round-trip probe: maya emits the OSC 5522
read only when `KITTY_WINDOW_ID` is set or `TERM` looks like kitty (an sshd
forwards those env vars but not arbitrary terminal capabilities). Everywhere
else it falls back to OSC 52 text.

!!! note "The app-facing shape is the same either way"
    A `PasteEvent` carries opaque `content` bytes — for an image the bytes
    *are* the image (e.g. PNG). You handle both with the same `PasteEvent`
    overload; sniff the payload (magic bytes) if you need to tell text
    from an image.

## Focus Events

```cpp
static std::optional<Msg> on(const Model&, const FocusEvent& f) {
    return f.focused ? Msg{Focused{}} : Msg{Blurred{}};
}
```

A common use is pausing an animation while the terminal is in the
background: keep a `focused` flag in the model and only include
`Sub::every` in `subscribe()` while it's set.

## Input Parsing Internals

The input parser is a state machine that handles:

- **Ground** → Normal character input
- **Escape** → After receiving `ESC`
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

You never interact with `InputParser` directly — the Screen parses input and
your `on()` overloads receive the results.
