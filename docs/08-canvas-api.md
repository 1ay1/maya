# Canvas API

The Canvas API is maya's low-level rendering surface. While most applications
use the DSL and element tree (`run()`, `print()`), the Canvas API gives you
direct cell-level control for games, animations, particle systems, and complex
visualizations.

## Canvas Basics

A `Canvas` is a 2D grid of `Cell` values. Each cell holds:

```cpp
struct Cell {
    char32_t character;      // Unicode codepoint
    uint16_t style_id;       // Index into StylePool
    uint16_t hyperlink_id;   // (reserved)
    uint8_t  width;          // Display width (1 or 2 for wide chars)
};
```

Cells are packed into 64 bits for efficient SIMD comparison during frame
diffing.

### Writing Cells

```cpp
// Set a single cell
canvas.set(x, y, U'*', style_id);
canvas.set(x, y, U'█', style_id);
canvas.set(x, y, U'╭', border_style_id);

// Write text (ASCII string → sequence of cells)
canvas.write_text(x, y, "Hello", style_id);
canvas.write_text(x, y, std::string_view{"Status"}, style_id);

// Fill a region
canvas.fill(Rect{pos, size}, U' ', bg_style_id);

// Clear the entire canvas
canvas.clear();
```

### Reading Cells

```cpp
Cell c = canvas.get(x, y);
if (c.character == U'*') { ... }
if (c.style_id == my_highlight) { ... }
```

### Canvas Dimensions

```cpp
int w = canvas.width();
int h = canvas.height();
```

## StylePool — Style Interning

Every unique `Style` gets a compact `uint16_t` ID via the `StylePool`. This
keeps cells at 8 bytes and enables fast SIMD comparison.

### Interning Styles

```cpp
uint16_t id = pool.intern(Style{}.with_bold().with_fg(Color::green()));
```

If the style was already interned, the existing ID is returned. The pool uses
open-addressing hashing for O(1) average lookup.

### Looking Up Styles

```cpp
const Style& s = pool.get(id);
```

### Pool Lifecycle

In `canvas_run()`, the pool is **cleared and rebuilt on every resize**. Your
`on_resize` callback must re-intern all styles:

```cpp
[&](StylePool& pool, int W, int H) {
    // Pool was just cleared — re-intern everything
    s_bold  = pool.intern(Style{}.with_bold().with_fg(Color::white()));
    s_dim   = pool.intern(Style{}.with_dim().with_fg(Color::gray()));
    s_bar   = pool.intern(Style{}.with_fg(Color::rgb(80, 200, 255)));

    // Pre-intern gradient palettes
    for (int i = 0; i < kGradientSteps; ++i) {
        float t = float(i) / float(kGradientSteps - 1);
        auto c = lerp_color(color_a, color_b, t);
        gradient[i] = pool.intern(Style{}.with_fg(c));
    }
}
```

### Pre-Interning for Performance

Intern all styles upfront in `on_resize`. **Never intern inside `on_paint`** —
the pool would grow unboundedly and the hash lookups add up.

For complex themes with many color combinations, pre-intern all needed combos:

```cpp
// Pre-intern NxN heatmap palette (fg=top pixel, bg=bottom pixel)
for (int fi = 0; fi < kHeat; ++fi) {
    for (int bi = 0; bi < kHeat; ++bi) {
        heat_styles[fi][bi] = pool.intern(
            Style{}.with_fg(heat_color(fi))
                   .with_bg(heat_color(bi))
        );
    }
}
```

## Clip Regions

The canvas supports a clip rectangle stack for implementing `overflow: hidden`:

```cpp
canvas.push_clip(Rect{...});
// All set/write_text calls are clipped to this rectangle
canvas.set(x, y, ch, sid);  // Only written if (x,y) is inside clip rect
canvas.pop_clip();
```

You typically don't use this directly — the renderer handles it for
`BoxElement` nodes with `Overflow::Hidden`.

## Unicode and Wide Characters

### Standard Characters

```cpp
canvas.set(x, y, U'A', sid);     // 1 cell wide
canvas.set(x, y, U'α', sid);     // 1 cell wide
canvas.set(x, y, U'→', sid);     // 1 cell wide
```

### Box-Drawing Characters

```cpp
canvas.set(x, y, U'┌', sid);     // Top-left corner
canvas.set(x, y, U'─', sid);     // Horizontal line
canvas.set(x, y, U'│', sid);     // Vertical line
canvas.set(x, y, U'╭', sid);     // Rounded corner
```

### Block Elements (for bars and gauges)

```cpp
// Full blocks
canvas.set(x, y, U'█', sid);     // Full block
canvas.set(x, y, U'▓', sid);     // Dark shade
canvas.set(x, y, U'▒', sid);     // Medium shade
canvas.set(x, y, U'░', sid);     // Light shade

// Partial blocks (vertical)
canvas.set(x, y, U'▁', sid);     // Lower 1/8
canvas.set(x, y, U'▂', sid);     // Lower 2/8
// ... through U'▇' (7/8) and U'█' (full)

// Half blocks (for 2x vertical resolution)
canvas.set(x, y, U'▀', sid);     // Upper half (fg=top, bg=bottom)
canvas.set(x, y, U'▄', sid);     // Lower half
```

