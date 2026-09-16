#!/usr/bin/env python3
"""Generate maya's built-in color schemes from iTerm2-Color-Schemes.

    scripts/gen_themes.py [--all] [-o include/maya/style/schemes.hpp]

Source of truth is mbadolato/iTerm2-Color-Schemes, whose `Xresources/`
variant is the one worth parsing: a scheme there is nineteen values in
`key: #rrggbb` form — foreground, background, cursorColor and color0..15 —
which is exactly a terminal palette and nothing else. The .itermcolors
plists carry the same data as float-triples inside XML, and the shells/
variants are scripts; neither earns its parser.

A scheme gives a PALETTE. maya's Theme wants SEMANTICS. The mapping below
is the whole design decision of this file, so it is stated once, here, and
applied uniformly — a per-scheme hand-tune is how 609 themes become 609
opinions that disagree about what `warning` means.
"""

import argparse
import re
import sys
import urllib.request
from pathlib import Path

RAW = "https://raw.githubusercontent.com/mbadolato/iTerm2-Color-Schemes/master/Xresources/"
API = ("https://api.github.com/repos/mbadolato/iTerm2-Color-Schemes/"
       "contents/Xresources?per_page=1000")

# The schemes worth compiling in by default: the ones people name when they
# name a terminal theme. --all takes the full 609 for anyone who wants them.
CURATED = [
    # The ones people name when they name a terminal theme, in the exact
    # spelling upstream uses — a near-miss here is a silent 404, so the list
    # is checked against the repo rather than written from memory.
    "Dracula", "Nord", "Zenburn", "Argonaut", "Chalk", "Cobalt2",
    "Homebrew", "Ocean", "Oceanic Next", "Seti", "Snazzy",
    "Spacedust", "Spacegray", "Spacegray Eighties",
    "Tomorrow", "Tomorrow Night", "Tomorrow Night Eighties", "Wez",

    "iTerm2 Solarized Dark", "iTerm2 Solarized Light",
    "Solarized Dark Higher Contrast", "Solarized Darcula",

    "Gruvbox Dark", "Gruvbox Dark Hard", "Gruvbox Light",
    "Gruvbox Material Dark", "Gruvbox Material Light",

    "TokyoNight", "TokyoNight Day", "TokyoNight Moon", "TokyoNight Storm",

    "Catppuccin Mocha", "Catppuccin Latte", "Catppuccin Frappe",
    "Catppuccin Macchiato",

    # nightfox family — one of the most-used modern sets, and it spans
    # polarity properly (dayfox/dawnfox are real light schemes, not a dark
    # one inverted).
    "Nightfox", "Duskfox", "Nordfox", "Terafox", "Carbonfox",
    "Dayfox", "Dawnfox",

    "Atom One Dark", "Atom One Light",
    "One Half Dark", "One Half Light",
    "Rose Pine", "Rose Pine Dawn", "Rose Pine Moon",
    "Kanagawa Wave", "Kanagawa Dragon", "Kanagawa Lotus",
    "Everforest Dark Hard", "Everforest Light Med",
    "Ayu", "Ayu Light", "Ayu Mirage",
    "GitHub", "GitHub Dark", "GitHub Dark Dimmed",
    "Material", "Material Dark", "Material Darker", "Material Ocean",
    "Monokai Classic", "Monokai Pro", "Monokai Pro Light",
    "Monokai Remastered", "Monokai Soda",
    "Xcode Dark", "Xcode Light",
    "Flexoki Dark", "Flexoki Light",
    "Zenwritten Dark", "Zenwritten Light",
    "Pencil Dark", "Pencil Light",

    # Modern single-scheme favourites.
    "Vesper", "Poimandres", "Aura", "Andromeda", "Horizon",
    "Night Owl", "Moonfly", "Firewatch", "Hopscotch", "Neutron",
    "Jellybeans", "Misterioso", "Obsidian", "Twilight", "Wombat",
    "Molokai", "Belafonte Night",

    # Light schemes are scarce upstream and the ones that exist are worth
    # carrying, because the whole point of issue #37 is that a light
    # terminal is a first-class case rather than an afterthought.
    "Novel", "Man Page", "Piatto Light", "Breeze",
]

KEY = re.compile(r"^\*(?:\.|\w*\.)?(\w+):\s*#([0-9A-Fa-f]{6})", re.M)


def fetch(url: str) -> str:
    with urllib.request.urlopen(url, timeout=30) as r:
        return r.read().decode("utf-8", "replace")


def parse(text: str) -> dict:
    """Xresources -> {foreground, background, cursorColor, color0..15}."""
    out = {}
    for m in KEY.finditer(text):
        out[m.group(1)] = int(m.group(2), 16)
    return out


