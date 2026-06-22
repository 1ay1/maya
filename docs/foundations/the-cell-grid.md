# The Cell Grid

In the previous page we established the foundational truth of terminal
programming: **a terminal is a grid of cells**. Not a canvas of pixels, not a
stream of words — a fixed grid, like graph paper, where every square holds one
character.

That single idea sounds simple enough to dismiss. It is not. Almost every hard
bug you will ever hit in a TUI traces back to one question that turns out to be
shockingly deep:

> *How many cells does this piece of text actually take up?*

You would think the answer is "count the characters." It is not. The answer
involves Unicode, East-Asian typography, decades-old terminal conventions,
emoji families holding hands, and the fact that two terminals looking at the
exact same bytes will frequently disagree.

This page is about the grid in detail — what one cell really holds, why
terminals demand monospace fonts, and the genuinely difficult problem of
**character width**. Get this right and your tables line up, your borders
close, and your layout engine is trustworthy. Get it wrong and everything looks
*almost* aligned, which is somehow worse than obviously broken.

---

## The grid, recapped

A terminal is `rows × columns` of cells. A typical window might be 80 columns
wide and 24 rows tall — the classic "80×24" you'll see referenced everywhere,
inherited from the punch-card and VT100 era.

```text
        col 1   2   3   4   5   6   7   8  ...  80
       ┌───┬───┬───┬───┬───┬───┬───┬───┬─────────┐
row 1  │ H │ e │ l │ l │ o │   │ W │ o │   ...   │
       ├───┼───┼───┼───┼───┼───┼───┼───┼─────────┤
row 2  │   │   │   │   │   │   │   │   │   ...   │
       ├───┼───┼───┼───┼───┼───┼───┼───┼─────────┤
 ...   │                                         │
       ├───┼───┼───┼───┼───┼───┼───┼───┼─────────┤
row 24 │ $ │ _ │   │   │   │   │   │   │   ...   │
       └───┴───┴───┴───┴───┴───┴───┴───┴─────────┘
```

Somewhere on this grid lives the **cursor**: the position where the next
character you print will land. After it prints, the cursor advances one cell to
the right (and wraps to the next row at the edge). The cursor is the terminal's
"you are here" marker, and a huge amount of TUI work is just moving it around
and stamping cells.

### A note on coordinates: 1-based vs 0-based

This trips up everyone, so let's be explicit. There are two coordinate systems
in play, and they disagree on where counting starts:

- **ANSI escape codes are 1-based.** The top-left cell is row `1`, column `1`.
  When you send the terminal "move the cursor to row 5, column 10," the numbers
  `5` and `10` count from one. (We'll cover these escape codes on the next
  page.)
- **Your code is almost always 0-based.** A framework storing the grid in an
  array will put the top-left cell at index `[0][0]`, because that's how arrays
  work in C, C++, Rust, and friends.

So a framework constantly translates: an internal cell at `(col=0, row=0)`
becomes the escape sequence targeting `(1, 1)`.

!!! warning "Off-by-one is the house specialty"
    The 1-based/0-based seam is the single most common source of "everything is
    shifted one column to the left" bugs. When your output is offset by exactly
    one cell, suspect this conversion first. A good framework hides it entirely
    so you only ever think in one system — but when you're debugging raw escape
    codes by hand, keep both pictures in your head.

Also note the convention is usually **`(row, col)` in ANSI** but frequently
**`(x, y)` = `(col, row)` in graphics-flavored APIs**. Two values, two possible
orderings, two possible bases. Read your framework's docs and pick a mental
model.

---

## What one cell holds

A cell is not just "a letter." It is a small bundle of state. At minimum, a
single cell stores:

| Field | Example | Notes |
|-------|---------|-------|
| **Glyph** | `A`, `你`, `🦀` | The visible character (one *grapheme* — more on that word soon) |
| **Foreground color** | bright green | Color of the text itself |
| **Background color** | dark grey | Color behind the text |
| **Attributes** | bold, italic, underline, reverse, blink, strikethrough… | Style flags, often bit-packed |