### Braille Characters (for sub-cell resolution)

Braille characters (U+2800–U+28FF) encode 2x4 pixel grids, giving 2x horizontal
and 4x vertical sub-cell resolution:

```cpp
// Each braille char has 8 dots (2 cols × 4 rows)
// Dot positions map to bits:
//   col 0 (left)   col 1 (right)
//   row 0: bit 0   bit 4
//   row 1: bit 1   bit 5
//   row 2: bit 2   bit 6
//   row 3: bit 3   bit 7

uint8_t dots = 0;
dots |= 0x01;  // Top-left dot
dots |= 0x80;  // Bottom-right dot
canvas.set(x, y, static_cast<char32_t>(0x2800 + dots), sid);
```

This technique is used in `viz.cpp` for area charts with sub-cell precision.

## Half-Block Heatmaps

Use `▀` (upper half block) with fg=top color and bg=bottom color to get 2x
vertical resolution in color heatmaps:

```cpp
// Each terminal row represents TWO pixel rows:
//   fg color = top pixel value
//   bg color = bottom pixel value
for (int cy = 0; cy < ch; ++cy) {
    for (int cx = 0; cx < cw; ++cx) {
        float top_val = compute(cx, cy * 2);
        float bot_val = compute(cx, cy * 2 + 1);
        int fi = int(top_val * (kPalette - 1));
        int bi = int(bot_val * (kPalette - 1));
        canvas.set(cx, cy, U'▀', heatmap_styles[fi][bi]);
    }
}
```

## Canvas Rendering Patterns

### Pattern: Status Bar

```cpp
void paint_bar(Canvas& canvas, int W, int H, uint16_t bg_id, uint16_t text_id) {
    int y = H - 1;
    // Fill background
    for (int x = 0; x < W; ++x)
        canvas.set(x, y, U' ', bg_id);
    // Write text
    canvas.write_text(1, y, "status text", text_id);
}
```

### Pattern: Sparkline

```cpp
void paint_sparkline(Canvas& canvas, int x, int y,
                     const std::deque<float>& data, int w, uint16_t sid) {
    static constexpr char32_t blocks[] = {
        U'▁', U'▂', U'▃', U'▄', U'▅', U'▆', U'▇', U'█'
    };
    int start = std::max(0, int(data.size()) - w);
    for (int i = start, dx = 0; i < int(data.size()) && dx < w; ++i, ++dx) {
        int level = std::clamp(int(data[i] * 7.99f), 0, 7);
        canvas.set(x + dx, y, blocks[level], sid);
    }
}
```

### Pattern: Particle System

```cpp
void paint_particles(Canvas& canvas, int W, int H) {
    for (const auto& p : particles) {
        int cx = int(p.x);
        int cy = int(p.y);
        if (cx < 0 || cx >= W || cy < 0 || cy >= H) continue;

        int grad_idx = int((1.0f - p.life) * (kGradSteps - 1));
        canvas.set(cx, cy, p.glyph, gradient_styles[p.palette][grad_idx]);
    }
}
```

### Pattern: Bordered Panel

```cpp
void paint_panel(Canvas& canvas, int x0, int y0, int x1, int y1,
                 uint16_t border_sid, uint16_t title_sid) {
    // Corners
    canvas.set(x0, y0, U'╭', border_sid);
    canvas.set(x1, y0, U'╮', border_sid);
    canvas.set(x0, y1, U'╰', border_sid);
    canvas.set(x1, y1, U'╯', border_sid);

    // Horizontal edges
    for (int x = x0 + 1; x < x1; ++x) {
        canvas.set(x, y0, U'─', border_sid);
        canvas.set(x, y1, U'─', border_sid);
    }

    // Vertical edges
    for (int y = y0 + 1; y < y1; ++y) {
        canvas.set(x0, y, U'│', border_sid);
        canvas.set(x1, y, U'│', border_sid);
    }

    // Title
    canvas.write_text(x0 + 2, y0, "Title", title_sid);
}
```

## SIMD-Accelerated Frame Diffing

maya's frame diff engine compares the current and previous canvas buffers using
SIMD instructions. Because cells are 64-bit packed values, the diff can compare
8 cells at once with AVX-512, 4 with AVX2, or 2 with SSE2/NEON.

Functions available in `maya::core::simd`:

| Function | Description |
|----------|-------------|
| `find_first_diff(a, b, count)` | Find index of first differing cell |
| `skip_equal(a, b, start, end)` | Find next diff starting from index |
| `bulk_eq(a, b, count)` | Check if two buffers are identical |
| `streaming_fill(dst, count, value)` | Non-temporal fill (bypasses cache) |

The framework uses these internally — you don't call them directly. The result
is that only changed cells are re-serialized and written to the terminal,
making even full-screen 60fps animations efficient.

## AlignedBuffer

Canvas storage uses `AlignedBuffer` — a 64-byte cache-line aligned buffer
optimized for SIMD access:

```cpp
struct AlignedBuffer {
    AlignedBuffer(size_t count, uint64_t fill_value);
    void resize(size_t count, uint64_t fill_value);
    uint64_t* data();
    size_t size();
};
```

You don't interact with this directly — it's the canvas's internal storage.
