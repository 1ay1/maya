# Getting Started

This is the tutorial. By the end you will have built four working apps — a
static panel, an interactive counter, the same counter rebuilt in the
testable **Program** architecture, and a small but complete to-do app — and
you will understand *every line* of each. Nothing here is hand-waved: every
type, every operator, every function call is explained where it first appears.

Work through it top to bottom with a terminal open. The whole thing takes
about 30 minutes and leaves you able to read any maya example and write your
own.

---

## 1. Install

### What you need

- **A C++26 compiler** — GCC 15+ or Clang 19+ (maya uses concepts, structural
  NTTPs, and `consteval`, so an older compiler will not build it).
- **CMake 3.28+**.
- **A Unix-like OS** — Linux or macOS. maya talks to the terminal through raw
  mode, `ioctl(TIOCGWINSZ)`, and POSIX signals.
- **Nothing else.** maya has zero external dependencies — no ncurses, no Boost,
  no package manager step. The whole library is in this one repository.

### Build the library and examples

```bash
git clone <repo-url> maya
cd maya
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

`-B build` puts all generated files in a `build/` directory (out-of-source, so
your tree stays clean). `--build build -j$(nproc)` compiles using one job per
CPU core. When it finishes you have `build/libmaya.a` (the static library you
link against) and every example binary.

Run one to confirm it works:

```bash
./build/maya_counter      # a minimal interactive counter
./build/maya_snake        # Snake, drawn on the canvas API
./build/maya_dashboard    # a multi-panel system dashboard
```

Press `q` to quit any of them.

### Build options

You only need these later, but here they are in one place:

| Option | Default | What it does |
|--------|---------|--------------|
| `MAYA_BUILD_EXAMPLES` | `ON` | Build the example programs. |
| `MAYA_BUILD_TESTS` | `OFF` | Build + register the test suite (`ctest`). |
| `MAYA_FAST_TESTS` | `ON` | When tests are on, link them against a non-LTO `-O1` library for fast iteration. Production `maya` is unaffected. |
| `MAYA_NATIVE_TUNING` | `ON` | Bake the host CPU in (`-march=native`). **Turn OFF for portable release builds** — a `native` binary will `SIGILL` on a chip missing the build host's instructions. |

### Use maya in your own project

The simplest path — vendor maya as a subdirectory:

```cmake
add_subdirectory(vendor/maya)         # builds libmaya.a as part of your build
add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE maya)   # the `maya` target carries all include paths + flags
```

Linking the `maya` target is all you need — it propagates the include
directories and the `-std=c++2c` requirement to your target automatically.

Or install it system-wide and find it:

```bash
cmake --install build --prefix /usr/local
```

```cmake
find_package(maya REQUIRED)
target_link_libraries(myapp PRIVATE maya::maya)
```

---

## 2. The two namespaces

Before any code: maya lives in two namespaces, and almost every program opens
both.

```cpp
using namespace maya;       // the runtime: run(), Cmd, Sub, Signal, Event, quit()…
using namespace maya::dsl;  // the UI vocabulary: t<>, v(), h(), Bold, Fg<>, border_<>…
```

- **`maya::dsl`** is everything you use to *describe* a UI — text nodes, layout
  containers, style tags, and the modifier operators that compose them.
- **`maya`** is everything that *runs* a UI — the entry-point functions, the
  message/effect types, reactive signals, and event helpers.

The split is deliberate: the `dsl` half is pure compile-time machinery that
produces data; the `maya` half is the engine that turns that data into pixels
and feeds it events. You will use both in the same file constantly.

---

## 3. Your first app — a static panel

Create `main.cpp`:

```cpp
#include <maya/maya.hpp>          // (1) the single umbrella header

using namespace maya;             // (2)
using namespace maya::dsl;        // (3)

int main() {
    print(                        // (4)
        (v(                       // (5)
            t<"Hello, maya!"> | Bold | Fg<100, 180, 255>,   // (6)
            t<"A C++26 TUI framework."> | Dim               // (7)
        ) | border_<Round> | pad<1>).build()                // (8)
    );
}
```

Line by line:

1. **`#include <maya/maya.hpp>`** — the umbrella header. It pulls in the DSL,
   the layout engine, the renderer, and the runtime. You never need to include
   anything else for normal app code.
