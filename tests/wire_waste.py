#!/usr/bin/env python3
"""tests/wire_waste.py BIN [TERM]: how much of a program's output is wasted.

Replays the byte stream through a tiny terminal model (cursor, SGR fg/bg/attrs,
a cell grid) and counts:
  * no-op SGR   : an SGR that leaves the pen exactly as it was
  * no-op cells : a cell written with the same glyph + colours already there
  * cup         : cursor-position bytes
so we know which optimisation actually pays on a slow link.
"""
import fcntl, os, pty, re, select, signal, struct, sys, termios, time

def capture(binary, term, cols=214, rows=60, secs=2.5, skip=1.0):
    env = {k: v for k, v in os.environ.items()
           if k not in ("NO_COLOR", "COLORTERM", "MAYA_COLOR", "CLICOLOR", "CLICOLOR_FORCE")}
    env["TERM"] = term
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(binary, [binary], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    os.kill(pid, signal.SIGWINCH)
    buf = bytearray(); t0 = time.time(); mark = None
    while time.time() - t0 < secs + skip:
        if mark is None and time.time() - t0 >= skip:
            mark = len(buf)
        if select.select([fd], [], [], 0.05)[0]:
            try: buf += os.read(fd, 1 << 16)
            except OSError: break
    os.close(fd); os.kill(pid, 9)
    try: os.waitpid(pid, 0)
    except ChildProcessError: pass
    return bytes(buf), mark or 0, secs

TOK = re.compile(rb"\x1b\[([0-9;:?]*)([A-Za-z])|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|\x1b.|[\x00-\x1f]|[\xc0-\xff][\x80-\xbf]*|[\x20-\x7f]")

def sgr_apply(pen, params):
    ps = [int(x) if x else 0 for x in params.replace(":", ";").split(";")] if params else [0]
    fg, bg, attrs = pen
    i = 0
    while i < len(ps):
        p = ps[i]
        if p == 0: fg, bg, attrs = None, None, frozenset()
        elif p in (38, 48) and i + 1 < len(ps):
            if ps[i + 1] == 5 and i + 2 < len(ps): v = ("i", ps[i + 2]); i += 2
            elif ps[i + 1] == 2 and i + 4 < len(ps): v = ("rgb",) + tuple(ps[i + 2:i + 5]); i += 4
            else: v = None
            if p == 38: fg = v
            else: bg = v
            i += 1
        elif 30 <= p <= 37 or 90 <= p <= 97: fg = ("n", p)
        elif 40 <= p <= 47 or 100 <= p <= 107: bg = ("n", p)
        elif p == 39: fg = None
        elif p == 49: bg = None
        else: attrs = attrs | {p}
        i += 1
    return (fg, bg, attrs)

def analyse(data, mark, cols, rows):
    grid = {}; x = y = 0; pen = (None, None, frozenset())
    counting = False; pos = 0
    tot = sgr_noop = sgr_all = cup = cells = cells_noop = 0
    for m in TOK.finditer(data):
        s = m.group(0); n = len(s)
        if not counting and m.start() >= mark: counting = True
        if counting: tot += n
        if m.group(2):
            params, fin = m.group(1).decode(), m.group(2)
            if fin == b"m":
                new = sgr_apply(pen, params)
                if counting:
                    sgr_all += n
                    if new == pen: sgr_noop += n
                pen = new
            elif fin in (b"H", b"f"):
                p = [int(v) if v else 1 for v in params.split(";")] + [1, 1]
                y, x = p[0] - 1, p[1] - 1
                if counting: cup += n
            elif fin == b"C":
                x += int(params or 1)
            elif fin == b"K":
                for cx in range(x, cols): grid[(cx, y)] = (" ", pen[1])
        elif s == b"\r": x = 0
        elif s == b"\n": y = min(rows - 1, y + 1)
        elif s[0] >= 0x20 and not s.startswith(b"\x1b"):
            cell = (s, pen[0], pen[1], pen[2])
            if counting:
                cells += n
                if grid.get((x, y)) == cell: cells_noop += n
            grid[(x, y)] = cell; x += 1
    return tot, sgr_all, sgr_noop, cup, cells, cells_noop

if __name__ == "__main__":
    b = sys.argv[1]; term = sys.argv[2] if len(sys.argv) > 2 else "tmux-256color"
    data, mark, secs = capture(b, term)
    tot, sgr, sgr_noop, cup, cells, cells_noop = analyse(data, mark, 214, 60)
    pct = lambda v: f"{v * 100 / tot:5.1f}%" if tot else "  -  "
    print(f"{os.path.basename(b)} TERM={term}: {tot / secs / 1024:.0f} KB/s")
    print(f"  SGR        {pct(sgr)}   of which no-op {pct(sgr_noop)}")
    print(f"  cursor pos {pct(cup)}")
    print(f"  glyphs     {pct(cells)}   of which unchanged cells {pct(cells_noop)}")
