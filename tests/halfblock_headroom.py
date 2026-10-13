#!/usr/bin/env python3
"""tests/halfblock_headroom.py BIN: what a smarter cell encoder could save.

Replays the program's output through a pen model and, for every glyph cell
written, asks what a pen-aware encoder would have needed:
  * solid cells (half-block with fg == bg, or a space): drawable as ' ' if
    the pen's bg already matches, or '\u2588' if its fg does -- no SGR at all
  * half-block cells whose colours match the pen SWAPPED: drawable as the
    opposite half-block with no SGR
Prints the share of SGR bytes that would disappear.
"""
import os, re, sys
sys.path.insert(0, os.path.dirname(__file__))
from wire_waste import capture, TOK, sgr_apply

UPPER, LOWER, FULL = "\u2580".encode(), "\u2584".encode(), "\u2588".encode()

def main(binary, term):
    data, mark, secs = capture(binary, term)
    pen = (None, None, frozenset())
    pending_sgr = 0                      # bytes of SGR since the last glyph
    pen_before = pen
    tot = sgr_bytes = saved = 0
    cells = solid = swap = 0
    for m in TOK.finditer(data):
        s = m.group(0); counting = m.start() >= mark
        if counting: tot += len(s)
        if m.group(2) == b"m":
            if pending_sgr == 0: pen_before = pen
            pen = sgr_apply(pen, m.group(1).decode())
            if counting: sgr_bytes += len(s); pending_sgr += len(s)
            continue
        if s.startswith(b"\x1b") or s[0] < 0x20:
            continue
        # a glyph cell, drawn with `pen`, after `pending_sgr` bytes of SGR
        if counting:
            cells += 1
            fg, bg, _ = pen
            pfg, pbg, pattrs = pen_before if pending_sgr else pen
            could_skip = False
            if s in (UPPER, LOWER) and fg == bg or s == b" ":
                c = bg if s == b" " else fg
                if c is not None and (pbg == c or pfg == c) and not pattrs:
                    could_skip = True; solid += 1
            elif s in (UPPER, LOWER) and (pfg, pbg) == (bg, fg):
                could_skip = True; swap += 1
            if could_skip and pending_sgr:
                saved += pending_sgr
        pending_sgr = 0
        pen_before = pen
    print(f"{os.path.basename(binary)} TERM={term}: {tot/secs/1024:.0f} KB/s, SGR {sgr_bytes*100/tot:.1f}%")
    print(f"  glyph cells {cells}: solid-and-pen-matches {solid*100/cells:.1f}%, swap-matches {swap*100/cells:.1f}%")
    print(f"  SGR bytes avoidable: {saved*100/tot:.1f}% of the stream")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "tmux-256color")
