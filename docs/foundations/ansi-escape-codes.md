# ANSI Escape Codes

A terminal is, at heart, a device that reads a stream of bytes and paints
glyphs onto a grid. Most of those bytes are *literal* — send the byte `A` and
you get an `A` on screen. But hidden inside that same byte stream is a control
language. A handful of magic byte sequences don't print anything at all;
instead they tell the terminal to **move the cursor**, **change the text
color**, **clear the screen**, **hide the cursor**, or **switch to a whole
different screen buffer**.

That language is **ANSI escape codes**. Every colored prompt, every progress
bar that updates in place, every full-screen text editor, every TUI you've ever
used is built on it. This page is the centerpiece of the Foundations series: if
you understand escape codes, you understand how terminal UIs actually work all
the way down to the wire.

!!! note "You already use these every day"
    When `git` prints a red diff line, when `ls` colors directories blue, when
    your shell prompt shows a green checkmark — that's escape codes. Nothing
    exotic is happening. The same `printf` you use for `"Hello\n"` is sending
    them.

---

## The core idea: one byte changes everything

Your terminal reads bytes one at a time. As long as the bytes are ordinary
printable characters, it prints them and advances the cursor. The trick is that
**one specific byte breaks the spell**: the **ESC** byte, value `0x1B`
(decimal 27).

When the terminal sees ESC, it stops printing and starts *listening* — it now
expects the bytes that follow to describe a command. The ESC byte plus the
bytes after it form an **escape sequence**, and the terminal interprets it
rather than displaying it.

You will see the ESC byte written four different ways depending on the language
and shell. **They are all the exact same byte:**

| Notation | Where you'll see it | Meaning |
|----------|---------------------|---------|
| `\x1b`   | C, C++, Python, most languages | hex escape for `0x1B` |
| `\033`   | C, older code, `printf` | octal escape (27 in octal is 33) |
| `\e`     | bash, `echo -e`, zsh, some C compilers | shell shorthand for ESC |
| `^[`     | what you see if you type it raw in a terminal | the "caret" rendering |

Throughout this page we'll write **`\x1b`** in examples because it's the most
portable and unambiguous, but know that `\033` and `\e` mean the same thing.

```bash
# All three of these print the word "hi" in bold, then reset.
# They are byte-for-byte identical.
printf '\x1b[1mhi\x1b[0m\n'
printf '\033[1mhi\033[0m\n'
echo -e '\e[1mhi\e[0m'
```

!!! tip "Seeing the raw bytes"
    Want to prove ESC is really there? Pipe output through a byte viewer:
    ```bash
    printf '\x1b[1mhi\x1b[0m\n' | hexdump -C
    ```
    You'll see `1b 5b 31 6d` — that's ESC (`1b`), `[` (`5b`), `1` (`31`),
    `m` (`6d`) — followed by `68 69` (`hi`) and the reset sequence. The ESC
    byte is invisible on screen but absolutely present in the stream.

---

## Anatomy of a CSI sequence

The most common kind of escape sequence is the **CSI** sequence — *Control
Sequence Introducer*. Almost everything you'll write (cursor moves, colors,
clears) is CSI. Its shape is rigid and worth memorizing:

```text
ESC  [   params ; params ...   final-byte
\x1b [   1 ; 31                 m
└─┬┘ │   └────────┬────────┘    └┬┘
 ESC CSI      parameters       final
      introducer               byte
```

Breaking it down piece by piece:

1. **`ESC` (`\x1b`)** — the byte that says "a command follows."
2. **`[` (the Control Sequence Introducer)** — the open bracket immediately
   after ESC marks this specifically as a CSI sequence. `ESC` + `[` is so
   common it has its own name: **CSI**.
3. **Parameters** — zero or more numbers, separated by semicolons (`;`). What
   they mean depends entirely on the final byte. `1;31` might mean "bold,
   then red"; `10;5` might mean "row 10, column 5." Numbers only — no spaces.
4. **The final byte** — a single letter (or a few punctuation chars) that says
   *what kind of command this is*. `m` = set graphics (color/style),
   `H` = move cursor, `A` = cursor up, `J` = erase, `K` = erase line, and so on.