2. **`using namespace maya;`** — brings `print` (and later `run`, `Signal`, …)
   into scope.
3. **`using namespace maya::dsl;`** — brings `t<>`, `v`, `Bold`, `Fg`,
   `border_`, `pad` into scope.
4. **`print(...)`** — the simplest of maya's four entry points. It renders an
   `Element` once to stdout and returns. No event loop, no raw mode — the
   output becomes ordinary scrollback text, exactly like `echo`. Perfect for
   CLI output that happens to be styled.
5. **`v(...)`** — a **v**ertical stack (a column). Its arguments are its
   children, stacked top to bottom. (`h(...)` is the horizontal sibling.)
6. **`t<"Hello, maya!">`** — a compile-time **t**ext node. The string lives in
   the *template parameter*, so it is baked into the binary with zero runtime
   cost. `| Bold` makes it bold; `| Fg<100, 180, 255>` sets its foreground to
   RGB(100,180,255). The `|` operator is how you attach style/layout modifiers
   to any node — read it as "with".
7. **`t<"…"> | Dim`** — a second text node, dimmed. Because it is the second
   argument to `v(...)`, it renders on the row below the first.
8. **`| border_<Round> | pad<1>`** then **`.build()`** — modifiers applied to
   the *whole stack*: `border_<Round>` wraps it in a rounded box;
   `pad<1>` adds one cell of padding on every side (between the border and the
   text). `.build()` converts the compile-time DSL tree into a runtime
   `Element` — which is what `print` actually consumes. **This `.build()` call
   is the bridge from the compile-time world to the runtime world**, and you
   will see it at the end of nearly every render expression.

Compile and run:

```bash
g++ -std=c++2c main.cpp -I maya/include -L maya/build -lmaya -o myapp
./myapp
```

(Or wire it through CMake as shown in §1 — far easier once you have more than
one file.)

You get a rounded box with two lines of styled text, printed inline. There is
no "press q to quit" because `print` does not loop — it draws once and exits.

!!! tip "The `.build()` rule"
    DSL expressions are compile-time *nodes*. The runtime wants *elements*. So
    a render expression almost always ends in `.build()`. If the compiler
    complains that it wanted an `Element` but got some long template type, you
    forgot `.build()`.

---

## 4. Making it interactive — the simple `run()`

`print` draws once. To handle keystrokes and redraw, use **`run()`** — maya's
simplest *interactive* entry point. It takes two lambdas: an **event handler**
and a **render function**, and loops between them until you quit.

Here is a counter you can increment with `+` and quit with `q`:

```cpp
#include <maya/maya.hpp>
using namespace maya;
using namespace maya::dsl;

int main() {
    int count = 0;                                  // (1) app state — a plain local

    run(
        [&](const Event& ev) {                      // (2) event handler
            on(ev, '+', '=', [&] { ++count; });     // (3) + or = → increment
            return !key(ev, 'q');                   // (4) keep running until 'q'
        },
        [&] {                                        // (5) render function
            return (v(
                text("Count: " + std::to_string(count)) | Bold,  // (6) dynamic text
                t<"[+] increment   [q] quit"> | Dim               // (7) static hint
            ) | border_<Round> | pad<1>).build();
        }
    );
}
```

1. **`int count = 0;`** — your application state. In the simple `run()` style,
   state is just ordinary variables captured by reference in the lambdas. (The
   Program architecture in §5 makes this state explicit and testable; for small
   apps, a local is fine.)
2. **The event handler** `[&](const Event& ev) -> bool`. maya calls it once for
   every input event (a keypress, a mouse action, a resize). The `[&]` capture
   gives it access to `count`. **Its return value controls the loop: `true`
   means keep running, `false` means quit.**
3. **`on(ev, '+', '=', [&] { ++count; })`** — the `on()` helper fires its
   callback if the event matches *any* of the listed keys. Here both `+` and
   `=` (the unshifted `+` key) increment the counter. `on()` returns whether it
   matched, which you can chain or ignore. This is the readable way to bind
   keys without writing `if (key(ev, ...))` by hand.
