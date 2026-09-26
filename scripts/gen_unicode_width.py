#!/usr/bin/env python3
"""
Generate maya/include/maya/text/unicode_width_table.hpp from pinned UCD data.

Inputs (committed under maya/data/):
  - EastAsianWidth.txt   — official Unicode East_Asian_Width property
  - emoji-data.txt       — official Unicode Emoji properties
  - UnicodeData.txt      — official Unicode general categories

Output:
  - maya/include/maya/text/unicode_width_table.hpp
        Three `constexpr std::array<WidthRange, N>` literals:
          kWideRanges                 — Wide + Fullwidth (always 2 cols)
          kEmojiPresentationRanges    — Emoji_Presentation (2 cols on
                                        modern terminals only — gated at
                                        runtime by mode 2027 / heuristic)
          kZeroWidthRanges            — combining marks + format controls
                                        (0 cols: they compose onto the
                                        preceding base character)
        Ranges are sorted, non-overlapping, and coalesced (adjacent
        ranges merged) so the runtime binary search has the smallest
        possible N.

Run from the repo root:
    python maya/scripts/gen_unicode_width.py

The generated header is checked in; this script only needs to run when
bumping to a new Unicode revision (drop newer .txt files into maya/data/
and re-run).
"""

from __future__ import annotations

import pathlib
import re
import sys
from typing import Callable, Iterable

ROOT = pathlib.Path(__file__).resolve().parent.parent  # repo / maya
DATA = ROOT / "data"
# The real header. This said ".hpp.tmp" for a while, which meant every run
# quietly wrote a scratch file next to the header and left the committed one
# untouched — the generator looked like it worked and changed nothing.
OUT  = ROOT / "include" / "maya" / "text" / "unicode_width_table.hpp"

# UCD line: "0023" or "1F300..1F5FF" then ';' then property then '#' comment
LINE_RE = re.compile(r"^([0-9A-Fa-f]+)(?:\.\.([0-9A-Fa-f]+))?\s*;\s*(\w+)")


def parse_ucd(path: pathlib.Path, want: Callable[[str], bool]) -> Iterable[tuple[int, int]]:
    """Yield (first, last) codepoint ranges whose property matches `want`."""
    with path.open(encoding="utf-8") as fh:
        for raw in fh:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            m = LINE_RE.match(line)
            if not m:
                continue
            first = int(m.group(1), 16)
            last  = int(m.group(2), 16) if m.group(2) else first
            if want(m.group(3)):
                yield (first, last)


def parse_categories(path: pathlib.Path):
    """Yield (codepoint, general_category) for every assigned code point.

    UnicodeData.txt encodes large blocks as a `<…, First>` / `<…, Last>`
    row pair sharing one category rather than listing each member. Those
    have to be expanded or whole scripts go missing — and the ranges that
    use this form include CJK and Hangul.
    """
    pending_first = None
    with path.open(encoding="utf-8") as fh:
        for raw in fh:
            f = raw.rstrip("\n").split(";")
            if len(f) < 3:
                continue
            cp, name, cat = int(f[0], 16), f[1], f[2]
            if name.endswith(", First>"):
                pending_first = (cp, cat)
                continue
            if name.endswith(", Last>"):
                if pending_first is not None:
                    first, fcat = pending_first
                    for c in range(first, cp + 1):
                        yield (c, fcat)
                    pending_first = None
                continue
            yield (cp, cat)


def coalesce(ranges: Iterable[tuple[int, int]]) -> list[tuple[int, int]]:
    """Sort and merge overlapping / adjacent ranges."""
    out: list[tuple[int, int]] = []
    for first, last in sorted(ranges):
        if out and first <= out[-1][1] + 1:
            out[-1] = (out[-1][0], max(last, out[-1][1]))
        else:
            out.append((first, last))
    return out


def emit(name: str, ranges: list[tuple[int, int]]) -> str:
    lines = [f"inline constexpr std::array<WidthRange, {len(ranges)}> {name} {{{{"]
    for first, last in ranges:
        lines.append(f"    {{0x{first:04X}, 0x{last:04X}}},")
    lines.append("}};")
    return "\n".join(lines)


def header_meta(path: pathlib.Path) -> str:
    """First non-empty header line from a UCD file (carries date + version)."""
    with path.open(encoding="utf-8") as fh:
        for raw in fh:
            stripped = raw.strip()
            if stripped.startswith("#") and stripped != "#":
                return stripped.lstrip("# ").strip()
    return "(unknown)"