The final byte is the verb; the parameters are the arguments.

```bash
# ESC [ 1 ; 31 m  =>  "bold + red foreground"
printf '\x1b[1;31mDANGER\x1b[0m\n'
#       │ │└┬┘│
#       │ │ │ └ final byte 'm' = SGR (set style)
#       │ │ └── params: 1 (bold), 31 (red fg)
#       │ └──── CSI introducer '['
#       └────── ESC
```

!!! warning "Omitted parameters default to a sensible value"
    Most parameters default to `1` (or `0` for SGR) when left out. `\x1b[A`
    moves the cursor up **one** line — same as `\x1b[1A`. `\x1b[;H` moves to
    the home position (row 1, col 1) because both omitted params default.
    This is why you'll see `\x1b[2J` but plain `\x1b[J` is also valid.

---

## Cursor control

The cursor is the invisible pen position where the next character will land.
Escape codes let you move it anywhere on the grid — this is how a program
"draws" by jumping around and overwriting cells instead of scrolling.

!!! note "Rows and columns are 1-based"
    The top-left cell is **row 1, column 1** — not `(0,0)`. This trips up
    every programmer at least once.

### Absolute positioning

```text
ESC [ <row> ; <col> H      move cursor to (row, col)
```

```bash
# Clear the screen, then write "X" at row 5, column 20.
printf '\x1b[2J\x1b[5;20HX\n'
```

`H` (cursor position) is the workhorse. There's an identical-behaving `f`
(`ESC[<r>;<c>f`) you may occasionally see; prefer `H`.

### Relative movement

These move the cursor *from where it is now* by `n` cells. `n` defaults to 1.

| Sequence | Direction |
|----------|-----------|
| `ESC[<n>A` | up `n` rows |
| `ESC[<n>B` | down `n` rows |
| `ESC[<n>C` | right `n` columns |
| `ESC[<n>D` | left `n` columns |

```bash
# Print "A", move up 1 + right 3, print "B".
printf 'A\x1b[1A\x1b[3CB\n'
```

A handy mnemonic: **A**bove, **B**elow, and C/D are the next letters going
right/left.

### Save and restore the cursor

You can stash the current position and jump back to it later — invaluable when
you want to scribble something elsewhere and return.

```text
ESC 7        save cursor position (and attributes)   — note: no '['
ESC 8        restore saved cursor position
ESC [ s      save cursor position (CSI variant)
ESC [ u      restore cursor position (CSI variant)
```

```bash
# Save, move away and print, then jump right back.
printf 'start\x1b7  ... moved away ...\x1b8 BACK\n'
```

### Hide and show the cursor

Full-screen apps almost always hide the blinking cursor so it doesn't flicker
around as the screen redraws. These use the `?` *private mode* form:

```text
ESC [ ? 25 l      hide cursor   (l = low / off)
ESC [ ? 25 h      show cursor   (h = high / on)
```

```bash
# Hide the cursor for two seconds, then show it again.
printf '\x1b[?25l'; sleep 2; printf '\x1b[?25h'
```

!!! warning "Always restore what you change"
    If your program hides the cursor and then crashes before showing it again,
    the user is left staring at a terminal with **no cursor**. The same goes
    for the alternate screen and any mode you toggle. We'll come back to why
    this is a recurring source of pain.

### `\r` and `\n` — the humble pair

You don't need full CSI for the two most common cursor moves:

- **`\r`** (carriage return, `0x0D`) sends the cursor to **column 1 of the
  current line** without moving down. Overwrite-in-place tricks rely on it.
- **`\n`** (line feed, `0x0A`) moves **down one line** (and, in a terminal,
  also to column 1).

```bash
# A crude in-place progress counter: \r rewinds to col 1 each time.
for i in 1 2 3 4 5; do printf '\rProgress: %d/5' "$i"; sleep 0.3; done; echo
```

---

## Erasing

Moving the cursor doesn't remove old characters — they sit there until
overwritten. To clean up, you erase explicitly.

