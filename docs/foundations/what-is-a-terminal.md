# What Is a Terminal?

You write code every day. You probably *open* a terminal every day. But have you
ever stopped to ask what a terminal actually **is**? Not "the black window where I
type `git status`" — but the real, layered machine underneath: what it draws, what
it understands, and why it behaves the way it does?

This is the first page of the **Foundations** series. By the end of it you'll have
a precise mental model of the terminal — the thing maya draws onto. Everything else
in this series (cells, escape codes, raw mode, rendering) builds on the ideas here.
We start from absolute zero. No prior TUI knowledge assumed.

!!! note "Who this is for"
    A capable programmer who has never had to think hard about terminals. If you
    can write a `for` loop but you've never wondered what `\033[2J` does, you're in
    exactly the right place.

---

## The one idea that changes everything

Here is the single most important thing to internalize, and almost everything else
follows from it:

!!! tip "The mental model"
    **A terminal is a grid of character cells. It is *not* a pixel canvas.**

When you draw in a web browser or a game, you think in **pixels**: "put a red dot at
(x=412, y=88)." You have millions of independently addressable points and you can
paint anything.

A terminal is nothing like that. A terminal is a **rectangular grid**, like a
spreadsheet or a sheet of graph paper. Each box in the grid is a **cell**, and each
cell holds **exactly one character** plus a little bit of styling (a foreground
color, a background color, maybe bold or underline). You don't address pixels — you
address **rows and columns**.

```text
        col 0   col 1   col 2   col 3   col 4   ...   col 79
       ┌───────┬───────┬───────┬───────┬───────┬─────┬───────┐
row 0  │   H   │   e   │   l   │   l   │   o   │ ... │       │
       ├───────┼───────┼───────┼───────┼───────┼─────┼───────┤
row 1  │   W   │   o   │   r   │   l   │   d   │ ... │       │
       ├───────┼───────┼───────┼───────┼───────┼─────┼───────┤
row 2  │       │       │       │       │       │ ... │       │
       └───────┴───────┴───────┴───────┴───────┴─────┴───────┘
                  (a classic 80×24 terminal: 80 columns, 24 rows)
```

A "standard" terminal has historically been **80 columns wide and 24 rows tall**.
That number isn't arbitrary — and the reason why is a tiny piece of history worth
knowing.

!!! note "Why 80 columns?"
    The 80-column width is a fossil from **IBM punch cards**, which held 80
    characters per card. Early terminals matched that width, and the convention
    stuck so hard that "keep lines under 80 columns" is *still* a coding-style rule
    in many projects almost a century later.

Every cell is the **same size**. Characters are monospaced — an `i` and a `W`
occupy the same box. This is why ASCII art lines up in a terminal but turns to
garbage if you paste it into a word processor with a proportional font. The grid
*depends* on every glyph being one cell wide.

!!! warning "Some characters are *two* cells wide"
    The "one character per cell" rule has a famous exception: many East Asian
    characters (CJK) and a lot of emoji occupy **two** cells, not one. A 🐈 or a 漢
    is "double-width." This single fact is responsible for an astonishing amount of
    terminal pain — text that should line up suddenly doesn't, because the program
    counted *characters* when it should have counted *cells*. We'll return to this
    in **The Cell Grid**; for now, just file away that "one char = one cell" is
    *mostly* true, not *always* true.

---

## A little history (it explains the weirdness)

Terminals are weird. They have escape codes, "raw mode," control characters, and a
pile of conventions that seem to come from nowhere. They make a lot more sense once
you know they're emulating hardware from the 1960s and 70s. The abstractions you use
today exist because they had to talk to *physical machines*.

### Act 1: Teletypes (the literal typewriters)

The earliest "terminals" were **teletypewriters** — electromechanical typewriters
hooked up to a computer over a wire. You typed a key, a byte went down the wire to
the computer; the computer sent bytes back, and a print head physically hammered
them onto a roll of **paper**.

```text
   ┌──────────────┐                       ┌────────────┐
   │   TELETYPE    │   bytes over a wire   │            │
   │  (keyboard +  │ ◄───────────────────► │  COMPUTER  │
   │   paper roll) │                       │            │
   └──────────────┘                       └────────────┘
```

This is where the abbreviation **TTY** comes from — *T*ele*TY*pewriter. You still
see it everywhere: `/dev/tty`, the `tty` command, "is this a tty?". A 60-year-old
piece of hardware is baked into the names of things you use today.