def ident(name: str, taken: set[str] | None = None) -> str:
    """A scheme name as a C++ identifier: 'Tokyo Night' -> tokyo_night.

    Upstream names are display strings, and two of them can differ only in
    case: the repo ships both "Dracula" and "dracula". Lowercasing collapses
    those onto one identifier, which is a REDEFINITION — the whole header
    fails to compile, not just the offending pair. With --all that is one
    collision in 615, i.e. exactly the kind of thing that works on the
    curated set and breaks the moment someone asks for everything.

    So uniqueness is enforced rather than assumed: a taken identifier gets a
    numeric suffix. The scheme's DISPLAY name is unaffected — that comes
    from the index table, not from here — so a user still sees "dracula"
    and "Dracula" spelled the way upstream spells them.
    """
    s = re.sub(r"[^0-9A-Za-z]+", "_", name).strip("_").lower()
    if not s or s[0].isdigit():
        s = "s_" + s
    if taken is None:
        return s
    base, n = s, 2
    while s in taken:
        s = f"{base}_{n}"
        n += 1
    taken.add(s)
    return s


def mix(a: int, b: int, t: float) -> int:
    """Blend two 0xRRGGBB colors, t=0 -> a, t=1 -> b."""
    out = 0
    for sh in (16, 8, 0):
        ca, cb = (a >> sh) & 0xFF, (b >> sh) & 0xFF
        out |= int(round(ca + (cb - ca) * t)) << sh
    return out