| Sequence | Effect |
|----------|--------|
| `ESC[K` or `ESC[0K` | erase from cursor to **end of line** |
| `ESC[1K` | erase from **start of line** to cursor |
| `ESC[2K` | erase the **entire current line** |
| `ESC[J` or `ESC[0J` | erase from cursor to **end of screen** |
| `ESC[1J` | erase from start of screen to cursor |
| `ESC[2J` | erase the **entire screen** |
| `ESC[3J` | erase the **scrollback buffer** (history above the screen) |

```bash
# Clear the whole screen and the scrollback, then move home.
printf '\x1b[3J\x1b[2J\x1b[H'
```

!!! tip "Clear-line beats clear-screen for updates"
    Repainting the entire screen with `\x1b[2J` on every frame causes visible
    flicker and wastes bytes. Smart programs erase only what changed —
    `\x1b[K` to wipe the rest of a line they're rewriting. (This is exactly
    the kind of minimal-diff bookkeeping a framework does for you.)

---

## SGR — Select Graphic Rendition (the styling workhorse)

`SGR` is the final byte **`m`**, and it controls *how* text looks: bold,
italic, underline, foreground color, background color. You pass one or more
codes separated by `;`, and they all apply at once.

```text
ESC [ <code> ; <code> ; ... m
```

The styling stays in effect until you change it — so you must **reset** when
you're done, or every line after will inherit your bold-red.

### Attributes

| Code | Effect | Turn off with |
|------|--------|----------------|
| `0` | **reset everything** | — |
| `1` | bold / bright | `22` |
| `2` | dim / faint | `22` |
| `3` | italic | `23` |
| `4` | underline | `24` |
| `7` | reverse video (swap fg/bg) | `27` |
| `9` | strikethrough | `29` |

```bash
printf '\x1b[1mbold\x1b[0m  \x1b[3mitalic\x1b[0m  \x1b[4munderline\x1b[0m\n'
printf '\x1b[9mstrikethrough\x1b[0m  \x1b[2mdim\x1b[0m\n'
```

!!! note "`0` is the most important code you'll write"
    `ESC[0m` resets *all* attributes back to the terminal default. Forgetting
    it is the #1 cause of "why is my whole prompt suddenly red?" When in doubt,
    reset.

### Color tier 1: the 16 basic colors

The original palette. Foreground codes are 30–37, background 40–47. The
**bright** variants are 90–97 (fg) and 100–107 (bg).

| Color | fg | bg | bright fg | bright bg |
|-------|----|----|-----------|-----------|
| black | 30 | 40 | 90 | 100 |
| red | 31 | 41 | 91 | 101 |
| green | 32 | 42 | 92 | 102 |
| yellow | 33 | 43 | 93 | 103 |
| blue | 34 | 44 | 94 | 104 |
| magenta | 35 | 45 | 95 | 105 |
| cyan | 36 | 46 | 96 | 106 |
| white | 37 | 47 | 97 | 107 |

```bash
# Yellow text on a blue background, then bright green.
printf '\x1b[33;44m sunny \x1b[0m \x1b[92mbright green\x1b[0m\n'
```

These 16 colors are **theme-dependent**: the exact RGB of "red" is whatever the
user's terminal color scheme says it is. That's a feature — your UI matches the
user's chosen palette — but it means you can't rely on a precise shade.

### Color tier 2: the 256-color palette

For more control, terminals expose a fixed palette of 256 colors. The form adds
two extra parameters: `5` (meaning "palette index follows") and the index
`0–255`.

```text
ESC [ 38 ; 5 ; <n> m      foreground = palette color n
ESC [ 48 ; 5 ; <n> m      background = palette color n
```

The 256 slots are organized as: 0–15 the basic colors, 16–231 a 6×6×6 RGB
color cube, and 232–255 a 24-step grayscale ramp.

```bash
# Print a strip of the 6x6x6 color cube (indices 16..51).
for n in $(seq 16 51); do printf '\x1b[48;5;%dm  ' "$n"; done; printf '\x1b[0m\n'
```

### Color tier 3: 24-bit truecolor

Modern terminals support full **24-bit RGB** — 16.7 million colors, any exact
shade you want. The form uses `2` (meaning "RGB follows") and three values
`r;g;b`, each `0–255`.