4. **`return !key(ev, 'q');`** — `key(ev, 'q')` is `true` when the event is the
   `q` key. Negated, the handler returns `false` on `q` (→ quit) and `true`
   otherwise (→ keep going).
5. **The render function** `[&] -> Element`. maya calls it after every event to
   redraw. It returns the current frame as an `Element`.
6. **`text("Count: " + std::to_string(count))`** — note this is `text(...)`
   (lowercase, runtime) not `t<...>` (compile-time). Use `text()` whenever the
   string is computed at runtime — here it embeds the live `count`. `| Bold`
   styles it.
7. **`t<"[+] increment   [q] quit">`** — a *static* hint line; the string never
   changes, so the compile-time `t<>` is ideal. `| Dim` de-emphasises it.

Build and run it; press `+` a few times and watch the count climb, then `q` to
quit. The terminal redraws in place — only the changed cells are written to the
wire (maya diffs every frame), so it is smooth even at speed.

### `t<>` vs `text()` — the one distinction that trips everyone up

| | When the string is… | Cost |
|---|---|---|
| **`t<"literal">`** | known at compile time | baked into the binary, zero runtime work |
| **`text(runtime_string)`** | computed at runtime | constructs a node each frame |

Reach for `t<>` for fixed labels and hints; reach for `text()` for anything
interpolated. Mixing them in one `v(...)` is normal and expected (line 6 vs 7
above).

---

## 5. The Program architecture — structure that scales

The simple `run()` is great for a screenful of state. Once your app grows —
multiple kinds of state, effects like timers or network calls, logic you want
to unit-test — reach for the **Program** architecture. It is maya's take on the
Elm pattern: your app is four pure functions over an explicit `Model` and a
`Msg` (message) type.

Here is the same counter, rebuilt as a Program:

```cpp
#include <maya/maya.hpp>
using namespace maya;
using namespace maya::dsl;

struct Counter {
    // ── 1. Model: ALL of the app's state, in one struct ──────────────
    struct Model { int n = 0; };

    // ── 2. Msg: every event that can change the model ────────────────
    struct Inc {};
    struct Quit {};
    using Msg = std::variant<Inc, Quit>;

    // ── 3. init: the starting model ──────────────────────────────────
    static Model init() { return {}; }

    // ── 4. update: (model, msg) -> (new model, effect) ───────────────
    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        return std::visit(overload{
            [&](Inc)  { return std::pair{Model{m.n + 1}, Cmd<Msg>::none()}; },
            [](Quit)  { return std::pair{Model{},        Cmd<Msg>::quit()}; },
        }, msg);
    }

    // ── 5. view: model -> UI (a pure function, no side effects) ──────
    static Element view(const Model& m) {
        return (v(
            text("Count: " + std::to_string(m.n)) | Bold,
            t<"[+] increment   [q] quit"> | Dim
        ) | border_<Round> | pad<1>).build();
    }

    // ── 6. subscribe: model -> which events produce which Msgs ───────
    static Sub<Msg> subscribe(const Model&) {
        return key_map<Msg>({ {'+', Inc{}}, {'q', Quit{}} });
    }
};

int main() {
    run<Counter>({ .title = "counter" });   // (7) launch the Program
}
```

This looks longer than the simple version, but every piece earns its place:

1. **`Model`** — *all* state in one struct. There are no stray variables; if it
   is not in the `Model`, it is not state. This is what makes a Program
   testable: `update` is a pure function of `Model` and `Msg`.
2. **`Msg`** — a `std::variant` of every distinct thing that can happen.
   `Inc{}` and `Quit{}` are empty "tag" structs; a message that carries data
   would have fields (e.g. `struct SetName { std::string name; };`). The
   variant is a *closed* set — the compiler knows every message type, so a
   `std::visit` over it is exhaustively checked.
3. **`init()`** — returns the first `Model`. `return {};` value-initialises it,
   so `n` starts at 0.
