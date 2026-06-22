# The Rendering Problem

You now know the three things a terminal gives you: a **grid of cells**, a
stream of **ANSI escape codes** to paint that grid, and a trickle of **input
bytes** coming back. With those pieces you could, in principle, draw any UI you
like. So let's try the obvious thing — and watch it fall apart.

This page is about *why* drawing a UI on a terminal is genuinely hard, and the
handful of techniques that turn a stuttering, flickering mess into something
that feels as smooth as a native app. Everything maya's renderer does exists to
solve the problems on this page. Once you've felt the problems, the architecture
in the next page will read like the only sane response.

---

## The naive approach

Here is the first program everyone writes. A clock, redrawn once a second:

```text
loop forever:
    clear the screen          # ESC[2J
    move cursor to home       # ESC[H
    print the whole UI        # every row, every cell
    sleep until next frame
```

In raw escape codes, one frame looks like this:

```text
ESC[2J            <- erase entire screen
ESC[H             <- cursor to row 1, col 1
"┌─ clock ──────────────┐\r\n"
"│  14:32:08            │\r\n"
"└──────────────────────┘\r\n"
```

It works. You run it, you see a clock, you feel clever. Then you actually *look*
at it, and you notice three separate things wrong — and the bigger your UI gets,
the worse all three become.

### Problem 1: Flicker

`ESC[2J` erases the screen *immediately*. Your redraw arrives a few milliseconds
later. In the gap between "screen is now blank" and "screen is painted again,"
the terminal is showing you **nothing** — a blank rectangle.

Do that 60 times a second and the display spends a meaningful slice of every
frame empty. Your eyes integrate those blank moments into a visible shimmer.
That's flicker, and it's the single most recognisable mark of an amateur TUI.

```text
frame N        gap            frame N+1
┌──────────┐   ┌──────────┐   ┌──────────┐
│ 14:32:08 │ → │          │ → │ 14:32:09 │
└──────────┘   └──────────┘   └──────────┘
   visible       BLANK          visible
                (flicker)
```

### Problem 2: Tearing

The terminal doesn't wait for your whole frame to arrive before it starts
drawing. It paints bytes as they stream in. If your redraw takes longer than a
single display refresh — or the OS schedules you out halfway through the
`write()` — the screen shows the **top half of the new frame and the bottom half
of the old one** at the same time.

That seam is *tearing*. On a clock it's subtle. On a scrolling list or an
animation it looks like the UI is being ripped in two.

### Problem 3: It's slow

This is the one that really bites, and it's the least obvious. Look at what the
naive loop sends every single frame: **the entire screen.** An 80x24 terminal is
1,920 cells. With styling — colours, bold, the escape codes that set them — a
full repaint is easily **8–20 KB of bytes on the wire, every frame.**

For a clock where *one digit changed*, you just sent twenty thousand bytes to
update three of them.

!!! warning "This is brutal over SSH"
    On a local terminal you might not notice — the bytes never leave the
    machine. Open the same app over SSH on hotel Wi-Fi, or a mosh session to a
    box three continents away, and the naive approach becomes unusable. Every
    keystroke triggers a full-screen repaint that has to crawl down a 200ms
    link. The UI feels like it's moving through syrup. TUIs live and die on
    slow links, and the naive renderer dies first.

---

## The real bottleneck: bytes on the wire

Here's the mental model that makes everything else click into place.

**A terminal is driven entirely by a byte stream.** You don't have a framebuffer
you can poke pixels into. You have one pipe — stdout — and the only thing you can
do is write bytes to it. Those bytes are *commands*: move the cursor, set a
colour, draw this glyph. The terminal reads them in order and obeys.

The cost of a frame is, to a very good approximation, **proportional to the
number of bytes you write.** Parsing them, applying styles, laying out glyphs,
and — crucially — *transmitting* them over whatever connection sits between your
program and the screen.

!!! tip "Write less. That's the whole game."
    Almost every rendering technique in this document is a different answer to a
    single question: *how do I send fewer bytes?* Diffing sends fewer bytes.
    Span batching sends fewer bytes. Minimising style transitions sends fewer
    bytes. Keep that lens and the rest of the page is just tactics.

The naive renderer fails because it has the worst possible answer: it sends
*every* byte, *every* frame, forever. We need a renderer that sends only what
actually changed.

---

## The key idea: double buffering + diffing

This is the most important rendering technique in all of TUI development. If you
take one thing from this page, take this.

Keep **two** cell grids in memory:

- the **front buffer** — what's currently on screen,
- the **back buffer** — the frame you're about to draw.

Each frame, you build the new UI into the back buffer *without touching the
terminal at all*. Then you **compare the two buffers cell by cell** and emit
escape codes only for the cells that actually differ. Finally you swap: the back
buffer becomes the new front buffer, ready for next time.

```text
        FRONT (on screen)            BACK (next frame)
        ┌───────────────────┐        ┌───────────────────┐
        │ 1 4 : 3 2 : 0 8   │        │ 1 4 : 3 2 : 0 9   │
        └───────────────────┘        └───────────────────┘
                  │                            │
                  └─────────── diff ───────────┘
                               │
                  only this cell changed:  8 → 9
                               │
                               ▼
              emit: move cursor to that column, write "9"
```

No clear. No full repaint. The screen is *never blanked*, so there's nothing to
flicker. And we sent almost nothing.

### Walk through the clock

Watch the bytes. The clock ticks from `14:32:08` to `14:32:09`. The diff finds
exactly one changed cell — the seconds' ones digit. So the renderer emits:

```text
ESC[2;22H     <- move cursor to that one cell (row 2, col 22)
9             <- write the new glyph
```

That's it. Roughly **9 bytes** instead of a full-screen repaint. The style
didn't change, so we don't even re-send the colour codes — the terminal's
current pen is already correct.

| Approach | Bytes per frame (80x24, one digit changed) | Over a 200ms SSH link |
|---|---|---|
| Naive: `ESC[2J` + full repaint | ~8,000–20,000 | sluggish, flickery |
| Double buffer + diff | ~3–10 | instant |

That is not a small win. That is the difference between "toy" and "tool." A
diffing renderer routinely sends **three orders of magnitude** fewer bytes than
a naive one on a typical UI update, because typical UI updates change a tiny
fraction of the screen.

!!! note "Why it's called double buffering"
    The term comes from graphics: you draw into an off-screen buffer, then
    present it all at once, so the viewer never sees a half-drawn frame. In a
    TUI the "presentation" step is the diff — but the principle is identical.
    You never let the terminal see work-in-progress.

---

## Run-length and span optimization

Diffing tells you *which* cells changed. But naively, you might emit a separate
"move cursor, write glyph" pair for each one. If a whole word changed, that's a
cursor move before every single character — and cursor-move escape codes
(`ESC[row;colH`) are *expensive*: 6–10 bytes each, far more than the glyph
they precede.

The fix is to **batch adjacent changed cells into spans.** If columns 10 through
18 all changed and they share the same style, you move the cursor *once* and
write all nine glyphs in a single run:

```text
naive per-cell:
  ESC[5;10H H   ESC[5;11H e   ESC[5;12H l   ESC[5;13H l   ESC[5;14H o
  (5 cursor moves, ~40 bytes)

span-batched:
  ESC[5;10H Hello
  (1 cursor move, ~13 bytes)
```

Style transitions cost bytes too. Every time the *pen* changes — a new colour, a
toggle of bold — you have to emit an SGR sequence (`ESC[1;38;5;201m` and
friends), which can be a dozen-plus bytes. So a good renderer also tries to
**minimise style changes**: it tracks the terminal's current pen and only emits
an SGR when the next cell genuinely needs a different one. Cells that share a
style get written under one SGR, no re-statement in between.

So the real diff output isn't "a list of changed cells." It's an optimised plan:

- the **fewest cursor moves** (batch runs, skip moves when the cursor is already
  in the right place after a write),
- the **fewest SGR changes** (coalesce same-styled runs, track the current pen),
- only the glyphs that genuinely differ.

!!! tip "Two layers of laziness"
    Diffing is "don't redraw cells that didn't change." Span batching is "and
    for the cells that *did*, don't waste bytes moving and re-styling between
    them." Together they squeeze the byte stream down to something close to the
    theoretical minimum.

---

## The alternate screen buffer

Full-screen apps — editors, dashboards, file managers — want the *whole*
terminal as a clean canvas, and they want your shell exactly as you left it when
they exit. The terminal gives you a dedicated mechanism for this: the **alternate
screen buffer.**

```bash
ESC[?1049h    # switch TO the alternate screen (blank, saves your shell)
# ... app runs here on a clean canvas ...
ESC[?1049l    # switch BACK (restores your scrollback, untouched)
```