So the letter in a single cell might really be "a bold, underlined, bright-green
`A` on a dark-grey background." All of that is the state of *one square* on the
grid.

### The packed cell

A terminal grid is big. An 80×24 screen is 1,920 cells; a fullscreen 4K
terminal can be hundreds of thousands. A framework redraws this grid many times
per second, so each cell needs to be **compact** and cheap to copy. The usual
trick is a *packed cell*: a small, fixed-size struct that holds everything about
one square.

Here's a generic sketch (not any specific framework, just to make it concrete):

```cpp
#include <cstdint>

// Attribute flags, one bit each, OR'd together.
enum Attr : uint16_t {
    None      = 0,
    Bold      = 1 << 0,
    Dim       = 1 << 1,
    Italic    = 1 << 2,
    Underline = 1 << 3,
    Blink     = 1 << 4,
    Reverse   = 1 << 5,   // swap fg/bg
    Strike    = 1 << 6,
    // ... room for more
};

struct Cell {
    char32_t glyph;   // ONE grapheme as a Unicode scalar (UTF-32)
    uint32_t fg;      // 0xRRGGBB foreground
    uint32_t bg;      // 0xRRGGBB background
    uint16_t attrs;   // bitmask of Attr flags
    uint8_t  width;   // how many columns this cell occupies: 0, 1, or 2
};
```

Notice that last field: `width`. Even at the level of a single cell, we have to
record how many columns it occupies — because, as we're about to see, **not
every glyph is one column wide.** That field is the entire reason this page
exists.

