# Pixels and glyphs

Games, fractals, fire, fluids, ray tracers and digital rain are ordinary
maya programs. There is no separate canvas loop and no paint callback: the
model holds the state, `update()` advances it on a `Tick`, and `view()`
returns an element built from a picture. Two elements cover every case:

- `pixels(img)` draws an `Image` (a w × h RGB buffer), two pixels per cell.
- `glyphs(grid)` draws a `Glyphs` grid of coloured characters.

Both live in `<maya/element/pixels.hpp>`. They compose like any other
element: put a status bar under them with `v(...)`, a border around them,
or a sparse `Glyphs` over an image in a `zstack`.

> **What changed.** `canvas_run`, `CanvasConfig` and the
> `(StylePool&, int W, int H)` / `(Canvas&, int W, int H)` callbacks are
> deleted for applications. Pre-interning styles, keeping style ids in
> globals and re-interning them on resize are all gone: `pixels()` and
> `glyphs()` do the style bookkeeping for you.

## Image

```cpp
struct Rgb { std::uint8_t r = 0, g = 0, b = 0; };   // 24-bit colour

Image img(w, h);              // w × h pixels, row-major, black
Image img(w, h, Rgb{0,0,40}); // ...filled with a colour
img(x, y) = Rgb{255, 128, 0}; // read / write a pixel
img.width(); img.height(); img.empty();
img.fill(Rgb{});              // clear
img.data();                   // std::vector<Rgb>&, row-major
```

`Image` is a plain value type: copy it, keep it in a Model, compare it with
`==`.

A terminal cell is two pixels tall: `pixels()` draws each cell as the upper
half block `▀` with the foreground set to the top pixel and the background to
the bottom pixel. So a screen of `cols × rows` cells is `cols × 2·rows`
pixels:

```cpp
auto [pw, ph] = Image::for_cells(cols, rows);   // {cols, rows * 2}
```

### fill_rows: per-pixel shaders on every core

When the picture is a pure function of the model (a fractal, a ray tracer,
a plasma), compute it with `fill_rows`:

```cpp
Image img(m.w, m.h);
img.fill_rows([&](int x, int y) -> Rgb { return shade(m, x, y); });
```

Rows are spread over the machine's cores and *interleaved* (thread `t`
takes rows `t, t+n, …`), because cost varies by region and contiguous bands
would leave most threads idle behind the slowest. The function must only
read shared state. Small images (under 4096 pixels) run on one thread.

## pixels()

```cpp
pixels(Image img)                        // copies the image into the element
pixels(std::shared_ptr<const Image> img) // shares it: building the view copies a pointer
```

The element fills the space layout gives it. The image is sampled to that
size (nearest neighbour), so a model can either keep a fixed-resolution
buffer and let it stretch, or size itself to the screen by handling a resize
message (exact size = no sampling). Either way the view stays a pure function
of the model.

Cost: one style lookup per cell through a small direct-mapped cache keyed on
the packed (top, bottom) colour pair, and the frame diff sends only the cells
that changed.

### Tip: quantise colours for the style cache

Every distinct (top, bottom) pair becomes an interned style. A smooth
gradient or a tone-mapped ray tracer can produce millions of distinct pairs,
which thrashes the cache and the style pool. Dropping the low 3 bits of each
channel (5 bits per channel) is visually indistinguishable and keeps the
working set small:

```cpp
auto q = [](float f) {   // [0,1] -> 0..255, 5-bit
    return static_cast<std::uint8_t>(static_cast<int>(std::clamp(f, 0.f, 1.f) * 255.f) & 0xF8);
};
return Rgb{q(r), q(g), q(b)};
```

`raymarch.cpp` and `space3d.cpp` do exactly this. Palette-driven images
(fire, life, a 256-entry lookup table) don't need it: they already use few
colours.

## Glyphs and glyphs()

`Glyphs` is the text twin of `Image`: a `cols × rows` grid of coloured
characters, for character art such as digital rain, a starfield of `.`, `*`
and `+`, or a spectrum of block glyphs.

```cpp
struct Glyph {
    char32_t ch   = U' ';
    Rgb      fg   {200, 200, 200};
    Rgb      bg   {0, 0, 0};
    bool     bold = false;
};

Glyphs g(cols, rows);                        // filled with Glyph{}
Glyphs g(cols, rows, Glyph{U' ', {}, {0, 8, 0}});
g(x, y) = Glyph{U'ｱ', Rgb{120, 255, 140}};
if (g.in(x, y)) { /* bounds check */ }
g.text(2, 0, U"SCORE 120", Rgb{255, 255, 0}, Rgb{}, /*bold=*/true);  // clipped
```

`glyphs(std::move(g))` is the element. The grid is drawn from the top left
of its slot and is not scaled, so size it to the slot in `update()` from a
resize message, like an Image. Cells with `ch == 0` are left untouched, so a
sparse grid can sit on top of something else in a `zstack`.

## A complete program

