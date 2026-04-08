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

This is how Claude Code works. The app stays inline for its entire lifetime.

```cpp
maya::run(
    {.alt_screen = false, .mouse = true},
    event_fn, render_fn
);
```

**Pipeline**: raw mode -> render_tree (auto_height) -> serialize_changed
(incremental, row-hash diffing) -> write

**Scrolling**: No application-level scrolling. The terminal's own scrollback
provides scrolling. The scroll widget is a pass-through in this mode.

**Key properties**:
- Content height is unconstrained (auto_height). Layout sizes to content.
- Row-hash comparison detects stable rows. Stable rows at the top are
  "committed" to scrollback and never overwritten.
- Output is capped to `term_height - 1` rows to avoid terminal auto-scroll.
- Resize erases only the live region (prev_height_ rows), then re-renders.
  Committed content in scrollback survives (possibly reflowed at old width).

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

## Mode comparison

| Aspect              | Inline         | Inline Run        | Alt Screen          |
|---------------------|----------------|-------------------|---------------------|
| Raw mode            | No             | Yes               | Yes                 |
| Alt screen buffer   | No             | No                | Yes                 |
| Layout height       | Auto (content) | Auto (content)    | Fixed (terminal)    |
| Diff method         | N/A            | Row-hash          | Cell-level          |
| Scroll widget       | No             | Pass-through      | Clip + scrollbar    |
| Terminal scrollback | Yes            | Yes (committed)   | None (alt buffer)   |
| Mouse               | No             | Optional          | Optional            |
| Resize handling     | N/A            | Erase live region | Recreate + repaint  |

---

## Scroll widget behavior per mode

- **Inline / Inline Run**: The scroll widget is a **pass-through**. It renders
  all content without clipping. No scrollbar. Height = content height. The
  terminal's native scrollback provides scrolling.

- **Alt Screen**: The scroll widget **clips** to the viewport height, renders a
  braille scrollbar, and handles scroll events (mouse wheel, PageUp/PageDown,
  Ctrl+Up/Down).

The scroll widget detects the mode via the layout allocation: in auto_height
mode the allocated height is small (h < 2, from default measure), so it returns
content directly. In alt-screen mode the allocated height comes from grow/fixed
constraints, enabling clip + scrollbar.