4. **`update(Model m, Msg msg)`** — the heart of the app. Given the current
   model and a message, it returns the *next* model plus an optional **effect**
   (`Cmd<Msg>`). It is pure: same inputs → same outputs, no I/O. We
   `std::visit` the message with the `overload{...}` helper, which bundles one
   lambda per message type:
     - **`Inc`** → a new model with `n + 1`, and `Cmd<Msg>::none()` (no effect).
     - **`Quit`** → `Cmd<Msg>::quit()`, the effect that stops the runtime. The
       returned model does not matter here since we are quitting.
   `Cmd<Msg>` is **"effects as data"**: instead of *calling* `exit()`, you
   *return* a value describing the effect, and the runtime performs it. Other
   commands include `Cmd<Msg>::after(duration, msg)` (deliver a message later),
   `Cmd<Msg>::set_title(...)`, and `Cmd<Msg>::batch(...)` (several at once).
   Because effects are data, `update` stays pure and unit-testable.
5. **`view(const Model& m)`** — turns a model into an `Element`. Identical body
   to the simple version, but it now reads from `m` (the explicit model) rather
   than a captured local. **`view` is pure** — it must not mutate anything; it
   just maps state → UI. maya calls it whenever the model changes.
6. **`subscribe(const Model&)`** — declares which inputs become which messages.
   `key_map<Msg>({...})` builds a keyboard subscription from a table: pressing
   `+` dispatches `Inc{}`; pressing `q` dispatches `Quit{}`. The runtime reads
   this, watches the keyboard, and routes matching keys into `update` as
   messages. (Subscriptions can also be timers — `every(16ms, Tick{})` — or
   mouse, focus, and resize streams.) It takes the model in case the active
   subscriptions depend on state (e.g. only listen for arrow keys while a menu
   is open).
7. **`run<Counter>({ .title = "counter" })`** — launches the Program.
   `run<P>()` wires `init` → `view` → render, feeds events through `subscribe`
   → `update`, performs returned `Cmd`s, and repeats. The config struct sets
   the window title; it also carries `.mode`, `.fps`, `.mouse`, and more (see
   §7).

### Why bother with all four functions?

Because the payoff compounds as the app grows:

- **`update` is unit-testable** with no terminal: `auto [m2, cmd] = update(Model{5}, Inc{}); assert(m2.n == 6);`.
- **State is centralised** — there is exactly one source of truth (`Model`),
  not a scatter of captured variables.
- **Effects are explicit and inspectable** — a test can assert that `Quit`
  yields `Cmd::quit()` without anything actually quitting.
- **The data flow is one direction**: event → `Msg` → `update` → new `Model` →
  `view` → screen. No callback ever reaches back and mutates the UI directly.

For a counter it is overkill. For a chat client, a deploy dashboard, or
anything with timers and async work, it is the difference between a maintainable
app and a tangle of callbacks.

---

## 6. A complete small app — a to-do list

Let us put it all together: a Program with a model that holds a list, messages
that carry data, text input, and a list view. This is a real (if tiny) app.