A plasma that fills the screen, animated at ~60 Hz, with a status bar and a
pause key. The model sizes itself from `on_resize`; `view()` builds the
image; `subscribe()` stops the timer while paused, so a still screen costs
nothing.

```cpp
#include <maya/element/pixels.hpp>
#include <maya/app.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

using namespace maya;
using namespace maya::dsl;
using namespace std::chrono_literals;

struct Plasma {
    struct Model {
        int   w = 0, h = 0;   // pixels: w = columns, h = 2 × (rows - 1)
        float t = 0.f;
        bool  paused = false;
    };

    struct Tick {};
    struct Resize { int cols, rows; };
    struct Pause {};
    struct Quit {};
    using Msg = std::variant<Tick, Resize, Pause, Quit>;

    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg, on_key, on_resize>;

    static Cmd update(Model& m, Resize r) {
        m.w = std::max(1, r.cols);
        m.h = std::max(2, (r.rows - 1) * 2);   // one row of cells for the status bar
        return {};
    }
    static Cmd update(Model& m, Tick)  { m.t += 0.03f; return {}; }
    static Cmd update(Model& m, Pause) { m.paused = !m.paused; return {}; }
    static Cmd update(Model&,   Quit)  { return Cmd::quit(0); }

    static Image render(const Model& m) {
        auto q = [](float f) {   // 5-bit channels keep the style cache small
            return static_cast<std::uint8_t>(static_cast<int>(std::clamp(f, 0.f, 1.f) * 255.f) & 0xF8);
        };
        Image img(m.w, m.h);
        img.fill_rows([&](int x, int y) -> Rgb {
            const float u = static_cast<float>(x) / 16.f, v = static_cast<float>(y) / 8.f;
            const float s = std::sin(u + m.t) + std::sin(v - m.t) + std::sin((u + v) * 0.5f + m.t * 1.3f);
            return {q(0.5f + 0.5f * std::sin(s)), q(0.5f + 0.5f * std::sin(s + 2.f)), q(0.5f + 0.5f * std::sin(s + 4.f))};
        });
        return img;
    }

    static Element status_bar(const Model& m) {
        return h(text(" PLASMA") | Bold | Fg<255, 140, 40>,
                 text("  [space] pause  [q] quit") | Dim,
                 spacer(),
                 text(m.paused ? "paused " : "")) | bgc(Color::rgb(20, 20, 20));
    }

    static Element view(const Model& m) {
        if (m.w == 0) return text("");       // no size yet
        return v(pixels(render(m)), status_bar(m));
    }

    static Sub subscribe(const Model& m) {
        auto resize = Sub::on(on_resize{}, [](const ResizeEvent& r) -> std::optional<Msg> {
            return Resize{r.width.value, r.height.value};
        });
        auto keys_ = keys<Sub>({{' ', Pause{}}, {'q', Quit{}}, {SpecialKey::Escape, Quit{}}});
        if (m.paused) return Sub::batch(std::move(resize), std::move(keys_));
        return Sub::batch(Sub::every(16ms, Tick{}), std::move(resize), std::move(keys_));
    }
    static bool subs_key(const Model& m) { return m.paused; }
};

static_assert(Program<Plasma>);

int main() { return run<Plasma>({.title = "plasma"}); }
```

Points to notice:

- **All state is in the Model.** The image is rebuilt from it in `view()`;
  nothing is kept in globals or statics.
- **Size comes from a message.** `on_resize` delivers the terminal size
  (also once at start-up); `update()` turns it into pixel dimensions.
  Until it arrives, `m.w == 0` and the view is empty.
- **Animation is a subscription.** `Sub::every(16ms, Tick{})` drives the
  frame rate. `subs_key()` tells the runtime what `subscribe()` depends on,
  so the timer is only rebuilt when `paused` flips.
- **Simulation state lives in the model, too.** For a stateful effect (the
  Doom fire's heat field, Life's grid, particles) keep the buffer in the
  Model, advance it in `update(Model&, Tick)`, and have `render()` map it
  through a palette into an Image. See `examples/doom_fire.cpp`.

## Where to look next

| Example | Shows |
|---------|-------|
| `doom_fire.cpp`  | heat field in the Model, palette LUT into an Image, embers drawn as whole cells |
| `mandelbrot.cpp` | `fill_rows` per-pixel shader, zoom/pan messages |
| `raymarch.cpp`, `space3d.cpp` | ray tracing with `fill_rows` and 5-bit quantisation |
| `fluid.cpp`, `life.cpp`, `particles.cpp` | simulations stepped in `update()` |
| `breakout.cpp`, `snake.cpp` | games: pixels plus text overlays |
| `matrix.cpp` | `Glyphs` digital rain |

## Low-level: Canvas and StylePool

`Canvas`, `Cell` and `StylePool` still exist as the renderer's internals:
the element tree paints into a `Canvas`, cells are packed into 64 bits and
diffed with SIMD, and styles are interned into a `StylePool`. Applications
no longer touch them: `pixels()` and `glyphs()` are the cell-level surface.
See [Wire efficiency](10-wire-efficiency.md) for how frames are diffed.