When you enter the alt screen, the terminal stashes your current screen and
scrollback and hands you a fresh, empty one. When you leave, it throws away
everything you drew and restores the original. This is why `vim` and `htop` can
take over your whole window and then vanish without a trace — your command
history and scrollback are exactly where they were.

### Inline apps are a different, harder beast

Not every app wants to seize the whole screen. Some want to draw a block of UI
*below your prompt* and update it in place while the surrounding scrollback stays
put — think a build tool with a live progress region, or a coding assistant that
streams output and keeps a status panel pinned at the bottom.

This **inline mode** is much harder than alt-screen, and it's worth knowing why:

- There's no clean canvas. You're sharing the screen with the shell and with
  whatever scrolls past, so you must track exactly where your region starts.
- The region can **grow**. Add a line and everything below shifts; the terminal
  may scroll, which moves your anchor out from under you.
- Get the cursor maths wrong and you don't just draw a glitch — you **corrupt the
  user's scrollback**, leaving stray fragments of your UI permanently wedged into
  their command history.

!!! warning "The inline trade-off"
    Alt-screen gives you a clean, bounded canvas at the cost of taking over the
    terminal. Inline keeps you embedded in the user's session at the cost of a
    much more delicate renderer — one that has to reason about scroll position,
    region growth, and where the shell prompt lives. Most "this feels magical"
    inline tools are doing a *lot* of careful bookkeeping you never see.

---

## Synchronized output (DEC 2026)

Diffing kills flicker. But tearing can still sneak in: even a small diff is
multiple writes, and the terminal might refresh the display *between* them,
catching a frame half-applied.

Modern terminals support a fix: **synchronized output**, the private mode
`DEC 2026`. You wrap your whole frame in a begin/end pair:

```text
ESC[?2026h        <- BEGIN sync: terminal stops repainting the display
  ... all of this frame's diff output ...
ESC[?2026l        <- END sync: terminal paints everything atomically
```

Between begin and end, the terminal buffers your updates and **does not touch
the visible display.** When you signal end, it paints the entire frame in one
atomic flip. The user never sees an intermediate state — no seam, no tear, even
if your frame was a hundred separate writes.

!!! note "Graceful when unsupported"
    Terminals that don't understand `ESC[?2026h` simply ignore it (it's a
    private-mode sequence they don't recognise). So you can always emit it: you
    get atomic frames where it's supported, and identical-but-slightly-tearable
    behaviour where it isn't. There's no downside to wrapping every frame in it.

Synchronized output is the belt to diffing's braces. Diffing means you *write
little*; sync means whatever little you write lands *all at once*.

---

## Cursor management and leftover state

The terminal is a **stateful machine**, and that statefulness is a trap.

There's one cursor and one "current pen" (the active colours and attributes).
Every escape code you send mutates that shared state, and it *persists* — the
terminal doesn't reset between your frames. Which leads to two rules.

**Hide the cursor while you redraw.** As your diff jumps the cursor around the
screen writing glyphs, a visible cursor flickers along with it, skittering across
the UI. So full-screen renderers hide it during the redraw and only show it (or
position it deliberately) when the frame is settled:

```text
ESC[?25l      <- hide cursor
  ... diff output, cursor leaping all over ...
ESC[?25h      <- show cursor (or leave hidden for a full-screen app)
```

**Never leave dangling state.** This is where stateful rendering punishes you.
If you emit an SGR to turn on bold and a colour, write your text, and *forget to
reset the pen* — every cell you write afterward, this frame and every frame
after, inherits that bold red until something happens to overwrite it. A stray
cursor move, a half-applied SGR, an un-popped style: each one silently corrupts
*future* frames, and the bug shows up nowhere near its cause.

!!! warning "State leaks are the worst TUI bugs"
    A pixel bug in a GUI is local — it's wrong *there*. A state leak in a TUI is
    non-local: the mistake is in frame 12 but the garbage appears in frame 40,
    in a completely different widget, because the pen was never reset. This is
    why serious renderers track the terminal's state meticulously and always
    leave it in a known-clean condition at the end of every frame.

---

## The frame budget

Smooth means **60 frames per second**, which gives you a hard ceiling:

```text
1 second / 60 frames ≈ 16.6 milliseconds per frame
```

In those 16ms you have to: handle input, update your application state, lay out
the new UI into the back buffer, diff it against the front buffer, and write the
result. Blow the budget and frames drop — the UI stutters.

Diffing is what keeps you under budget on the *output* side: a small change means
a small write, so the expensive part (bytes on the wire) stays tiny regardless of
how big the screen is.

