# Rewriting a maya example the jaal way — agent brief

**Token budget matters: you have a hard output limit.** Work in this order
and do not deviate:

1. `cat` the original ONCE, whole (`cat examples/NAME.cpp`). Do not re-read
   it in pieces afterwards: take notes of every key, feature and constant
   the first time.
2. Read ONE reference once: `examples/jaal_breakout.cpp` for games/pixel
   demos, or `examples/jaal_proc_table.cpp` for text/widget UIs. Skim the
   API list below instead of reading headers.
3. Write the new file in ONE `write` call. Then build; fix errors with
   small `edit`s. Never rewrite the whole file twice.
4. Smoke + CPU, then a SHORT report (under 15 lines).

API you need (don't grep for it):
- `maya::Image img(w, h, Rgb{..});  img(x, y) = Rgb{r, g, b};  pixels(std::move(img))`
  (header `<maya/element/pixels.hpp>`; 2 image pixels per terminal row).
- `maya::run_jaal<App>({.title = "x"})`, `maya::jaal_key_map<Sub>({{'q', Quit{}}, {SpecialKey::Left, X{}}})`
  (header `<maya/jaal/host.hpp>`), sources `on_key`, `on_resize`, `on_mouse`, `on_paste`.
- `Sub::every(std::chrono::milliseconds(16), Tick{})`, `Sub::batch(a, b, c)`,
  `Sub::on(on_resize{}, [](const ResizeEvent& r) -> std::optional<Msg> { return Resize{r.width.value, r.height.value}; })`.
- DSL (`using namespace maya::dsl;`): `v(a, b)`, `h(a, b)`, `text(std::string)`,
  `| Bold`, `| Dim`, `| fgc(Color::rgb(r,g,b))`, `| bgc(...)`, `| pad<1>`,
  `| border_<Round>`, `spacer()`, `zstack({a, b})`, `center()(child)`.

You are rewriting ONE maya example program as a jaal program. Repository:
`~/projects/maya` (branch `jaal-rewrite`). Read this whole brief first.

## What "the jaal way" means (non-negotiable)

A jaal program is one struct with this exact shape:

```cpp
struct App {
    struct Model { ... };                        // ALL state. A value type.
    using Msg = std::variant<Tick, Resize, ..., Quit>;   // every event is a message
    using Cmd = jaal::Cmd<Msg>;                  // effects returned from update
    using Sub = jaal::Sub<Msg, maya::on_key, maya::on_resize /*, on_mouse, on_paste */>;

    static Cmd update(Model& m, Tick);           // ONE overload per message type
    static Cmd update(Model& m, Resize r);       // (a missing one is a compile error)
    static Cmd update(Model&, Quit) { return Cmd::quit(0); }

    static maya::Element view(const Model& m);   // PURE: model -> element, no mutation
    static Sub subscribe(const Model& m);        // where messages come from
    static bool subs_key(const Model&) { return true; }   // if subscribe() never changes
};
int main() { return maya::run_jaal<App>({.title = "..."}); }
```

Rules:
- **No globals.** Every piece of state (including RNGs, timers, scroll
  positions, animation phases) is a field of `Model`. A `static` mutable
  variable anywhere is a bug. Constant tables (`constexpr`) are fine.
- **update() is the only mutator.** view() takes `const Model&`.
- **Time is a subscription:** `Sub::every(16ms, Tick{})` (use the rate the
  original used: `.fps = N` means `1000/N` ms). Keys: `maya::jaal_key_map<Sub>({{'q', Quit{}}, {SpecialKey::Left, Move{-1}}, ...})`.
  Resize: `Sub::on(on_resize{}, [](const ResizeEvent& r) -> std::optional<Msg> { return Resize{r.width.value, r.height.value}; })`.
  Mouse: `Sub::on(on_mouse{}, [](const MouseEvent& e) -> std::optional<Msg> {...})`.
- **Keys are intents**, not raw events: `Move{-1}`, `Launch{}`, `NextPalette{}`.
- **Pixel graphics use `pixels()`**: build a `maya::Image(w, h)` (RGB,
  `img(x, y) = Rgb{r, g, b}`) in view() and return `pixels(std::move(img))`.
  Two image pixels per terminal row (upper half block); size the model's
  field as `w = cols`, `h = (rows - status_rows) * 2`. NEVER intern styles,
  never hold style ids, never write a paint callback.
- **Text UI uses maya's DSL** (`v(...)`, `h(...)`, `text(...)`, `| Bold`,
  `| fgc(Color::rgb(..))`, `| bgc(..)`, `| pad<1>`, `| border_<Round>`,
  `spacer()`, `zstack({...})`, `center()(child)`, widgets from
  `<maya/widget/...>`). Look at what the original used.
- **Phases/modes as enums** (e.g. `enum class Phase { Serving, Playing, Over }`),
  not piles of booleans.
- Keep ALL the original's features, keys, visuals and behaviour. Read the
  original fully first. The status bar / help text must still be there.
- Keep it readable: a header comment explaining the program and its keys,
  short comments where a rule isn't obvious. No dead code.

## Reference programs (read both before starting)

- `examples/jaal_doomfire2.cpp` — a simulation/animation with pixels().
- `examples/jaal_breakout.cpp` — a game with phases, entities, overlay card.
- `examples/jaal_widgets.cpp`, `examples/jaal_proc_table.cpp` — widget/text UIs.
- `include/maya/element/pixels.hpp` — the Image / pixels() API.
- `include/maya/jaal/host.hpp` — run_jaal, jaal_key_map, on_key etc.

## What to write

Write `examples/jaal_NAME.cpp` (overwrite it if it exists: the existing one,
if any, is a generated wrapper and must be REPLACED by a real rewrite).
Do NOT modify the original `examples/NAME.cpp`, any header in `include/`,
the CMake files, or any other example. Only your one file.

## How to build and test (do NOT use cmake/ninja: other agents share the tree)

```sh
cd ~/projects/maya
sh tools/build_one.sh NAME                      # -> /tmp/jaalbin/NAME
python3 tests/jaal_smoke.py /tmp/jaalbin/NAME --keys="KEYS" --animates="KEY"
```

`--keys` = keys that visibly change the screen; `--animates` = a key to press
before checking it animates on its own (omit for static UIs; add
`--min-frames=2` for slow movers; `--idle-cpu=0.9` only for ray tracers).
The smoke run must end with `NAME: ok`. Fix every compile error and every
FAIL. Also run the ORIGINAL for comparison:
`python3 tests/jaal_smoke.py ~/projects/maya/build-jaal/maya_NAME --keys=... `
and compare behaviour.

Also measure CPU and compare to the original:
`python3 tests/cpu_at.py /tmp/jaalbin/NAME xterm-256color` and
`python3 tests/cpu_at.py ~/projects/maya/build-jaal/maya_NAME xterm-256color`.
The rewrite must not be meaningfully slower (within ~1.3x of the original's
CPU at the same fps). If it is, profile and fix (e.g. don't rebuild big
tables per frame; precompute palettes as constexpr/static const tables).

## Report back (short)

1. Final smoke output line and the exact smoke command you used.
2. CPU: original vs rewrite (the two cpu_at lines).
3. Anything from the original you could NOT preserve, and why (should be nothing).
4. Line count of the new file.
Do NOT commit. Do NOT touch any other file.
