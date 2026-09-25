#!/usr/bin/env python3
"""tests/visible_change.py BIN: of the cells a program re-sends each frame,
how many LOOK different on the terminal it is talking to?

Replays the stream through a grid and, for every glyph cell written, compares
(glyph, fg, bg) with what that cell already showed. A cell re-sent with the
same appearance is pure waste, and at a 256-colour depth two truecolor
shades often become the same index.
"""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
from wire_waste import capture, TOK, sgr_apply

def main(binary, term):
    data, mark, secs = capture(binary, term)
    grid = {}; x = y = 0; pen = (None, None, frozenset())
    sent = same = 0; bytes_in_same = 0; tot = 0
    run_bytes = 0
    for m in TOK.finditer(data):
        s = m.group(0); counting = m.start() >= mark
        if counting: tot += len(s)
        if m.group(2):
            fin = m.group(2); params = m.group(1).decode()
            if fin == b"m": pen = sgr_apply(pen, params); run_bytes += len(s) if counting else 0
            elif fin in (b"H", b"f"):
                p = [int(v) if v else 1 for v in params.split(";")] + [1, 1]
                y, x = p[0] - 1, p[1] - 1
                run_bytes += len(s) if counting else 0
            elif fin == b"C": x += int(params or 1)
            continue
        if s == b"\r": x = 0; continue
        if s == b"\n": y += 1; continue
        if s.startswith(b"\x1b") or s[0] < 0x20: continue
        cell = (s, pen[0], pen[1])
        if counting:
            sent += 1
            if grid.get((x, y)) == cell:
                same += 1; bytes_in_same += len(s) + run_bytes
        run_bytes = 0
        grid[(x, y)] = cell; x += 1
    print(f"{os.path.basename(binary)} TERM={term}: {tot/secs/1024:.0f} KB/s, "
          f"{sent/secs:.0f} cells/s sent, {same*100/max(sent,1):.1f}% of them LOOK unchanged "
          f"(~{bytes_in_same*100/max(tot,1):.1f}% of bytes)")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "tmux-256color")