```cpp
#include <maya/maya.hpp>
#include <string>
#include <vector>
using namespace maya;
using namespace maya::dsl;

struct Todos {
    struct Item { std::string text; bool done = false; };

    // The whole app state: the items, and what the user is currently typing.
    struct Model {
        std::vector<Item> items;
        std::string       draft;
        int               cursor = 0;   // which item the selection is on
    };

    // Messages — some carry data (Type), some are bare tags.
    struct Type   { char c; };          // a character was typed into the draft
    struct Back   {};                   // backspace in the draft
    struct Add    {};                   // commit the draft as a new item
    struct Toggle {};                   // flip done/undone on the selected item
    struct Up     {};
    struct Down   {};
    struct Quit   {};
    using Msg = std::variant<Type, Back, Add, Toggle, Up, Down, Quit>;

    static Model init() { return {}; }

    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        std::visit(overload{
            [&](Type ty) { m.draft.push_back(ty.c); },
            [&](Back)    { if (!m.draft.empty()) m.draft.pop_back(); },
            [&](Add)     {
                if (!m.draft.empty()) {
                    m.items.push_back({ m.draft, false });
                    m.draft.clear();
                }
            },
            [&](Toggle)  {
                if (!m.items.empty())
                    m.items[m.cursor].done = !m.items[m.cursor].done;
            },
            [&](Up)      { if (m.cursor > 0) --m.cursor; },
            [&](Down)    { if (m.cursor + 1 < (int)m.items.size()) ++m.cursor; },
            [](Quit)     {},   // handled by returning quit() below
        }, msg);

        if (std::holds_alternative<Quit>(msg))
            return { std::move(m), Cmd<Msg>::quit() };
        return { std::move(m), Cmd<Msg>::none() };
    }

    static Element view(const Model& m) {
        // Build one row per item. We accumulate runtime elements in a vector
        // and feed them to vec() — the runtime sibling of v().
        std::vector<Element> rows;
        for (int i = 0; i < (int)m.items.size(); ++i) {
            const auto& it = m.items[i];
            std::string mark = it.done ? "[x] " : "[ ] ";
            // Selected row is highlighted (inverse video); done rows are dimmed.
            Style s{};
            if (i == m.cursor) s = s.with_inverse();
            if (it.done)       s = s.with_dim();
            rows.push_back(text(mark + it.text, s));
        }
        if (rows.empty())
            rows.push_back(text("(no items yet — type one and press Enter)",
                                Style{}.with_dim()));

        return (v(
            t<"maya to-do"> | Bold | Fg<120, 200, 255>,
            // The draft line, with a caret so you can see where you're typing.
            text("> " + m.draft + "_"),
            t<"────────────────────────────"> | Dim,   // a simple rule
            // v() also accepts a std::vector<Element> directly — the runtime
            // list of rows we built in the loop above.
            std::move(rows),
            t<"[Enter] add   [Space] toggle   [up/down] move   [q] quit"> | Dim
        ) | border_<Round> | pad<1>).build();
    }

    static Sub<Msg> subscribe(const Model&) {
        // Combine several subscriptions with Sub::batch. They run together;
        // the FIRST one that produces a Msg for a given event wins.
        return Sub<Msg>::batch(
            // A raw key handler: inspect the KeyEvent and map printable
            // characters to Type{c} (or q → Quit). KeyEvent::key is a
            // variant<CharKey, SpecialKey>; CharKey carries a char32_t
            // codepoint. We only forward plain ASCII into the draft.
            Sub<Msg>::on_key([](const KeyEvent& k) -> std::optional<Msg> {
                if (auto* ck = std::get_if<CharKey>(&k.key)) {
                    char32_t cp = ck->codepoint;
                    if (cp == U'q') return Quit{};
                    if (cp >= U' ' && cp < 0x7f) return Type{ (char)cp };
                }
                return std::nullopt;
            }),
            // A declarative key table for the non-printable controls.
            key_map<Msg>({
                { SpecialKey::Enter,     Add{}    },
                { SpecialKey::Backspace, Back{}   },
                { ' ',                   Toggle{} },
                { SpecialKey::Up,        Up{}     },
                { SpecialKey::Down,      Down{}   },
            })
        );
    }
};

int main() {
    run<Todos>({ .title = "todos", .mode = Mode::Fullscreen });
}
```

What is new here, and why it matters:

- **Messages with payloads.** `struct Type { char c; };` carries the typed
  character. `update` reads `t.c`. This is how events become data: the
  subscription decides *what* happened, `update` decides *what it means*.
- **`std::move(m)` in the return.** `update` takes the model *by value* (a
  fresh copy it owns) and returns it moved — no heap churn, the model flows
  through the function. Mutating the local `m` and returning it is the normal
  pattern.
- **`std::move(rows)` as a `v(...)` child.** `v(...)` accepts not only fixed
  child nodes but also a `std::vector<Element>` built at runtime — it splices
  the vector's elements in as siblings. So you build your dynamic rows in a
  loop, then drop the whole vector into the stack alongside the static
  header/footer lines. (If you have *only* a vector and no static siblings,
  `map(range, projection)` builds the list in one expression — see
  [Runtime Content](05-runtime-content.md).)
- **`Style{}.with_inverse()` / `.with_dim()`** — the *runtime* styling API. The
  `t<"…"> | Bold` pipe is compile-time; when you compute a style at runtime
  (here, depending on selection and done-state) you build a `Style` value with
  the `with_*` chain and pass it to `text(string, style)`. Both reach the same
  renderer. (`with_inverse` swaps fg/bg — the classic "selected row" look.)
- **The rule line** is just a `t<>` of box-drawing characters dimmed — the
  simplest possible divider. maya also ships richer separators and dozens of
  other widgets (see the [Widget Reference](13-widgets.md)).