def luma(c: int) -> float:
    r, g, b = (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF
    return (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0


def rel_luminance(c: int) -> float:
    """WCAG relative luminance — gamma-correct, unlike the simple average."""
    def ch(v: int) -> float:
        s = v / 255.0
        return s / 12.92 if s <= 0.03928 else ((s + 0.055) / 1.055) ** 2.4
    r, g, b = (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF
    return 0.2126 * ch(r) + 0.7152 * ch(g) + 0.0722 * ch(b)


def contrast(a: int, b: int) -> float:
    """WCAG contrast ratio, 1.0 (identical) to 21.0 (black on white)."""
    l1, l2 = rel_luminance(a), rel_luminance(b)
    if l1 < l2:
        l1, l2 = l2, l1
    return (l1 + 0.05) / (l2 + 0.05)


def readable(c: int, bg: int, fg: int, ratio: float = 3.0) -> int:
    """Nudge `c` toward `fg` until it clears `ratio` against `bg`.

    Upstream palettes set color8 ("bright black") for a DARK background and
    a lot of them make it nearly invisible on their own: measured across the
    57 schemes we ship, 35 had a muted tone below 3:1 — the tier agentty uses
    for timestamps, hints, and every piece of chrome that is supposed to
    recede but still be legible. Several were under 2:1, which is decorative
    rather than readable.

    Blending toward the FOREGROUND rather than picking a fixed grey keeps the
    scheme's own hue: Catppuccin's muted stays lilac, Gruvbox's stays warm.
    It also works on a light scheme unchanged, because "away from the
    background" is the invariant that survives polarity.

    3.0 is the floor deliberately, not 4.5. This text is SUPPOSED to be
    quiet — pushing every scheme to body-text contrast would erase the
    distinction between muted and text, which is the whole point of the slot.
    3:1 is the WCAG threshold for large/incidental text and is where a dim
    tone stops being guesswork.
    """
    if contrast(c, bg) >= ratio:
        return c
    best = c
    for i in range(1, 21):
        cand = mix(c, fg, i / 20.0)
        best = cand
        if contrast(cand, bg) >= ratio:
            return cand
    return best


def theme_of(p: dict) -> dict:
    """Palette -> maya's 24 semantic slots.

    The mapping, and why:

      A terminal palette already assigns meaning to six of its sixteen
      slots — red is error, green is success, yellow is warning — because
      that is what every program emitting ANSI has meant by them for forty
      years. Those go straight across.

      What a palette does NOT carry is the chrome: selection, border,
      overlay, the diff tints. Those are DERIVED by blending toward the
      background, so they sit on it rather than fighting it, and so the
      derivation holds for a light scheme and a dark one alike.
    """
    fg, bg = p["foreground"], p["background"]
    c = [p[f"color{i}"] for i in range(16)]
    dark = luma(bg) < 0.5
    # Toward the foreground on a dark scheme, toward it on a light one too:
    # "away from the background" is the invariant that survives polarity.
    def lift(base, t):
        return mix(base, fg, t)

    return {
        "primary":      c[4],   # blue
        "secondary":    c[2],   # green
        "accent":       c[5],   # magenta
        "success":      c[2],
        "error":        c[1],   # red
        "warning":      c[3],   # yellow
        "info":         c[6],   # cyan
        "text":         fg,
        "inverse_text": bg,
        "muted":        readable(c[8], bg, fg),   # bright black, floored
        "surface":      lift(bg, 0.06),
        "background":   bg,
        "border":       lift(bg, 0.22),
        "diff_added":   mix(bg, c[2], 0.22),
        "diff_removed": mix(bg, c[1], 0.22),
        "diff_changed": mix(bg, c[4], 0.18),
        "highlight":    mix(bg, c[4], 0.30),
        "selection":    mix(bg, c[4], 0.24),
        "cursor":       p.get("cursorColor", fg),
        "link":         c[6],
        "placeholder":  lift(bg, 0.34),
        "shadow":       mix(bg, 0x000000, 0.45) if dark else mix(bg, 0x000000, 0.18),
        "overlay":      lift(bg, 0.10),
    }


SLOTS = ["primary", "secondary", "accent", "success", "error", "warning",
         "info", "text", "inverse_text", "muted", "surface", "background",
         "border", "diff_added", "diff_removed", "diff_changed", "highlight",
         "selection", "cursor", "link", "placeholder", "shadow", "overlay"]


def emit(cpp_ident: str, t: dict) -> str:
    """One `inline constexpr Theme <ident> { ... };`.

    Takes the RESOLVED identifier rather than the display name, because
    uniqueness is decided by the caller (see ident()'s `taken` set) and
    re-deriving it here would throw away the disambiguating suffix.
    """
    w = max(len(s) for s in SLOTS)
    body = "\n".join(
        f"    .{s:<{w}} = Color::hex(0x{t[s]:06X}),"
        for s in SLOTS)
    return f"inline constexpr Theme {cpp_ident} {{\n{body}\n}};\n"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--all", action="store_true",
                    help="every scheme in the upstream repo, not the curated set")
    ap.add_argument("-o", "--out", default="include/maya/style/schemes.hpp")
    args = ap.parse_args()

    if args.all:
        import json
        names = [e["name"] for e in json.loads(fetch(API))]
    else:
        names = CURATED

    out, made, failed = [], [], []
    taken: set[str] = set()
    for n in sorted(names):
        try:
            p = parse(fetch(RAW + urllib.parse.quote(n)))
            if "foreground" not in p or "color15" not in p:
                print(f"  skip {n}: incomplete", file=sys.stderr)
                failed.append((n, "incomplete palette"))
                continue
            ident_n = ident(n, taken)
            out.append(emit(ident_n, theme_of(p)))
            made.append((n, ident_n))
            print(f"  {n}", file=sys.stderr)
        except Exception as e:                       # noqa: BLE001
            print(f"  skip {n}: {e}", file=sys.stderr)
            failed.append((n, str(e)))

    # A CURATED name that did not resolve is a BUG, not a warning.
    #
    # These were warnings, and the warning scrolled past: five themes
    # (Oceanic-Next, Panda, SpaceGray, PaperColor Light, Nightfly) had
    # spellings upstream does not use, 404'd, and were silently absent from
    # the shipped header for however long. Nobody noticed because the only
    # evidence was a line in a regeneration log nobody re-reads.
    #
    # `--all` is exempt: it takes whatever the repo has, so an entry
    # disappearing upstream is news rather than an error.
    if not args.all and failed:
        print("\nERROR: curated schemes did not resolve:", file=sys.stderr)
        for n, why in failed:
            print(f"  {n}: {why}", file=sys.stderr)
        print("\nCheck the exact spelling against\n  " + API, file=sys.stderr)
        return 1

    idx = "\n".join(
        f'    {{"{n}", &{i}}},' for n, i in made)

    hdr = f"""#pragma once
// maya::theme — built-in color schemes.
//
// GENERATED by scripts/gen_themes.py from mbadolato/iTerm2-Color-Schemes.
// Do not edit by hand; re-run the script instead.
//
// {len(made)} schemes. Each is a terminal PALETTE mapped onto maya's
// semantic slots by the one rule stated in the generator: the six slots a
// palette already gives meaning to (red is error, green is success, yellow
// is warning) go straight across, and the chrome — border, selection,
// overlay, the diff tints — is derived by blending toward the background,
// so it sits on the scheme rather than fighting it and the derivation holds
// whatever the scheme's polarity.
//
// These are OPT-IN. maya's default is theme::native, which states no colors
// of its own and lets the terminal's palette through; see theme.hpp.

#include "theme.hpp"

namespace maya::theme {{

{"".join(out)}
// Every generated scheme, by its upstream name.
struct NamedTheme {{ const char* name; const Theme* theme; }};

inline constexpr NamedTheme schemes[] = {{
{idx}
}};

}}  // namespace maya::theme
"""
    Path(args.out).write_text(hdr)
    print(f"wrote {args.out}: {len(made)} schemes", file=sys.stderr)
    return 0


if __name__ == "__main__":
    import urllib.parse
    raise SystemExit(main())