!!! note "Why `char32_t` and not `char`?"
    A `char` is one byte and can only hold ASCII. Real text is Unicode, and a
    single visible character can be a code point far above 127 (like `你`,
    U+4F60). Storing the glyph as a 32-bit Unicode scalar lets one cell hold any
    single code point. (Even this isn't quite enough for the gnarliest emoji —
    we'll get there.)

---

## Why terminals are monospace

Open a word processor and type `iiii` then `mmmm`. The `m`s take up far more
horizontal space than the `i`s, because the font is **proportional** — each
glyph is as wide as it needs to be. That's beautiful for prose and a disaster
for a grid.

Terminals assume a **monospace** (fixed-width) font: every character occupies
exactly the same cell box. `i` and `m` and `W` all get one identical cell.

```text
Proportional (a word processor):       Monospace (a terminal):

 i i i i                                ┌─┬─┬─┬─┐
 m m m m                                │i│i│i│i│
 ↑ wildly different widths              ├─┼─┼─┼─┤
                                        │m│m│m│m│
                                        └─┴─┴─┴─┘
                                        ↑ identical cells
```

This is not an aesthetic choice — it's structural. The entire grid model
*depends* on a glyph fitting predictably into a cell. If glyph widths varied per
character, the columns would no longer line up and the concept of "column 40"
would be meaningless.

!!! warning "Proportional fonts break the grid"
    If you (or your user) configure the terminal with a proportional font,
    columns drift out of alignment, box-drawing characters develop gaps, and
    ASCII art turns to mush. This is why "pick a monospace font" is rule one of
    terminal setup, and why fonts ship in dedicated coding variants (Fira
    **Code**, JetBrains **Mono**, Cascadia **Mono**). The word *Mono* in the
    name is a promise about cell width.

So far so good: monospace gives every character one equal cell. Except — and
here's where the floor drops out — **some characters legitimately need two
cells, and some need zero.** Monospace fixes the *box* size; it does not make
every character occupy exactly one box.

---

## The width problem

This is the heart of the page. The question "how wide is this text?" has no
single easy answer, because different characters claim different numbers of
cells. Let's build it up case by case.

### Case 1: ASCII — one cell

The easy case. Every printable ASCII character (`A`–`Z`, `a`–`z`, `0`–`9`,
punctuation, space) is exactly **one cell wide**. For decades this was the whole
story, and a lot of old code still assumes "one byte = one character = one
cell." That assumption is wrong the moment you leave ASCII.

### Case 2: East-Asian wide characters — two cells

Chinese, Japanese, and Korean (collectively **CJK**) characters are visually
*square* — about as tall as they are wide. To keep them readable, terminals
render them at **two cells wide**.

```text
"Hi你好" laid out on the grid:

┌───┬───┬───────┬───────┐
│ H │ i │   你  │   好  │
└───┴───┴───────┴───────┘
  1   1     2       2      = 6 cells, but only 4 "characters"
```

So `你好` ("hello" in Chinese) is **two characters but four cells**. The Unicode
standard formalizes this with the **East Asian Width** property: characters are
classed as `Wide`, `Fullwidth`, `Narrow`, `Halfwidth`, `Ambiguous`, or
`Neutral`. `Wide` and `Fullwidth` characters take two cells; most others take
one.

That word **`Ambiguous`** is a foreshadowing of pain. Hold that thought.

### Case 3: Combining marks — zero cells

Some code points don't occupy any cell of their own — they *modify* the
preceding character. The classic example is the accented `é`. There are two ways
to encode it:

| Form | Code points | Bytes (UTF-8) | Cells |
|------|-------------|---------------|-------|
| Precomposed `é` | U+00E9 | 2 | 1 |
| Decomposed `é` | U+0065 (`e`) + U+0301 (combining acute) | 3 | 1 |

In the decomposed form, the combining accent U+0301 is **zero cells wide**: it
stacks onto the `e` rather than taking its own square. Visually both forms look
identical — a single `é` in a single cell — but one is *one* code point and the
other is *two*.

```text
"e" + "◌́"  renders as  "é"   ← two code points, ZERO extra cells, one square
```

Combining marks are everywhere: accents, diacritics, the marks that build
Vietnamese, Hindi (Devanagari), Arabic, Thai. A naive "count the code points"
width calculation overcounts every one of them.

### Case 4: Emoji — two cells, and then it gets weird

Emoji are typically rendered **two cells wide**, like CJK characters (they're
square and pictorial). `🦀` takes two cells. Fine. But emoji are where Unicode's
composition machinery goes into overdrive.

**Skin-tone modifiers.** A waving hand `👋` (U+1F44B) can be followed by a skin
tone modifier `🏽` (U+1F3FD) to produce `👋🏽`. That's **two code points**
combining into **one visible glyph**, still two cells wide.

**ZWJ sequences.** The real boss fight. A *Zero-Width Joiner* (ZWJ, U+200D) glues
emoji together into a single grapheme. The family emoji `👨‍👩‍👧‍👦` is built from:

```text
👨  (man)        U+1F468
ZWJ             U+200D     ← zero-width joiner, invisible
👩  (woman)      U+1F469
ZWJ             U+200D
👧  (girl)       U+1F467
ZWJ             U+200D
👦  (boy)        U+1F466
```

That is **seven code points** (`👨` ZWJ `👩` ZWJ `👧` ZWJ `👦`), 25 bytes in
UTF-8, forming **one** visible emoji that *should* occupy **two cells** — if the
terminal understands the sequence. Many older terminals don't, and render it as
four separate two-cell emoji jammed together, blowing your layout to bits.

!!! tip "One grapheme, many code points"
    The family emoji is the perfect illustration of the gap between a
    *grapheme* (one user-perceived character) and a *code point* (one Unicode
    scalar). Seven code points, one grapheme. Your width logic has to reason
    about graphemes, not code points — but terminals vary in how well they
    actually do this.

### The four lengths of a string

Here is the punchline of the whole page. Any non-trivial string has **four
different "lengths,"** and they routinely disagree:

1. **Bytes** — how many bytes it occupies in memory (depends on encoding;
   UTF-8 uses 1–4 bytes per code point).
2. **Code points** — how many Unicode scalars it contains.
3. **Grapheme clusters** — how many *user-perceived characters* it has (what a
   human would call "the letters").
4. **Cells** — how many columns it occupies on the terminal grid. **This is the
   one layout cares about.**

Let's measure one carefully chosen string against all four. The string is:

```text
A你é👨‍👩‍👧‍👦
```

That's: an ASCII `A`, the CJK character `你`, a decomposed `é` (`e` +
combining acute), and the family emoji.