But there's a subtler cost: **the diff itself.** Comparing two 80x24 grids is
1,920 cell comparisons; a 4K terminal in a tiny font can be tens of thousands.
Do that 60 times a second and a naive cell-by-cell loop can start eating into the
very budget it's supposed to protect.

The answer is to make the comparison itself cheap. If each cell is packed into a
machine word, you can compare *many cells at once* with **SIMD** instructions —
the CPU's "do this to a whole vector in one go" hardware — and skip unchanged
rows wholesale. Precomputing layout and style data means the diff is comparing
ready-made values, not recomputing them.

!!! tip "maya packs cells for exactly this"
    This is a place where maya's design earns its keep. Each `Cell` is packed
    into 64 bits precisely so the frame diff can be a **SIMD** comparison — many
    cells checked per instruction — keeping the diff itself comfortably inside
    the 16ms budget even on large terminals. (The mechanics live in the Canvas
    and rendering pages; here it's enough to know *why* the cell is shaped that
    way.)

---

## The edge cases that make this genuinely hard

If diffing, alt-screen, and sync were the whole story, every TUI framework would
be a weekend project. They aren't, because the terminal is a forty-year-old
abstraction held together by convention, and the edges are sharp. A few that a
real renderer has to survive:

- **Resize mid-frame.** The user drags the window while you're halfway through
  writing a frame computed for the *old* size. Your cursor moves now point at the
  wrong cells; your front buffer describes a screen that no longer exists. The
  renderer has to detect the resize, throw away its assumptions, and reconcile
  the buffers to the new geometry without painting garbage.

- **Wide characters straddling a diff boundary.** A CJK glyph or a wide emoji
  occupies **two** cells. If your diff decides to redraw only the *right* half of
  one, you've split an indivisible glyph — best case a mojibake smear, worst case
  the terminal's column count desyncs from yours for the rest of the line. The
  diff has to treat wide cells as atomic units, never half-touching them.

- **Scrollback corruption in inline mode.** As covered above: miscount the
  cursor while drawing below the prompt and you wedge fragments of your UI
  permanently into the user's history. There's no undo — it's their scrollback,
  not your canvas.

- **Terminals that mis-track cursor moves.** Some terminals — and many SSH and
  multiplexer combinations — don't move the cursor exactly where you asked,
  especially around line wraps, the last column, and wide glyphs. If you *assume*
  the cursor is where your move said and it isn't, every subsequent write lands
  one cell off, and the whole frame slides into nonsense. This is the
  **compatibility tax**: a serious renderer can't just emit the theoretically
  minimal sequence, it has to emit the sequence that's *robust* across the messy
  reality of terminals people actually use.

!!! note "This is why renderers are careful, not clever"
    None of these are exotic. Resizing a window, pasting an emoji, running over
    SSH — these are *Tuesday*. A renderer that only handles the happy path
    produces a UI that glitches the moment a real human touches it. The careful
    handling of these edges is most of what separates a robust framework from a
    demo.

---

## Tying it together

Step back and the shape of the solution is clear. To draw a UI on a terminal
*smoothly*, you must:

1. **Never blank the screen** — build the next frame in memory, off-screen.
2. **Diff** it against the previous frame and write only what changed.
3. **Batch** the changes into spans to minimise cursor moves and style switches.
4. Wrap each frame in **synchronized output** so it lands atomically.
5. Keep terminal **state clean** — hidden cursor during redraw, no dangling pen.
6. Make the **diff itself fast** so you stay inside the 16ms budget.
7. Survive the **edge cases** — resize, wide chars, cursor mis-tracking, inline
   scrollback — because real terminals are messy.

This is *exactly* the problem maya's renderer is built to solve. At a high level:
your declarative **Element tree** is laid out into a grid of cells — a
**Canvas** — which is then diffed against the previous frame's Canvas using a
SIMD cell comparison, so that only the minimal set of changed cells is written,
batched into spans, and wrapped in synchronized output. You describe *what the UI
should look like*; the renderer figures out the smallest, safest stream of bytes
that gets the terminal there.

Everything on this page — flicker, tearing, bytes on the wire, the frame budget,
the compatibility tax — is a force that shaped that architecture. Now that you
can feel those forces, the next page shows how the pieces fit.

---

## What's next

You understand the problem; next is the machine that solves it.

→ **[How maya Works](how-maya-works.md)** — how the Element tree, layout, Canvas,
diffing renderer, and event loop connect into the architecture you've been
reading hints about. Everything on this page becomes a concrete component there.
