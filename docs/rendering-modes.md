# Maya Rendering Modes

Three rendering modes, each with a clear purpose and distinct pipeline.

## 1. Inline Mode (`maya::print`)

One-shot rendering. Print an element tree to stdout and return. No event loop,
no interactivity, no raw mode. The terminal's cursor advances past the output.

```cpp
maya::print(markdown("# Hello"));
```

**Pipeline**: render_tree (auto_height) -> serialize -> fwrite -> newline -> return

**Scrolling**: Terminal's native scrollback. Content is just text on stdout.

---

## 2. Inline Run Mode (`alt_screen = false`)

Interactive inline rendering. The app runs in raw mode but does NOT enter the
alt screen buffer. Content renders inline in the terminal, preserving scrollback
above. Each frame erases and redraws the live region.

This is the **brief entry mode** for chat-style apps. The app renders its first
frame or two inline (the user sees their shell history above), then immediately
promotes to alt screen.

```cpp
maya::run(
    {.alt_screen = false, .mouse = true},
    event_fn, render_fn
);
```

**Pipeline**: raw mode -> render_tree (auto_height) -> serialize_changed
(incremental, row-hash diffing) -> write

**Scrolling**: No application-level scrolling. Content is sized to fit. The
terminal's own scrollback holds committed rows.

**Key properties**:
- Content height is unconstrained (auto_height). Layout sizes to content.
- Row-hash comparison detects stable rows. Stable rows at the top are
  "committed" to scrollback and never overwritten.
- Output is capped to `term_height - 1` rows to avoid terminal auto-scroll.
- Width changes trigger full erase + redraw (terminal reflow makes incremental
  updates impossible).

---

## 3. Alt Screen Mode (`alt_screen = true`)

Full-screen mode. The terminal switches to the alternate screen buffer. The app
owns the entire viewport. Double-buffered cell-level diffing.

```cpp
maya::run(
    {.alt_screen = true, .mouse = true},
    event_fn, render_fn
);
```

**Pipeline**: alt screen -> render_tree (fixed height) -> diff (cell-level,
front vs back canvas) -> write -> swap buffers

**Scrolling**: Application-level. The `scroll()` widget clips content to a
viewport and renders a scrollbar. The terminal has no scrollback in alt screen.

**Key properties**:
- Layout is constrained to terminal dimensions (width x height).
- Cell-level diff: only changed cells emit ANSI sequences.
- Resize: recreate canvases, full clear + serialize.

---

## 4. Promotion: Inline Run -> Alt Screen

This is how Claude Code works. The app starts in inline run mode and
**promotes to alt screen on the first resize** (SIGWINCH). Until then, the app
renders inline -- the user sees their shell history above and the app's UI
below. On resize, the app enters alt screen for the rest of the session.

### Promotion trigger

Promotion happens when:
- The terminal is resized (SIGWINCH) while the app is still in inline mode.

### Before promotion

The app renders inline for as long as the user doesn't resize. Content is
auto-height, scroll widgets pass through, the terminal's own scrollback
provides history. This is lightweight and familiar.

### Promotion sequence

```
1. SIGWINCH fires, handle_resize() detects size change
2. Enter alt screen (CSI ?1049h)
   - Terminal saves the main screen (scrollback + inline output)
   - Switches to a clean alternate buffer
3. Switch rendering pipeline: inline -> alt screen
   - Replace canvas (500-row virtual -> terminal-sized)
   - Create front buffer for diffing
   - Enable scroll widgets (fixed-height layout)
4. Full clear + render first alt-screen frame
```

### On exit

When the app exits from promoted mode:
```
1. Leave alt screen (CSI ?1049l)
   - Terminal restores the main screen with all scrollback intact,
     including the inline output from before promotion
2. Restore termios (raw -> cooked)
3. The shell prompt appears right below where the inline content was
```

The user sees: their shell history, the inline output from before promotion,
then their shell prompt. Everything the app rendered in alt screen is gone (as
expected -- it was on the alternate buffer).

### Resize in alt screen

Once promoted, resize is handled entirely within alt screen mode:
- Recreate canvases at new dimensions
- Full clear + serialize (no diff -- terminal content is stale)
- No demotion back to inline

---

## Mode comparison

| Aspect              | Inline         | Inline Run        | Alt Screen (+ Promoted) |
|---------------------|----------------|-------------------|-------------------------|
| Raw mode            | No             | Yes               | Yes                     |
| Alt screen buffer   | No             | No                | Yes                     |
| Layout height       | Auto (content) | Auto (content)    | Fixed (terminal)        |
| Diff method         | N/A            | Row-hash           | Cell-level              |
| Scroll widget       | No             | No                | Yes                     |
| Terminal scrollback | Yes            | Yes (committed)   | Preserved but hidden    |
| Mouse               | No             | Optional          | Optional                |
| Resize handling     | N/A            | Erase + redraw    | Recreate + full repaint |

---

## Scroll widget behavior per mode

- **Inline / Inline Run**: The scroll widget is a **pass-through**. It renders
  all content without clipping. No scrollbar. Height = content height. The
  terminal's native scrollback provides scrolling.

- **Alt Screen**: The scroll widget **clips** to the viewport height, renders a
  braille scrollbar, and handles scroll events (mouse wheel, PageUp/PageDown,
  Ctrl+Up/Down).

The scroll widget checks whether the app is in alt-screen mode (fixed-height
layout) to decide its behavior. This is implicit from the layout constraints:
if the parent gives it a fixed height, it clips; if auto_height, it passes
through.

---

## Implementation plan

1. **Implement `promote_to_alt_screen()` in App**: Transition `raw_terminal_`
   -> `alt_terminal_` (consuming move through the type-state chain). Switch
   pipeline state: create terminal-sized canvases, front buffer, set
   `needs_clear_`, flip `is_inline()` to false.

2. **Promote on resize**: In `handle_resize()`, if still inline, call
   `promote_to_alt_screen()`. The app stays inline until the user resizes.

3. **Fix scroll widget**: In auto_height mode (inline), render all content
   without clipping or scrollbar. Remove the measure function hack. The scroll
   widget becomes a simple vstack wrapper when unconstrained.

4. **Update chat example**: Demonstrates inline -> promoted pattern. Uses
   `{.alt_screen = false}` to start inline, scroll widget works in both modes.
