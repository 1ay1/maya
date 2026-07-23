#!/usr/bin/env python3
"""
Generate maya/include/maya/text/unicode_width_table.hpp from pinned UCD data.

Inputs (committed under maya/data/):
  - EastAsianWidth.txt   — official Unicode East_Asian_Width property
  - emoji-data.txt       — official Unicode Emoji properties

Output:
  - maya/include/maya/text/unicode_width_table.hpp
        Two `constexpr std::array<WidthRange, N>` literals:
          kWideRanges                 — Wide + Fullwidth (always 2 cols)
          kEmojiPresentationRanges    — Emoji_Presentation (2 cols on
                                        modern terminals only — gated at
                                        runtime by mode 2027 / heuristic)
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
OUT  = ROOT / "include" / "maya" / "text" / "unicode_width_table.hpp.tmp"

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

    if not eaw_path.exists() or not emoji_path.exists():
        sys.stderr.write(
            f"missing UCD files in {DATA} — drop EastAsianWidth.txt and "
            "emoji-data.txt from https://www.unicode.org/Public/<ver>/ucd/ "
            "and re-run.\n"
        )
        return 1

    wide  = coalesce(parse_ucd(eaw_path,   lambda p: p in ("W", "F")))
    emoji = coalesce(parse_ucd(emoji_path, lambda p: p == "Emoji_Presentation"))

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
//
// Two range tables, both sorted and coalesced for O(log n) binary search:
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

#include <array>
#include <cstdint>

namespace maya::unicode::detail {{

struct WidthRange {{
    char32_t first;
    char32_t last;
}};

{emit('kWideRanges', wide)}

{emit('kEmojiPresentationRanges', emoji)}

}} // namespace maya::unicode::detail
"""

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(body, encoding="utf-8", newline="\n")

    print(f"wrote {OUT.relative_to(ROOT.parent)}: "
          f"{len(wide)} wide ranges, {len(emoji)} emoji-presentation ranges")
    return 0


if __name__ == "__main__":
    sys.exit(main())