| Piece | Bytes (UTF-8) | Code points | Graphemes | Cells |
|-------|:-------------:|:-----------:|:---------:|:-----:|
| `A` | 1 | 1 | 1 | 1 |
| `你` | 3 | 1 | 1 | 2 |
| `é` (`e`+◌́) | 3 | 2 | 1 | 1 |
| `👨‍👩‍👧‍👦` | 25 | 7 | 1 | 2 |
| **Total** | **32** | **11** | **4** | **6** |

Read that bottom row again. The **same string** is simultaneously:

- **32 bytes** long,
- **11 code points** long,
- **4 graphemes** long,
- and occupies **6 cells** on screen.

Four numbers, all correct, all different. If you ask `strlen()` you get bytes.
If you ask most languages' `.length` you get code points (or worse, UTF-16
units). Neither one tells you how many columns to reserve. **Only the cell count
does** — and computing it requires understanding grapheme clustering *and* the
width of each cluster.

```text
The string A你é👨‍👩‍👧‍👦 on the grid (6 cells):

┌───┬───────┬───┬───────┐
│ A │   你  │ é │  👨‍👩‍👧‍👦   │
└───┴───────┴───┴───────┘
  1     2     1     2
```

### `wcwidth`, East Asian Width, and why terminals disagree

How does a program figure out a character's cell width? The traditional tool is
the C function **`wcwidth()`** (and `wcswidth()` for strings). Given a wide
character, it returns:

- `0` for combining/zero-width characters,
- `1` for normal characters,
- `2` for wide (CJK / fullwidth) characters,
- `-1` for control characters and non-printables.

`wcwidth` consults a table derived from Unicode's **East Asian Width** property.
In principle, every terminal and every program could share one table and always
agree. In practice, they emphatically do not, for two big reasons:

**1. The `Ambiguous` category.** Remember that East Asian Width class? Some
characters — certain box-drawing glyphs, Greek letters, and others — are marked
`Ambiguous`: one cell wide in a Western context, two cells in an East-Asian
context. Terminals pick a side, and they pick differently. A character that's
one cell in your terminal might be two cells in your user's.

**2. Emoji width, the eternal argument.** Are emoji one cell or two? Unicode's
guidance and real terminals have shifted over time. Modern terminals usually
render emoji at two cells; older or minimalist ones render many of them at one
cell. Worse, *whether a terminal even recognizes a ZWJ sequence as a single
grapheme* varies wildly. So the literal same bytes (`👨‍👩‍👧‍👦`) might be:

| Terminal behavior | Reported width |
|-------------------|:--------------:|
| Modern, ZWJ-aware | 2 cells |
| Older, counts each emoji | 8 cells (4 × 2) |
| Minimal, narrow emoji | 4 cells (4 × 1) |

!!! warning "This is a real, daily source of misaligned UIs"
    When a terminal and your program disagree by even one cell on one
    character, every cell *after* it on that row is shifted. Borders don't
    close. Table columns stagger. A status bar's right-aligned clock drifts left
    by one. The text is fine; the *width bookkeeping* desynced. There is no
    perfect, universal fix — the best frameworks ship their own well-tested
    width tables, follow Unicode's recommendations closely, and (crucially) use
    the *same* table for measuring and for rendering so at least they're
    internally consistent.

---

## Why this matters for layout

Pull it all together. A TUI framework's whole job is placing text on the grid so
that columns line up. To do that it must answer "how wide is this?" in **cells**
— never bytes, never code points.

Consider drawing a simple bordered table. The framework decides a column is 10
cells wide, then pads each entry to fill it:

```text
Measured in BYTES (wrong):              Measured in CELLS (right):

┌──────────┬──────────┐                 ┌──────────┬──────────┐
│ Alice    │ 你好世界  │                │ Alice    │ 你好世界 │
│ Bob      │ hello    │                 │ Bob      │ hello    │
└──────────┴──────────┘                 └──────────┴──────────┘
            ↑ "你好世界" is 12 bytes,                ↑ "你好世界" is 8 cells,
              padded to 10 bytes → too                 padded to 10 cells → lines
              short, border misaligns                  up perfectly
```

