# Keyboard & Mouse Input

You press a key. A letter appears. Surely there's a function somewhere called
`getKeyPress()` and that's the end of it?

Not even close. Reading input in a terminal is one of the most surprisingly
deep corners of TUI programming. The terminal was designed in the 1970s to talk
to physical teletype machines over serial lines, and almost every quirk you'll
fight today is an echo of that history. By the end of this page you'll know
exactly what happens between a keypress and your program — and why a real
framework spends a lot of code hiding it from you.

This is the fourth page in the **Foundations** series. We've covered what a
terminal *is* and how text gets *out* of your program. Now we go the other
direction: how bytes get *in*.

---

## The two modes of a terminal

Every terminal (more precisely, the *terminal driver* in your operating system)
operates in one of two fundamental modes. Understanding the difference is the
single most important idea on this page.

### Cooked mode (canonical mode)

This is the default. It's what you get when you write a normal command-line
program that calls `read()` or `scanf` or `std::getline`. It's called "cooked"
because the kernel does a lot of helpful preparation before your program ever
sees the data.

In cooked mode, **the kernel line-buffers input**. When the user types, the
following all happen *inside the operating system*, before your program is
involved at all:

- Characters are echoed to the screen automatically (you see what you type).
- **Backspace** erases the previous character from the buffer. Your program
  never learns the character was typed and then deleted.
- **Ctrl-C** raises `SIGINT` and (by default) kills your program.
- **Ctrl-Z** suspends it. **Ctrl-D** signals end-of-input.
- **Ctrl-U** / **Ctrl-W** do line-kill and word-erase.
- Nothing is handed to your program until the user presses **Enter**. At that
  point the *entire finished line* arrives in one `read()`.

This is wonderful for a shell command. You ask a question, the user types an
answer, fixes their typos, and presses Enter; you get one clean string. The
kernel did the editing for you.

It's a disaster for a TUI.

A TUI needs to react the *instant* a key is pressed — arrow keys should move a
cursor immediately, `j`/`k` should scroll without an Enter, and you certainly
don't want the kernel echoing keystrokes wherever the text cursor happens to be,
scribbling over your carefully drawn interface. You also want **Ctrl-C** to be a
key event you can choose to handle, not an instant death sentence.

### Raw mode

Raw mode turns off all that helpfulness. In raw mode:

- **No echo.** Keystrokes are not auto-printed. *You* decide what appears on
  screen.
- **No line buffering.** Every byte is delivered to your program the moment it
  arrives — no waiting for Enter.
- **No line editing.** Backspace is just a byte (`0x7F`); it's your job to
  decide what it means.
- **No signal generation** (optional). Ctrl-C arrives as the byte `0x03` instead
  of killing you — unless you choose to leave signals on.

This is the mode every full-screen TUI runs in. The terminal becomes a dumb pipe
that hands you raw bytes, and your program takes full responsibility for
interpreting them.

!!! note "Cooked vs raw, at a glance"
    Cooked mode = "the kernel helps you read a *line*."
    Raw mode = "give me every *byte*, right now, and get out of my way."

### How you actually switch modes

On POSIX systems (Linux, macOS, BSD), terminal behavior is controlled through a
struct called `termios`, manipulated with `tcgetattr()` and `tcsetattr()`. You
read the current settings, flip some bits, and write them back. The relevant
flags are:

| Flag | Lives in | Controls | Raw mode |
|------|----------|----------|----------|
| `ICANON` | local flags (`c_lflag`) | Canonical (line-buffered) input | **off** |
| `ECHO` | local flags (`c_lflag`) | Auto-echo of typed characters | **off** |
| `ISIG` | local flags (`c_lflag`) | Ctrl-C/Ctrl-Z → signals | usually **off** |
| `IXON` | input flags (`c_iflag`) | Ctrl-S/Ctrl-Q flow control | **off** |
| `ICRNL` | input flags (`c_iflag`) | Translate CR → newline on input | **off** |
| `OPOST` | output flags (`c_oflag`) | Output post-processing | often **off** |

A minimal "enter raw mode" in C looks roughly like this:

```c
#include <termios.h>
#include <unistd.h>

struct termios original;          // save the cooked settings!

void enable_raw_mode(void) {
    tcgetattr(STDIN_FILENO, &original);   // 1. read current state
    struct termios raw = original;        // 2. copy it
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);   // 3. flip the bits off
    raw.c_iflag &= ~(IXON | ICRNL);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);  // 4. apply
}

void disable_raw_mode(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);  // restore!
}
```

!!! info "Windows does the same thing differently"
    Windows has no `termios`. Instead the console exposes `GetConsoleMode()` /
    `SetConsoleMode()`, where you clear flags like `ENABLE_LINE_INPUT`,
    `ENABLE_ECHO_INPUT`, and `ENABLE_PROCESSED_INPUT` — the conceptual
    equivalents of `ICANON`, `ECHO`, and `ISIG`. The idea is identical; only the
    API differs. A cross-platform framework wraps both.

---

## The single most important rule: restore the terminal

Notice the `original` variable in the code above. That's not optional bookkeeping
— it's the whole ballgame.

!!! danger "If you enable raw mode, you MUST restore cooked mode on exit"
    The terminal's mode is **global state owned by the terminal, not by your
    process.** When your program exits, the OS does *not* automatically reset it.
    If you crash, get killed, or simply forget to restore — the user's shell is
    left in raw mode.

    They'll type and see *nothing* (echo is off). Ctrl-C won't work (signals are
    off). Enter won't start a new line (CR translation is off). It looks like
    their terminal is completely broken. The fix is to blindly type `reset` or
    `stty sane` and hit Enter, but a beginner won't know that — they'll just close
    the window and curse your program.

This is why robust frameworks treat tty restoration as a *safety-critical*
operation, not a tidy-up step. A naive program restores the terminal at the end
of `main()`. But what if you never *reach* the end of `main()`?

A serious TUI library installs restoration on every plausible exit path:

- **Normal exit** — via `atexit()` / RAII destructors.
- **Signals** — a handler for `SIGINT`, `SIGTERM`, `SIGQUIT` that restores the
  terminal *before* re-raising the signal to die properly.
- **Crashes** — even a handler for `SIGSEGV` / `SIGABRT` that restores the tty as
  its dying act, so a segfault in your app doesn't also wreck the user's shell.
- **Suspension** — restore on `SIGTSTP` (Ctrl-Z) and re-enter raw mode on
  `SIGCONT` when the job is resumed.

Getting all of this right is fiddly and easy to forget. This is one of the first
things maya handles for you: enter `run()` and the terminal is put into raw mode;
leave it — *however* you leave it, including via a crash — and the terminal is
handed back exactly as it was found.

---

## How keys actually arrive: it's just bytes

Once you're in raw mode, `read(STDIN_FILENO, buf, n)` hands you bytes. That's the
entire interface. There is no rich `KeyEvent` waiting for you; there is a stream
of `uint8_t`. Your job is to decode meaning from it. Keys fall into three
categories.

### 1. Printable keys → their bytes

The easy case. Press `a`, you get one byte: `0x61`. Press `A` (shift held), you
get `0x41`. The shift key itself is invisible — the terminal already folded it
into the resulting character.

Non-ASCII printable characters arrive as their **UTF-8 encoding**, which means a
single keypress can be multiple bytes:

| Key | Bytes (hex) | Notes |
|-----|-------------|-------|
| `a` | `61` | one byte |
| `A` | `41` | shift already applied |
| `é` | `C3 A9` | 2-byte UTF-8 |
| `€` | `E2 82 AC` | 3-byte UTF-8 |
| `😀` | `F0 9F 98 80` | 4-byte UTF-8 |

So even "just a printable character" requires UTF-8 decoding to reassemble
multi-byte runes from the byte stream — and those bytes can be split across
separate `read()` calls.

### 2. Control keys → control bytes