Because the output was *paper*, you couldn't "move the cursor up" to redraw a line —
the paper had already moved on. The only thing you could do was move forward and
emit special **control characters** to nudge the mechanism:

| Character | Name | What the teletype did |
|-----------|------|------------------------|
| `\r` (13) | Carriage Return | Slam the print head back to the left margin |
| `\n` (10) | Line Feed | Roll the paper up one line |
| `\t` (9)  | Horizontal Tab | Advance to the next tab stop |
| `\a` (7)  | Bell | Ring an actual physical **bell** |
| `\b` (8)  | Backspace | Move the head left one position |

!!! note "Why is it `\r\n` on Windows?"
    On a real teletype, ending a line meant doing *two* physical things: return the
    carriage (`\r`) **and** feed the paper (`\n`). Windows still writes both bytes
    for a newline. Unix decided one byte (`\n`) was enough and let the driver handle
    the rest. That decades-old hardware detail is *literally* why your git diffs
    sometimes scream about line endings.

### Act 2: Video terminals (the glass teletype)

In the 1970s, paper was replaced by a **CRT screen** — a "glass teletype." The most
famous was the **DEC VT100** (1978). Now something new was possible: because the
output was a screen, not paper, you could **move the cursor anywhere and overwrite
what was already there.**

But there was a problem. The wire could only carry bytes, and most bytes were needed
for ordinary text. How do you say "move the cursor to row 5, column 10" using a
stream that's mostly carrying letters?

The answer was **escape sequences**: a special byte — the **Escape** character
(`\033`, also written `\x1b` or `ESC`) — signals "the next few bytes aren't text,
they're a *command*." So `ESC [ 5 ; 10 H` means "move the cursor to row 5,
column 10." The VT100's command set became so dominant that it's effectively the
*standard* — which is why your $TERM is probably still `xterm-256color`, a direct
descendant.

```text
   ┌──────────────┐    bytes (text + escape codes)    ┌────────────┐
   │   VT100       │ ◄───────────────────────────────► │  COMPUTER  │
   │ (CRT screen + │                                    │            │
   │  keyboard)    │   ESC[5;10H  = "go to row 5,col10" └────────────┘
   └──────────────┘
```

!!! tip "This is the punchline of the whole history lesson"
    **Escape codes and raw mode aren't legacy cruft you have to tolerate — they're
    the actual API of the terminal.** Every colored prompt, every progress bar,
    every full-screen text editor works by writing VT100-lineage escape sequences to
    a byte stream. maya, vim, htop, and your shell's fancy prompt all speak the same
    1978 language.

### Act 3: Terminal emulators (today)

We don't have VT100 hardware on our desks anymore. Instead, we run a **terminal
emulator**: a normal graphical application that *pretends to be* a VT100 (and then
some). It opens a window, draws the cell grid using a font, and faithfully
interprets the same escape codes the real hardware did.

When you open **iTerm2**, **Alacritty**, **Windows Terminal**, **GNOME Terminal**,
**kitty**, or the **integrated terminal in VS Code or Zed**, you're running a
terminal *emulator*. The hardware is gone; the protocol lives on.

---

## The four words people mix up: terminal, emulator, shell, TTY/PTY

Beginners (and plenty of experts) blur these together. Let's separate them cleanly,
because every TUI bug eventually forces you to know which layer you're standing on.

| Term | What it is | Concrete example |
|------|-----------|------------------|
| **Terminal emulator** | The GUI app that draws the grid and handles your keyboard/mouse | iTerm2, Alacritty, Windows Terminal, GNOME Terminal, Zed's built-in terminal |
| **Shell** | A *program* that reads commands and runs them; it runs **inside** the terminal | `bash`, `zsh`, `fish`, `pwsh` |
| **TTY / PTY** | The kernel's pipe-with-special-powers that connects the two | `/dev/pts/3` on Linux |
| **Terminal** | Loosely, all of the above together — or strictly, the historical hardware | "open a terminal" |

The key insight that surprises beginners:

!!! tip "Your shell is just a program running inside the terminal"
    The terminal emulator does **not** know what `bash` is. It draws a grid and
    shunts bytes back and forth. `bash` is simply the program that happens to be
    running inside it. You can replace `bash` with `python`, or `vim`, or maya app —
    the terminal doesn't care. It's a dumb, beautiful pipe-and-grid.

Here's how the layers actually stack up:

```text
   ┌─────────────────────────────────────────────────────────────┐
   │  TERMINAL EMULATOR  (e.g. Alacritty)                          │
   │  • a GUI window on your desktop                               │
   │  • draws the cell grid with a font                            │
   │  • captures keypresses & mouse, sends them as bytes           │
   │  • reads bytes back and interprets text + escape codes        │
   │                                                               │
   │     writes bytes ▼          ▲ reads bytes                     │
   │   ┌───────────────────────────────────────────────────────┐  │
   │   │  KERNEL  pty (pseudo-terminal)                          │  │
   │   │  • the "master" end talks to the emulator               │  │
   │   │  • the "slave" end (/dev/pts/N) looks like a real tty   │  │
   │   │  • a line-discipline layer does echo, line editing,     │  │
   │   │    Ctrl-C → SIGINT, etc. (this is what "raw mode" turns  │  │
   │   │    off — more on that later in the series)               │  │
   │   └───────────────────────────────────────────────────────┘  │
   │     stdin ▲ ▼ stdout/stderr                                   │
   │   ┌───────────────────────────────────────────────────────┐  │
   │   │  YOUR PROGRAM  (the shell, or vim, or a maya TUI)       │  │
   │   │  • reads bytes from stdin                               │  │
   │   │  • writes bytes to stdout                               │  │
   │   │  • has NO idea a human or a GUI is on the other side    │  │
   │   └───────────────────────────────────────────────────────┘  │
   └─────────────────────────────────────────────────────────────┘
```

### What's a PTY, exactly?

The original TTY was a wire to physical hardware. Today there's no hardware, so the
kernel provides a **PTY** — a **pseudo-terminal**. It's a software object with two
ends:

- The **master** end is held by the terminal emulator.
- The **slave** end (named `/dev/pts/3`, `/dev/pts/7`, etc. on Linux) is handed to
  your program as its stdin/stdout/stderr. To your program, it looks *exactly* like
  a real terminal would.

The PTY is more than a dumb pipe. It has a **line discipline** — a kernel layer that,
by default, does helpful things: it **echoes** your keystrokes back so you can see
what you type, it buffers a whole line so backspace works before you hit Enter, and
it turns `Ctrl-C` into a `SIGINT` signal. TUI programs usually *turn most of this
off* by switching to **raw mode**, because they want every keystroke immediately and
unprocessed. (Raw mode gets its own page later in the series.)

!!! note "See it for yourself"
    Run `tty` in your shell. It prints the path of the slave PTY you're attached to,
    like `/dev/pts/2`. Open a second terminal and run `tty` there — you'll get a
    *different* number. Each window is its own pseudo-terminal.

---

## Everything is a byte stream

Strip away the history and the layers, and the terminal's contract is shockingly
simple:

!!! tip "The contract"
    Your program **writes bytes to stdout**. The terminal looks at each byte and
    decides: is this a **printable character** (put a glyph in a cell, advance the
    cursor) or part of a **control sequence** (do something special, like move the
    cursor or change the color)? Meanwhile, anything the user types arrives as
    **bytes on stdin**.

That's it. There is no "draw button" API, no "set pixel" call. There's a stream of
bytes going out and a stream of bytes coming in. A "user interface" is an *illusion*
you construct by writing exactly the right bytes in the right order.

### Output: text vs. control sequences

When the terminal reads bytes from your program, it sorts them into two buckets:

```text
   bytes your program writes:   H  e  l  l  o  ESC [ 3 1 m  W  o  r  l  d
                                └──── text ────┘└─ command ─┘└─ text ──┘
                                      │              │            │
                                      ▼              ▼            ▼
                            put glyphs in cells   "set fg     put glyphs,
                                                   to red"     now red
```

- **Printable bytes** (`H`, `e`, `l`, a space, a digit...) → drop a glyph into the
  current cell and move the cursor right by one.
- **An ESC byte** (`\033`) → "a command is coming." The terminal reads the following
  bytes as an instruction: move the cursor, change colors, clear the screen, hide
  the cursor, and so on.

This is the whole game. A full-screen app like vim or a maya dashboard is just an
extremely well-organized stream of "move here, set this color, print these glyphs,
move there, print those glyphs" — emitted 60 times a second, fast enough that your
eye reads it as a stable, updating picture.

### Input: keystrokes are bytes too

Input is the mirror image. Press `a` and the byte `0x61` arrives on stdin. Press
`Enter` and you get `\r` (or `\n`). The interesting cases are the **special keys**:
arrows, function keys, Page Up. Those don't have a single byte — so the terminal
sends them *as escape sequences too*. Pressing the **Up arrow** typically sends the
three bytes `ESC [ A`.