```text
ESC [ 38 ; 2 ; <r> ; <g> ; <b> m      foreground = exact RGB
ESC [ 48 ; 2 ; <r> ; <g> ; <b> m      background = exact RGB
```

```bash
# Foreground rgb(255,128,0) — a precise orange.
printf '\x1b[38;2;255;128;0mexact orange\x1b[0m\n'
```

!!! warning "Mind the `5` vs `2` and the semicolons"
    `38;5;n` is **palette** (one index). `38;2;r;g;b` is **truecolor**
    (three channels). Mixing them up — e.g. writing `38;2;200` with a missing
    channel — produces wrong colors or swallows the next character. This
    finicky, easy-to-typo structure is exactly what hand-rolling escape codes
    gets wrong.

#### How a terminal decides what it can do

Not every terminal supports every tier. Truecolor in particular is advertised
through an environment variable:

```bash
echo "$COLORTERM"   # prints 'truecolor' or '24bit' on capable terminals
echo "$TERM"        # e.g. 'xterm-256color' implies 256-color support
```

- If `COLORTERM` is `truecolor`/`24bit` → 24-bit RGB works.
- Else if `TERM` contains `256color` → use the 256-color palette.
- Else → fall back to the 16 basic colors.

A robust program (or framework) **downgrades gracefully**: it picks the closest
256-palette color when truecolor is unavailable, and the closest of the 16 when
even 256 isn't there. Your design keeps looking right; only the precision drops.

```text
truecolor (24-bit RGB)  ──►  256-color palette  ──►  16 basic colors
   16.7M colors              fixed 256-slot table     theme-dependent
```

### Three worked examples

```bash
# 1. Bold red text.
printf '\x1b[1;31mError:\x1b[0m something broke\n'

# 2. A truecolor gradient line (red -> green across 32 cells).
for i in $(seq 0 31); do
  r=$(( 255 - i*8 )); g=$(( i*8 ))
  printf '\x1b[48;2;%d;%d;0m ' "$r" "$g"
done; printf '\x1b[0m\n'

# 3. Reverse video — swap foreground and background for a "selected" look.
printf 'normal \x1b[7m SELECTED \x1b[0m normal\n'
```

---

## Modes worth knowing

Beyond cursor and color, terminals expose **modes** — stateful switches toggled
with the `?...h` (set/high) and `?...l` (reset/low) private-mode form. Two of
them are essential for full-screen apps.

### Alternate screen buffer — `ESC[?1049h` / `ESC[?1049l`

The terminal has two screens: the **main** buffer (your normal scrollback
history) and an **alternate** buffer (a blank, scrollback-free scratch
surface).

```text
ESC [ ? 1049 h      switch to the alternate screen
ESC [ ? 1049 l      switch back to the main screen
```

When you run `vim`, `less`, `htop`, or `top`, they switch to the alternate
buffer on startup and switch back on exit. That's why, after you quit `vim`,
**your shell history reappears exactly as it was** — the editor never touched
the main buffer; it painted on the alternate one and then discarded it.

```bash
# Enter the alternate screen, draw, wait, then restore the shell.
printf '\x1b[?1049h\x1b[2J\x1b[HI am on the alternate screen!'
sleep 2
printf '\x1b[?1049l'
echo "...and now your old terminal contents are back."
```

!!! tip "This is why your scrollback is safe"
    Full-screen TUIs use the alternate buffer specifically so they can clear
    and repaint freely without destroying the commands and output you had
    before you launched them. Restoring the main buffer on exit is part of
    being a well-behaved terminal citizen.

### Synchronized output (DEC mode 2026) — `ESC[?2026h` / `ESC[?2026l`

Here's a subtle problem. To redraw a frame, a program emits *many* escape
sequences — move here, set color, write text, move there, write more. If the
terminal renders each piece the instant it arrives, the user can briefly see a
**half-drawn frame**: text in the old position next to text in the new one.
This visual glitch is called **tearing**.

**Synchronized output** fixes it. You wrap a frame between a *begin* and *end*
marker; the terminal buffers everything in between and presents it as one atomic
update.