def main() -> int:
    eaw_path   = DATA / "EastAsianWidth.txt"
    emoji_path = DATA / "emoji-data.txt"
    ud_path    = DATA / "UnicodeData.txt"

    if not eaw_path.exists() or not emoji_path.exists() or not ud_path.exists():
        sys.stderr.write(
            f"missing UCD files in {DATA} — drop EastAsianWidth.txt, "
            "emoji-data.txt and UnicodeData.txt from "
            "https://www.unicode.org/Public/<ver>/ucd/ and re-run.\n"
        )
        return 1

    wide  = coalesce(parse_ucd(eaw_path,   lambda p: p in ("W", "F")))
    emoji = coalesce(parse_ucd(emoji_path, lambda p: p == "Emoji_Presentation"))

    # Zero-width: everything that composes onto a preceding base character
    # rather than occupying a cell of its own.
    #
    #   Mn  non-spacing mark   — accents, Arabic/Hebrew points, Indic
    #                            matras, Hangul conjoining jamo
    #   Me  enclosing mark     — combining circles/squares
    #   Cf  format control     — ZWSP/ZWNJ/ZWJ, bidi controls, BOM
    #
    # This list used to be written by hand in unicode_width.hpp and covered
    # only Latin, Cyrillic, Hebrew and Arabic. Everything it missed measured
    # one column too wide per mark: Hangul conjoining jamo (agentty#55 — an
    # IME-decomposed Korean syllable came out 4 columns instead of 2), and
    # the same bug for Thai, Devanagari, Bengali, Tamil and the rest.
    #
    # Two deliberate exclusions, both of which a terminal DOES advance for:
    #   * U+0000..U+001F, U+007F..U+009F — Cc controls. is_control() handles
    #     them; they are not text and must never reach a width query.
    #   * U+00AD SOFT HYPHEN — Cf, but every terminal prints it as a cell.
    zero_cats = {"Mn", "Me", "Cf"}
    zero_cps = {
        cp for cp, cat in parse_categories(ud_path)
        if cat in zero_cats and cp != 0x00AD and not (cp < 0x20 or 0x7F <= cp <= 0x9F)
    }

    # Hangul conjoining jamo are the one thing the general category can't
    # tell us. They are Lo (a letter), not Mn — but a JUNGSEONG (vowel) and
    # a JONGSEONG (final) compose ONTO the preceding CHOSEONG to form one
    # syllable block, so they advance the cursor by nothing. That's
    # Hangul_Syllable_Type V and T; UAX #11 and every terminal treat them as
    # zero-width, and glibc's wcwidth returns 0 for the whole span. Only the
    # leading CHOSEONG (U+1100..U+115F, EAW=W) takes the two columns.
    #
    # Without this, typing Korean through an IME — which delivers the
    # decomposed form, e.g. 한 as U+1112 U+1161 U+11AB — measured one
    # syllable as 4 columns instead of 2 (agentty#55).
    zero_cps |= set(range(0x1160, 0x1200))    # Jamo    V + T
    zero_cps |= set(range(0xD7B0, 0xD800))    # Jamo Ext-B (V + T)

    # The Arabic number-sign family (U+0600..U+0605, U+06DD, U+070F, U+0890,
    # U+0891, U+08E2) is Cf, but these PREFIX a following digit sequence and
    # terminals advance a cell for them. glibc agrees. Excluding them keeps
    # the common case right; they are not combining marks in any real sense.
    zero_cps -= {0x0600, 0x0601, 0x0602, 0x0603, 0x0604, 0x0605,
                 0x06DD, 0x070F, 0x0890, 0x0891, 0x08E2}

    zero = coalesce((cp, cp) for cp in zero_cps)

    eaw_meta   = header_meta(eaw_path)
    emoji_meta = header_meta(emoji_path)

    body = f"""\
#pragma once
// AUTO-GENERATED — DO NOT EDIT BY HAND.
// Regenerate with: python maya/scripts/gen_unicode_width.py
//
// Source files (pinned under maya/data/):
//   EastAsianWidth.txt — {eaw_meta}
//   emoji-data.txt     — {emoji_meta}
//   UnicodeData.txt    — general categories (Mn/Me/Cf → zero width)
//
// Three range tables, all sorted and coalesced for O(log n) binary search:
//
//   kWideRanges
//     Codepoints with East_Asian_Width = Wide or Fullwidth. These are the
//     "always 2 columns" code points — every TUI library and terminal
//     agrees on these (CJK ideographs, Hangul syllables, fullwidth Latin,
//     etc.). Used unconditionally.
//
//   kEmojiPresentationRanges
//     Codepoints with Emoji_Presentation = Yes. These are the "2 columns
//     on modern terminals" code points (⚡ U+26A1, ✅ U+2705, the entire
//     1F300..1FAFF emoji blocks, regional indicators, …). Modern
//     terminals (Windows Terminal, Kitty, iTerm 3.5+, WezTerm,
//     Alacritty, Ghostty, vte 0.62+) render them as 2 cells; legacy
//     emulators may render them as 1. The runtime gates this table on
//     a DECRQM ?2027$p probe (mode 2027 — Grapheme Cluster Wide-
//     Character) plus an env-var heuristic; see
//     maya::ansi::env_supports_synchronized_output() and
//     maya::Runtime::supports_grapheme_clusters() for the gate.
//
//   kZeroWidthRanges
//     Codepoints with general category Mn (non-spacing mark), Me
//     (enclosing mark) or Cf (format control). They compose onto the
//     preceding base character and occupy no columns of their own:
//     accents, Arabic/Hebrew points, Indic matras, Hangul conjoining
//     jamo, ZWSP/ZWNJ/ZWJ, bidi controls. Excludes the Cc controls
//     (is_control()'s job) and U+00AD SOFT HYPHEN, which terminals do
//     advance for.

#include <array>
#include <cstdint>

namespace maya::unicode::detail {{

struct WidthRange {{
    char32_t first;
    char32_t last;
}};

{emit('kWideRanges', wide)}

{emit('kEmojiPresentationRanges', emoji)}

{emit('kZeroWidthRanges', zero)}

}} // namespace maya::unicode::detail
"""

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(body, encoding="utf-8", newline="\n")

    print(f"wrote {OUT.relative_to(ROOT.parent)}: "
          f"{len(wide)} wide ranges, {len(emoji)} emoji-presentation ranges, "
          f"{len(zero)} zero-width ranges")
    return 0


if __name__ == "__main__":
    sys.exit(main())