- **`Sub<Msg>::batch(a, b, …)`** — combines multiple subscriptions into one.
  Here a raw key handler *and* a declarative key table run together; the first
  to produce a `Msg` for a given event wins.
- **`Sub<Msg>::on_key([](const KeyEvent& k){ … })`** — the general key
  subscription: you get the raw `KeyEvent` and return `std::optional<Msg>`.
  `k.key` is a `std::variant<CharKey, SpecialKey>`; `std::get_if<CharKey>`
  pulls out a printable character (as a `char32_t codepoint`). Returning
  `std::nullopt` ignores the key. `key_map` (used below it) is just a
  convenience wrapper around this for the common key→message table case.
- **`SpecialKey::Enter` / `Backspace` / `Up` / `Down`** — non-printable keys
  are matched by enum, not by `char`.
- **`.mode = Mode::Fullscreen`** — this app takes over the whole screen (alt
  buffer). The counter used the default inline mode. The next section explains
  the difference.

Build it, type a few items, `Enter` to add each, `Space` to tick one off,
arrows to move the selection, `q` to quit. That is a complete maya app in ~90
lines, and you can read every one of them.

---

## 7. Choosing a rendering mode

maya has four entry points. You have now used three (`print`, `run(event,
render)`, `run<P>`). Here is when to reach for each:

- **`print(element)`** — draw once to stdout and return. Styled CLI output, no
  interactivity. Becomes normal scrollback text.
- **`run(event_fn, render_fn)`** — the simple interactive loop with captured
  state. Best for small apps where a Program is overkill.
- **`run<Program>(cfg)`** — the structured architecture from §5–6. Best for
  anything that grows.
- **`canvas_run(...)`** — imperative cell-by-cell painting for games,
  visualisations, and animations (Snake, Game of Life, the Mandelbrot example).
  You get a `Canvas` and set cells directly; see the [Canvas API](08-canvas-api.md).

Both `run` forms accept a config with a **mode**:

- **`Mode::Inline`** (the default) — renders *inline* in the terminal without
  the alternate screen. Scrollback above your app is preserved; completed
  content scrolls into the terminal's native history. This is how a chat or
  agent UI behaves — the app lives in the normal terminal flow.
- **`Mode::Fullscreen`** — switches to the alternate screen buffer; your app
  owns the entire viewport and the terminal has no scrollback while it runs (it
  is restored on exit). Best for dashboards, editors, games — anything
  full-screen and self-contained.

The full mode comparison, pipelines, and scrollback semantics are in
[Rendering Modes](07-rendering-modes.md). For now: **inline for flow-style
apps, fullscreen for take-over apps.**

---

## 8. Where to go next

You now know the four entry points, both styling APIs, the compile-time vs
runtime split, and the Program architecture. The rest of the guide goes deep on
each axis:

- **[The Compile-Time DSL](02-dsl.md)** — every node and pipe: `t<>`, `v`/`h`,
  `dyn`, `map`, and how the type-state validation rejects impossible UIs at
  compile time.
- **[Styling](03-styling.md)** — colours, attributes, themes, and the
  compile-time style tags.
- **[Layout](04-layout.md)** — the flexbox model: direction, grow/shrink, gap,
  alignment, wrapping, and borders.
- **[Runtime Content](05-runtime-content.md)** — `dyn()`, `text()`, `vec()`,
  `map()`, and mixing static and dynamic subtrees.
- **[Event Handling](06-events.md)** — keys, mouse, paste, focus, resize, and
  the full `on()`/`key_map`/`sub_batch` toolkit.
- **[Signals & Reactivity](09-signals.md)** — `Signal<T>`, `Computed<T>`,
  `Effect`, `Batch` for fine-grained reactive state inside the simple `run()`.
- **[Animation](14-animation.md)** — the motion framework: self-driving
  `Motion<T>`, springs, timelines, and the streaming typewriter.
- **[Widget Reference](13-widgets.md)** — the 50+ built-in widgets.
- **[Examples Walkthrough](10-examples.md)** — annotated tours of every example
  binary you ran in §1.

Read them in any order — each is self-contained. But you already have what you
need to start building. Open `examples/` and you will recognise the shape of
every program there.