```text
ESC [ ? 2026 h      begin synchronized update (buffer this frame)
ESC [ ? 2026 l      end synchronized update   (present it all at once)
```

```bash
# Pseudocode shape of a tear-free frame:
#   printf '\x1b[?2026h'        # begin: terminal starts buffering
#   ... all your moves/colors/text for this frame ...
#   printf '\x1b[?2026l'        # end: terminal shows the finished frame
```

Terminals that don't understand mode 2026 simply ignore the markers and render
as before — so it's safe to always emit them. The payoff on terminals that *do*
support it is buttery, flicker-free updates.

---

## Why doing this by hand hurts

Everything above is mechanical and, in isolation, simple. The pain comes from
*composition and bookkeeping*:

- **One wrong byte corrupts the display.** Forget the `m` on an SGR sequence and
  the terminal keeps swallowing your text as parameters. Write `\x1b[38;2;200`
  with a missing channel and the next character vanishes into the sequence.
  A torn or truncated escape sequence (e.g. output cut off mid-write) can
  scramble everything that follows.
- **State is invisible and sticky.** You set bold; you must unset bold. You hide
  the cursor; you must show it. You enter the alternate screen; you must leave
  it. Miss any of these — especially on an error path or crash — and the user's
  terminal is left in a **broken mode** (no cursor, stuck colors, wrong
  buffer). Recovering means typing `reset` blind.
- **Capability juggling.** Truecolor here, 256 there, 16 on that old SSH session
  — you have to detect and downgrade or your carefully chosen colors come out
  wrong.
- **Efficiency.** Naively repainting the whole screen every frame floods the
  output and flickers. Doing it well means tracking what actually changed and
  emitting the *minimal* set of sequences — diffing the screen, batching with
  synchronized output, erasing only stale cells.

!!! note "Where maya comes in"
    [maya](../00-introduction.md) generates **correct, minimal** escape
    sequences for you. You describe *what* the UI should look like; maya figures
    out the bytes — picking the right color tier for the user's terminal,
    diffing frames so it only repaints what changed, wrapping each frame in
    synchronized output to avoid tearing, and always restoring the terminal to a
    clean state on exit. Understanding the codes on this page is what lets you
    reason about *why* it does what it does — but you won't be hand-typing
    `\x1b[38;2;...` in application code.

---

## Quick reference

The sequences you'll reach for most often. `ESC` = `\x1b` = `\033` = `\e`.

| Sequence | Meaning |
|----------|---------|
| `ESC[<r>;<c>H` | move cursor to row `r`, col `c` (1-based) |
| `ESC[<n>A` / `B` / `C` / `D` | move cursor up / down / right / left `n` |
| `ESC7` / `ESC8` | save / restore cursor position |
| `ESC[?25l` / `ESC[?25h` | hide / show cursor |
| `\r` | carriage return (cursor to column 1) |
| `\n` | line feed (cursor down one line) |
| `ESC[K` | erase to end of line |
| `ESC[2K` | erase entire line |
| `ESC[2J` | erase entire screen |
| `ESC[3J` | erase scrollback |
| `ESC[0m` | **reset all styles** |
| `ESC[1m` / `2m` / `3m` / `4m` / `7m` / `9m` | bold / dim / italic / underline / reverse / strike |
| `ESC[30–37m` / `40–47m` | basic fg / bg color |
| `ESC[90–97m` / `100–107m` | bright fg / bg color |
| `ESC[38;5;<n>m` / `48;5;<n>m` | 256-palette fg / bg |
| `ESC[38;2;<r>;<g>;<b>m` / `48;2;...m` | truecolor fg / bg |
| `ESC[?1049h` / `?1049l` | enter / leave alternate screen |
| `ESC[?2026h` / `?2026l` | begin / end synchronized output |

---

## What's next

You now know how to *write* to the terminal — how a program paints styled,
positioned output. The other half of a TUI is reading back what the user does:
keystrokes, arrow keys, and mouse clicks arrive as their own (often
escape-code-shaped) byte sequences. That's the subject of the next Foundations
page:

→ **[Keyboard & Mouse Input](keyboard-and-mouse.md)**