!!! note "Yes — input and output use the same escape-code language"
    The Up arrow sends `ESC[A`; moving the cursor up one row is `ESC[1A`. The
    overlap is not a coincidence. Both directions speak the VT100 dialect. This is
    also why a single stray `Ctrl-V` followed by a keypress in your shell will print
    the raw bytes that key produces — a great party trick and a great debugging tool.

---

## Capability detection: `$TERM`, `$COLORTERM`, `$TERM_PROGRAM`

Not all terminals can do all things. Some only do 16 colors; some do 256; modern
ones do 16 million ("true color"). Some support fancy underlines, hyperlinks, or
mouse reporting; some don't. Since your program writes blind into a byte stream, how
does it know what the terminal on the other end can handle?

Mostly by reading a few **environment variables** the terminal sets for you:

| Variable | What it tells you | Typical value |
|----------|-------------------|---------------|
| `$TERM` | The terminal *type* — historically an index into the `terminfo` capability database | `xterm-256color`, `screen`, `tmux-256color` |
| `$COLORTERM` | A hint about color depth, mainly used to detect 24-bit truecolor | `truecolor` or `24bit` |
| `$TERM_PROGRAM` | Which emulator app you're in (set by some, not all) | `iTerm.app`, `vscode`, `Apple_Terminal`, `WezTerm` |

```bash
echo "$TERM"          # e.g. xterm-256color
echo "$COLORTERM"     # e.g. truecolor   (may be empty)
echo "$TERM_PROGRAM"  # e.g. vscode      (may be empty)
```

`$TERM` is the old-school mechanism: it names a profile in **terminfo**, a database
that maps capabilities ("how do I clear the screen on *this* terminal?") to the exact
escape sequences. The `tput` command is the friendly front-end to that database (try
the snippets below).

!!! warning "Don't over-trust these variables"
    These variables are *hints*, not guarantees. `$TERM` is frequently set to a
    conservative value, `$COLORTERM` is often missing even on capable terminals, and
    `$TERM_PROGRAM` isn't set by many emulators at all. Robust TUI frameworks
    combine these hints with actual runtime probing of the terminal. The takeaway
    for now: capability detection is *a real problem*, and it's one of the many
    things a framework like maya handles so you don't have to.

---

## stdin, stdout, stderr, and "isatty"

Every program starts life with three byte streams already open:

| Stream | Number (fd) | Default destination | Purpose |
|--------|-------------|---------------------|---------|
| **stdin**  | 0 | your keyboard (via the PTY) | bytes coming *in* |
| **stdout** | 1 | your terminal screen | normal output |
| **stderr** | 2 | your terminal screen | errors & diagnostics |

The crucial twist: **these streams don't have to point at a terminal at all.** The
shell can redirect them. That's the whole basis of Unix pipes:

```bash
ls                 # stdout → your terminal (you see a grid of names)
ls > files.txt     # stdout → a file        (nothing on screen)
ls | grep ".md"    # stdout → another program's stdin
```

When stdout goes to a file or a pipe, **there is no cell grid on the other end** —
just a sink that swallows bytes. So a program needs to ask: "Am I actually attached
to a terminal right now, or to a file/pipe?" The answer comes from a system call
named **`isatty`** ("is a TTY?").

!!! tip "Why `ls` changes its mind when you pipe it"
    Run `ls` in a terminal and you get neat columns, often with color. Run
    `ls | cat` and suddenly it's one filename per line, no color. `ls` calls
    `isatty(stdout)`: if it's a real terminal, it formats for humans (columns,
    color); if it's a pipe, it formats for machines (one item per line, no escape
    codes that would corrupt the data downstream). Many well-behaved tools do this.

This is also why you should write **diagnostics to stderr, not stdout**: if someone
does `myprogram > output.txt`, your error messages still reach the screen instead of
silently polluting `output.txt`.

!!! warning "A TUI needs a real terminal"
    A full-screen TUI fundamentally *requires* a real terminal — it needs to know
    the grid dimensions, move the cursor, and read raw keystrokes. Piping a TUI's
    output to a file produces a mess of literal escape codes. Frameworks like maya
    check `isatty` and refuse (or degrade gracefully) when there's no terminal on the
    other end, which is exactly the right thing to do.

---

## Try it yourself

Reading about byte streams is fine; *feeling* them is better. Open a terminal and
run these. Each one writes raw bytes and lets you watch the grid react. None of them
will harm anything.