The bottom 32 byte values (`0x00`–`0x1F`) are *control codes*. Historically,
holding **Ctrl** and pressing a letter clears the top bits of its ASCII code,
which is why the mapping is so regular: Ctrl-A is `0x01`, Ctrl-B is `0x02`, and
so on (`Ctrl-<letter>` = letter's position in the alphabet).

| Key | Byte | Name |
|-----|------|------|
| Ctrl-A | `0x01` | SOH |
| Ctrl-C | `0x03` | ETX (would be SIGINT in cooked mode) |
| Ctrl-D | `0x04` | EOT |
| Ctrl-H | `0x08` | Backspace (sometimes) |
| Tab | `0x09` | also Ctrl-I |
| Enter / Return | `0x0D` | CR — also Ctrl-M |
| Ctrl-J | `0x0A` | LF — newline |
| Esc | `0x1B` | ESC — remember this one! |
| Backspace | `0x7F` | DEL — what most keyboards actually send |

Two notorious overlaps worth burning into memory:

- **Tab and Ctrl-I are the same byte** (`0x09`). **Enter and Ctrl-M are the same
  byte** (`0x0D`). In a plain terminal you literally *cannot* distinguish them —
  they are indistinguishable at the byte level. (The Kitty protocol, below, fixes
  this.)
- **Backspace is usually `0x7F` (DEL), not `0x08`.** This trips up everyone the
  first time. If your backspace handling does nothing, check whether you're
  matching the wrong byte.

### 3. Special keys → escape sequences

Here's the twist that makes terminal input genuinely tricky. Keys that aren't
characters — arrows, function keys, Home, End, Page Up — don't have a single
byte. They send a **multi-byte escape sequence**, almost always beginning with
the ESC byte (`0x1B`).

| Key | Bytes | As text |
|-----|-------|---------|
| Up arrow | `1B 5B 41` | `ESC [ A` |
| Down arrow | `1B 5B 42` | `ESC [ B` |
| Right arrow | `1B 5B 43` | `ESC [ C` |
| Left arrow | `1B 5B 44` | `ESC [ D` |
| Home | `1B 5B 48` | `ESC [ H` |
| End | `1B 5B 46` | `ESC [ F` |
| Page Up | `1B 5B 35 7E` | `ESC [ 5 ~` |
| Page Down | `1B 5B 36 7E` | `ESC [ 6 ~` |
| Insert | `1B 5B 32 7E` | `ESC [ 2 ~` |
| Delete | `1B 5B 33 7E` | `ESC [ 3 ~` |
| F1 | `1B 4F 50` | `ESC O P` |
| F2 | `1B 4F 51` | `ESC O Q` |
| F5 | `1B 5B 31 35 7E` | `ESC [ 1 5 ~` |

!!! warning "The ESC byte does double duty"
    This is the deep symmetry that confuses everyone. In the previous page we
    saw that **output** styling uses escape sequences: `ESC [ 31 m` to turn text
    red. Now we see that **input** special keys *also* arrive as escape sequences
    starting with `ESC [`. The very same byte (`0x1B`) that you write *out* to
    color your UI is the byte that comes *in* to signal an arrow key.

    The terminal is a half-duplex stream of bytes where ESC is the universal
    "something structured follows" marker — in both directions.

Note also the variation: arrows and Home/End use the `ESC [` (CSI) introducer,
while F1–F4 classically use `ESC O` (SS3), and F5+ go back to `ESC [ … ~`. There
is no single tidy rule; the sequences are a historical patchwork, and they vary
between terminal types (`xterm`, `rxvt`, `linux` console, etc.). This is exactly
the kind of mess a framework normalizes away.

---

## The Escape ambiguity problem

Now we hit the single nastiest gotcha in terminal input. Look again at the table
above: every special key starts with the ESC byte (`0x1B`). But ESC is *also* a
key the user can press all by itself (to cancel a dialog, leave a mode, etc.).

So when your `read()` returns a lone `0x1B`, you face an impossible question:

> Did the user press the **Escape key**, or is this the **first byte of an arrow
> key** whose remaining bytes (`[ A`) haven't arrived yet?

You cannot tell from the byte alone. The information simply isn't there yet.
There are two classic strategies, and real frameworks use a blend:

1. **Timeout disambiguation.** After seeing ESC, wait a short interval (commonly
   ~25–50 ms). If more bytes arrive almost instantly, it's a sequence — a human
   can't type `ESC` then `[` then `A` in single-digit milliseconds, but a
   terminal emits them back-to-back. If nothing arrives within the window, treat
   it as a standalone Escape press. This is why some TUIs feel like Escape has a
   tiny lag.

2. **Maximal-munch parsing.** Read what's available, try to match the longest
   valid sequence; if the bytes so far form a *complete* known sequence, emit it;
   if they're a *prefix* of one, wait for more.

Layered on top is a second headache: **sequences can be split across `read()`
calls.** The kernel might hand you `ESC [` in one read and `A` in the next, simply
because of how the bytes were buffered or how the terminal flushed them. So your
parser cannot be a simple `switch` on a fixed-size buffer. It must be a **stateful
machine** that remembers "I'm currently in the middle of a CSI sequence" across
reads, accumulating bytes until it has enough to decide.

!!! tip "This is the heart of why a parser exists"
    A lone byte is ambiguous. A sequence can arrive in pieces. Escape might be a
    key or a prefix. Any honest input layer is a small state machine, not a lookup
    table — and that state machine is precisely the kind of tedious, error-prone
    code maya writes once so you never have to.

---

## Mouse input

Yes, the terminal can report the mouse — and it does it, of course, through more
escape sequences. But mouse reporting is **off by default**. You opt in by
*writing* an escape sequence to the terminal, which flips a private mode on.

```text
ESC [ ? 1000 h     enable basic mouse: report button press/release
ESC [ ? 1002 h     also report motion while a button is held (drag)
ESC [ ? 1003 h     report ALL motion, even with no button down
ESC [ ? 1006 h     use SGR extended coordinate encoding (do this!)
```

(`h` = "high"/enable; the matching `… l` = "low"/disable. You must turn mouse
reporting back off on exit, just like raw mode.)

Once enabled, mouse actions arrive on stdin as escape sequences. The old default
encoding packed the button and coordinates into raw bytes and broke past column
223 — which is why you almost always also request `?1006h`, the **SGR extended**
format, which is readable and unbounded:

```text
ESC [ < Cb ; Cx ; Cy M     button event (press)   — note trailing capital M
ESC [ < Cb ; Cx ; Cy m     button release         — note trailing lowercase m
```

- `Cb` = a button/modifier code (left=0, middle=1, right=2; scroll wheel and
  shift/ctrl/alt modifiers are encoded by adding bit flags; motion sets bit 32).
- `Cx`, `Cy` = column and row, **1-based**.

For example, a left-click at column 10, row 5 looks like
`ESC [ < 0 ; 10 ; 5 M`, and letting go looks like `ESC [ < 0 ; 10 ; 5 m`. The
scroll wheel reports as buttons 64 (up) and 65 (down).

!!! warning "Any-motion mode (`?1003h`) is a firehose"
    With `?1003h` the terminal emits a fresh escape sequence for *every cell the
    pointer moves over*, button or no button. Move the mouse across the screen and
    you can get hundreds of sequences in a heartbeat. If your input loop and parser
    aren't ready for that volume — and you don't coalesce events — your app will
    stutter or fall behind. Most apps prefer `?1002h` (drag only) and reach for
    `?1003h` only when they truly need hover tracking.

---

## Bracketed paste

Here's a subtle one. When a user **pastes** a block of text into the terminal,
each character arrives on stdin exactly as if it had been typed. That's a problem:
if your app treats a newline in the pasted text as "the user pressed Enter, run
the command," pasting a multi-line snippet can trigger a cascade of unintended
actions. (This has even been a security issue — pasting text that secretly
contains a newline + a dangerous command.)

**Bracketed paste mode** solves it. Enable it by writing:

```text
ESC [ ? 2004 h     enable bracketed paste
```

Now, when text is pasted, the terminal wraps it in markers:

```text
ESC [ 200 ~   <the pasted bytes, verbatim>   ESC [ 201 ~
```

Everything between `ESC[200~` and `ESC[201~` was *pasted*, not typed. Your app can
treat it as literal text — insert it, ignore embedded newlines as line breaks
rather than command submissions, and stay safe. This is how editors and shells
tell "the human is typing" from "the human dumped in a blob."

---

## Modern protocols: Kitty / CSI-u

Everything above is the *legacy* world, and it has real holes:

- Tab and Ctrl-I are the same byte; so are Enter and Ctrl-M.
- You can't see modifier keys (Ctrl/Alt/Shift) in combination with most keys.
- Key *release* events don't exist.
- The Escape ambiguity requires guessing with a timeout.

Newer terminals support better schemes. The **Kitty keyboard protocol** (and the
related **CSI-u** encoding) report keys in a single, unambiguous, structured form:

```text
ESC [ <unicode-codepoint> ; <modifiers> u
```

Every key — including ones that legacy encoding couldn't represent — gets a clean
codepoint plus an explicit modifier bitmask, with optional key-release reporting
and no timeout guessing for Escape. An app opts in by writing an enable sequence,
and falls back to legacy parsing if the terminal doesn't respond. It's the future,
but it isn't universal yet, so a framework must still speak the legacy dialect
fluently and upgrade when it can.

---

## Why you want a parser (and why maya is it)

Step back and look at what "read the keyboard" actually entails:

- Put the terminal into raw mode — and restore it on *every* exit path,
  including crashes and signals, or you brick the user's shell.
- Read a stream of bytes that mixes printable UTF-8 runes, control codes, and
  escape sequences, with no framing.
- Reassemble multi-byte UTF-8 characters that may split across reads.
- Run a stateful machine to recognize escape sequences that *also* may split
  across reads.
- Disambiguate a lone Escape from the start of a sequence using timing.
- Optionally enable, decode, and coalesce mouse events.
- Handle bracketed paste so pastes aren't mistaken for typing.
- Detect and prefer modern protocols (Kitty/CSI-u) where available, while still
  supporting the legacy mess everywhere else.
- Normalize all of it across `xterm`, `rxvt`, the Linux console, macOS Terminal,
  Windows console, tmux, and friends.

The output of all that machinery should be *clean, typed events* your app can
`switch` on without thinking about any of the above:

```cpp
// You write this — clear intent, no byte-wrangling.
on(ev, Key::Up,    [&] { cursor.move_up(); });
on(ev, 'q',        [&] { quit(); });
on(ev, Key::Enter, [&] { submit(); });
```

That transformation — from a messy, stateful, terminal-specific byte stream into
tidy `KeyEvent` / `MouseEvent` / `PasteEvent` objects — is exactly what maya's
input layer does for you. You get to think about *what the user meant*, not which
byte the End key happens to send on the third terminal emulator down.

---

## Try it yourself

The best way to believe all this is to watch the bytes with your own eyes. None
of these need any code — just a terminal.

### Watch raw key bytes with `cat -v`

```bash
cat -v
```

Now press keys. Each control/special key prints its bytes. Press the **Up arrow**
and you'll see `^[[A` — that's `ESC [ A`, exactly as promised (`^[` is how `cat -v`
draws the ESC byte). Try F1 (`^[OP`), Page Up (`^[[5~`), Tab (`^I`), and Enter.
Press Ctrl-C to leave (it still works here because `cat` runs in cooked mode).

### Even better: `showkey -a`

On Linux, `showkey -a` shows each key's decimal, octal, and hex value as you
press it — a perfect decoder ring for the tables above:

```bash
showkey -a
```

(It exits on its own after a few seconds of no input.)

### Feel raw mode (and how to escape it)

```bash
stty raw -echo
```

You just disabled canonical mode *and* echo in your live shell. Type something —
you'll see nothing, because echo is off, and the shell won't react to Enter the
way you expect. This is *precisely* the broken state a careless TUI leaves behind.

To recover, type the following **blind** (you won't see it as you type) and press
Enter:

```bash
stty sane
```

!!! tip "If you're ever stuck in a broken terminal"
    `stty sane` and `reset` both rescue a terminal left in raw mode. Worst case,
    just close the window and open a fresh one. Now you know why a framework's
    "restore on exit, no matter what" guarantee matters.

### See mouse bytes

Turn on mouse reporting in your terminal, then read raw bytes:

```bash
printf '\e[?1000h'   # enable basic mouse reporting
printf '\e[?1006h'   # SGR extended coordinates
cat -v               # now click around in the window
```

Click somewhere and you'll see something like `^[[<0;12;5M` — button 0 (left) at
column 12, row 5. Scroll the wheel and watch the button code jump to 64/65. When
you're done, exit `cat` (Ctrl-C) and turn reporting back off:

```bash
printf '\e[?1000l'
printf '\e[?1006l'
```

If your mouse cursor still acts strangely afterward, a quick `reset` clears it.

---

## What's next

You now know how a TUI gets *input* and why even "read a keypress" is a small
engineering project. But there's a mirror-image problem on the output side: once
you know *what* to draw in response to a key, how do you actually paint it to the
screen — fast, flicker-free, and without redrawing the whole world every frame?

That's the subject of the next Foundations page:
[**The Rendering Problem**](the-rendering-problem.md).