If the framework measures `你好世界` as 12 (its byte length) it pads to the
wrong amount and the right border jumps out of alignment. If it measures it as 4
(its code-point/grapheme count) it pads two cells too far. Only measuring it as
**8 cells** produces a table that closes cleanly.

Everything alignment-related rides on this:

- **Tables** — column widths, padding, truncation.
- **Borders and boxes** — the corners only meet if every row is exactly the
  same cell width.
- **Centering and right-alignment** — "center in 40 cells" needs the text's
  true cell width.
- **Truncation with `…`** — cutting a string to fit *N* cells must not slice a
  wide character in half or strand a combining mark.
- **Word wrapping** — deciding where a line breaks depends on accumulated cell
  width, not character count.

!!! tip "What good frameworks do for you"
    A solid TUI framework (maya included) measures text in cells internally, so
    you can hand it `"你好"` or `"👋🏽"` and trust that layout, padding, and
    borders still line up. You think in terms of *widgets and content*; the
    framework sweats the grapheme clustering and width tables underneath. That's
    a lot of the value — turning the four-lengths nightmare into "just print
    text and it works."

---

## Try it yourself

You don't need a framework to *see* the width problem. A plain terminal is
enough. The gap between "how many bytes" and "how wide on screen" becomes
visible immediately.

**Bytes vs. visible width.** Print two CJK characters and count their bytes:

```bash
echo -n "你好" | wc -c
# 6      ← six BYTES (each CJK char is 3 bytes in UTF-8)

echo -n "你好" | wc -m
# 2      ← two CHARACTERS (code points)

# ...but on screen, 你好 occupies 4 CELLS. No standard
# coreutil reports that — cell width is a terminal concept.
```

Three different numbers for the same two characters: **6 bytes, 2 code points, 4
cells.**

**Watch alignment break.** Print rows where some entries are ASCII and some are
CJK or emoji, padding each to the same *character* count:

```bash
printf '|%-6s|\n' "abc"
printf '|%-6s|\n' "你好"
printf '|%-6s|\n' "👋🦀"
```

You'll likely see something like this — the right-hand `|` does **not** line up,
because `printf`'s `%-6s` pads to a fixed number of *bytes-ish units*, not
cells:

```text
|abc   |
|你好    |
|👋🦀    |
↑ borders fail to align — the classic symptom
```

The ASCII row is correct. The CJK and emoji rows are off, because each wide
glyph eats two cells but `printf` only "spent" one unit of padding budget on it.
This is the exact bug a width-aware layout engine exists to prevent.

**Provoke the ambiguity.** Print a ZWJ emoji and stare at how *your* terminal
handles it:

```bash
printf '[👨‍👩‍👧‍👦]\n'
```

If your terminal is modern and ZWJ-aware, you'll see one family emoji snug
between the brackets. On an older terminal you may see four separate people, or
gaps, or boxes — the same bytes, rendered at a different width. Try it in two
different terminal emulators and you may get two different pictures. That
divergence, in one screenshot, is the whole reason text width is hard.

!!! note "The takeaway"
    A terminal is a grid of cells. Each cell holds one grapheme plus color and
    attributes. The deep problem is that **the number of cells a string occupies
    is not its byte length, not its code-point count, and not even its grapheme
    count** — it's a fourth measurement that depends on Unicode width rules and
    on terminal behavior that isn't fully standardized. Measure in cells, use
    one consistent width table, and your UIs line up.

---

## What's next

We now know *what* lives in each cell and *how much room* text really takes.
The next question is: how do we actually tell the terminal to move the cursor,
set those colors, flip those attribute bits, and stamp cells where we want them?

The answer is a tiny, cryptic language the terminal has spoken since the 1970s.

Head to **[ANSI Escape Codes](ansi-escape-codes.md)** to learn the control
sequences that drive the grid — moving the cursor, painting color, and clearing
the screen.
