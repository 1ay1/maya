# Glossary

Every term used across this manual, in one place. New to TUIs? Skim this once;
it makes the rest of the docs click.

## Terminals & the stack

**Terminal**
: Historically a physical device (keyboard + screen) for talking to a computer.
Today, the *abstraction* of a fixed grid of character cells you read from and
write to as a byte stream.

**Terminal emulator**
: The application that draws the grid and shuttles bytes — iTerm2, Alacritty,
kitty, WezTerm, GNOME Terminal, Windows Terminal, Ghostty, Zed's integrated
terminal. It interprets escape codes and turns keystrokes into bytes.

**Shell**
: A *program running inside* the terminal (bash, zsh, fish, PowerShell). Not the
same thing as the terminal emulator — the emulator is the window; the shell is
one program drawn in it.

**TTY**
: "Teletype." The kernel device representing a terminal. `isatty(fd)` asks "is
this file descriptor a real terminal?" — which is why piping to a file behaves
differently from running interactively.

**PTY (pseudo-terminal)**
: A software TTY pair (master/slave) used when there's no physical terminal —
how terminal emulators, `ssh`, `tmux`, and test harnesses give a program a
terminal to talk to.

**`$TERM` / `$COLORTERM` / `$TERM_PROGRAM`**
: Environment variables a terminal sets to advertise its identity and
capabilities (terminal type, truecolor support, which emulator). Frameworks
read them to decide what features and color depth to use.

## The grid & text

**Cell**
: One slot in the grid. Holds one *grapheme* plus a foreground color, background
color, and attributes (bold, underline, …). maya stores each cell as a packed
64-bit value.

**Glyph**
: The visible character drawn in a cell.

**Grapheme cluster**
: What a human perceives as "one character," which may be several Unicode
codepoints (e.g. `é` = `e` + combining accent; `👨‍👩‍👧‍👦` = several codepoints joined
by zero-width joiners). The unit that occupies cells.

**Codepoint**
: A single Unicode scalar value. A grapheme can be many codepoints; a codepoint
can be many bytes (UTF-8). Bytes ≠ codepoints ≠ graphemes ≠ cells — four
different "lengths" of a string.

**Cell width (`wcwidth`)**
: How many columns a grapheme occupies: ASCII = 1, East-Asian wide (CJK) and
most emoji = 2, combining/zero-width = 0. Getting this wrong misaligns
everything; maya measures text in cells.

**Monospace font**
: A font where every cell is the same width — the assumption that makes the grid
model work.

## Escape codes & output

**ANSI escape code / control sequence**
: A run of bytes, starting with the `ESC` byte (`0x1B`), that the terminal
*interprets* (move cursor, set color, clear) instead of printing.

**CSI (Control Sequence Introducer)**
: The common form `ESC [ params final-byte`, e.g. `ESC[2J` (clear screen),
`ESC[1;31m` (bold red).

**SGR (Select Graphic Rendition)**
: The `ESC[…m` family that sets text attributes and colors.

**16 / 256 / truecolor**
: The three color tiers — 16 named colors, a 256-color palette
(`ESC[38;5;Nm`), and 24-bit RGB (`ESC[38;2;R;G;Bm`). maya downgrades truecolor →
256 → 16 when the terminal can't do better.

**Alternate screen buffer**
: A separate full-screen canvas (`ESC[?1049h`) that full-screen apps switch to,
so they don't clobber — and restore on exit — your shell's scrollback.

**Synchronized output (DEC 2026)**
: `ESC[?2026h`/`l` — tells the terminal "buffer this frame and paint it all at
once," eliminating tearing on terminals that support it.

**Scrollback**
: The history of lines that have scrolled off the top of the viewport. Inline
rendering must take great care not to corrupt it.

## Input

**Cooked (canonical) mode**
: Default tty mode: the kernel line-buffers input, echoes it, and handles
backspace/Ctrl-C — your program gets a whole line at <Enter>.

**Raw mode**
: tty mode where every keystroke is delivered immediately, unbuffered, no echo.
What a TUI needs. Enabling it is a *promise* to restore cooked mode on exit.

**Bracketed paste**
: A mode (`ESC[?2004h`) where the terminal wraps pasted text in markers so apps
can distinguish pasting from typing.

## Rendering

**Double buffering**
: Keeping the previous frame and the new frame in memory at once.

**Diffing**
: Comparing the two buffers and emitting escape codes only for the cells that
changed — the core technique that makes TUIs fast and flicker-free.

**Bytes-on-wire**
: The number of bytes written to the terminal per frame — the real performance
metric, especially over SSH. maya's diff minimizes it.

**Tearing**
: A visible half-drawn frame, caused by the terminal painting before a full
update arrives. Fixed by synchronized output.

**Frame budget**
: The time available per frame (~16ms for 60fps). Diffing + SIMD keep maya under
it.

**SIMD**
: "Single instruction, multiple data" — CPU instructions that process many
values at once. maya uses SIMD to diff many cells per instruction.

## maya concepts

**Element**
: A node in the UI tree — a box, text, or widget, with style and layout
properties. The thing you build with the DSL.

**DSL (domain-specific language)**
: maya's declarative, *compile-time* builder syntax (`v(...)`, `t<"...">`,
`pad<1>`, `| border(...)`) for constructing Element trees.

**Type-state**
: A technique where an object's *type* encodes its build stage, so invalid
operations (border color before border style, negative padding) fail to
compile. The basis of "impossible states don't compile."

**Yoga / flexbox**
: The layout engine maya uses (the same one behind React Native): rows, columns,
`grow`/`shrink`, alignment, gaps, padding.

**Canvas**
: maya's in-memory `width × height` grid of packed cells — the painted frame
before it's diffed and sent.

**Signal**
: A reactive state container; reading it in a view creates a dependency, and
updating it triggers a re-render of the dependents.

**Event**
: A parsed, typed input: `KeyEvent`, `MouseEvent`, `PasteEvent`, resize — what
maya hands your code after decoding the raw input byte stream.

**Program (MVU)**
: The Elm-style Model–View–Update architecture for larger apps: pure
`init`/`update`/`view` functions, with side effects expressed as commands.

**Full-screen mode**
: Rendering on the alternate screen — the app owns the whole window.

**Inline mode**
: Rendering a live frame at the cursor, below your prompt, growing/shrinking in
place without disturbing scrollback.

**Widget**
: A ready-made, composable Element — charts, tables, scroll views, markdown,
sparklines, agent-UI components, and more.

**Witness chain**
: maya's type-theoretic technique for guaranteeing inline-mode scrollback
integrity — see [the internals chapter](internals/witness-chain.md).

**Compatibility repaint**
: A renderer mode (auto-enabled on terminals like Zed) that redraws each
*changed* row in full to avoid mid-row cursor moves the terminal mis-tracks —
trading a few extra bytes for correctness.