**1. Plain text vs. an escape code.** `printf` (unlike some `echo`s) interprets
backslash escapes, so we can send a raw `ESC` byte (`\033`):

```bash
# 31 = red foreground, 0 = reset back to normal
printf '\033[31mThis is red\033[0m and this is normal\n'
```

**2. Move the cursor around the grid.** This jumps to row 5, column 20 and prints
there, proving you address cells by (row, column):

```bash
printf '\033[5;20HI am at row 5, column 20\n'
```

**3. Ring the 1960s bell.** The byte `\a` is a direct descendant of the teletype
bell:

```bash
printf '\a'   # your terminal beeps or flashes
```

**4. Ask `tput` instead of memorizing codes.** `tput` looks up the right sequence
for *your* terminal in terminfo:

```bash
tput cols       # how many columns wide is your grid right now?
tput lines      # how many rows tall?
tput bold; echo "bold text"; tput sgr0   # sgr0 = reset all attributes
tput setaf 2; echo "green text"; tput sgr0   # setaf 2 = foreground green
```

!!! tip "Resize your window and run `tput cols` again"
    The number changes. The grid isn't fixed — it's whatever your emulator window
    currently is. (When the size changes, the kernel sends your program a `SIGWINCH`
    signal so it can re-layout. That's how `htop` and maya apps reflow instantly when
    you drag the window edge.)

**5. See the line discipline with `stty`.** This prints your terminal's current
input settings — echo, line editing, the works:

```bash
stty -a
```

You'll spot flags like `echo` and `icanon` (canonical/line-buffered mode). When a
TUI enters raw mode, it's flipping exactly these. (Don't change them by hand right
now — if you ever leave a terminal in a weird state, the magic incantation `stty
sane` or just typing `reset` puts it right.)

**6. Peek at the layers.**

```bash
tty            # which pseudo-terminal am I on? e.g. /dev/pts/2
echo "$TERM"   # what terminal type am I claiming to be?
ps             # see the shell process running *inside* this terminal
```

---

## So why is this hard? (Why maya exists)

You now know the terminal's real contract: **bytes out, bytes in, a grid of cells,
and a 1970s escape-code dialect.** It looks simple. Drawing a real UI on it is
not. Just a partial list of what you'd have to handle by hand:

- **Diffing the grid.** Repainting the whole screen every frame is slow and flickers.
  You want to send bytes *only* for the cells that actually changed since last frame.
- **Layout.** "Center this box, put a sidebar on the left, wrap that text" — there's
  no layout engine. You compute every row and column yourself.
- **Double-width characters & Unicode.** Remember the 🐈 problem? Get the width wrong
  and your whole UI shears sideways.
- **Capability detection.** 16 vs 256 vs truecolor, and degrading gracefully when a
  terminal can't keep up.
- **Raw mode & input parsing.** Turning `ESC [ A` back into "the user pressed Up,"
  while not mistaking a real `ESC` keypress for the start of a sequence.
- **Resize, cursor hiding, alternate screen, cleanup on crash** — and a dozen more
  papercuts.

!!! note "This is the gap maya fills"
    **maya** is a C++ TUI framework that sits on top of everything you just learned.
    You describe *what* the UI should look like — boxes, text, colors, layout — and
    maya figures out the exact bytes to write, when to write them, and how to do it
    fast. You think in cells and components; maya speaks fluent VT100 underneath. The
    rest of this Foundations series teaches you that underlying machinery so that when
    you use maya (or debug a TUI at 2am), you understand exactly what's happening.

---

## What's next

You've got the big picture: a terminal is a **byte-stream-driven grid of cells**,
emulating decades-old hardware, with a shell or your program running inside it.

The next page zooms all the way into that grid. What *exactly* lives in a single
cell? How do characters, colors, and attributes pack together? Why is the cell — not
the pixel — the atom of everything maya draws?

[**Next: The Cell Grid →**](the-cell-grid.md)

!!! tip "Foundations recap so far"
    - A terminal is a **grid of character cells** (rows × columns), not a pixel
      canvas.
    - Today's terminals are **emulators** faithfully imitating hardware like the
      VT100.
    - **Emulator ≠ shell ≠ TTY/PTY** — they're distinct layers; your shell is just a
      program running inside the terminal.
    - Communication is a **byte stream**: printable bytes become glyphs; `ESC`
      sequences are commands. Input arrives as bytes too.
    - `$TERM` / `$COLORTERM` / `$TERM_PROGRAM` and `isatty` let a program discover
      *where* it's running and *what* it can do.
