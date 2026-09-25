#!/usr/bin/env python3
"""tests/snapshot.py BIN out.png ["keys"] [--size 100x30] [--wait 1.0]

Run BIN in a pty, send keys (same <name> syntax as screen_after.py), wait,
and save what's on screen as a PNG: each cell drawn as a 1x2 pixel block
in its colours (a half block ▀ gives fg on top, bg below; any other glyph
shows its fg over bg as a half-and-half cell). For looking at pixel demos
with no terminal.
"""
import argparse, fcntl, os, pty, re, select, struct, termios, time, zlib
import pyte

NAMES = {"up": "\x1b[A", "down": "\x1b[B", "right": "\x1b[C", "left": "\x1b[D",
         "pgup": "\x1b[5~", "pgdn": "\x1b[6~", "home": "\x1b[H", "end": "\x1b[F",
         "tab": "\t", "esc": "\x1b", "enter": "\r"}

ap = argparse.ArgumentParser()
ap.add_argument("binary"); ap.add_argument("out"); ap.add_argument("keys", nargs="?", default="")
ap.add_argument("--size", default="100x30"); ap.add_argument("--wait", type=float, default=1.0)
a = ap.parse_args()
cols, rows = (int(v) for v in a.size.split("x"))
env = {k: v for k, v in os.environ.items() if k not in ("NO_COLOR", "CLICOLOR")}
env["TERM"] = "xterm-256color"; env["COLORTERM"] = "truecolor"; env["MAYA_COLOR"] = "truecolor"
pid, fd = pty.fork()
if pid == 0:
    os.execve(a.binary, [a.binary], env)
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
screen = pyte.Screen(cols, rows); stream = pyte.ByteStream(screen)

def pump(t):
    end = time.time() + t
    while time.time() < end:
        if select.select([fd], [], [], 0.02)[0]:
            try: b = os.read(fd, 1 << 20)
            except OSError: return
            if b"\x1b[5n" in b: os.write(fd, b"\x1b[0n")
            stream.feed(b)

pump(0.6)
for tok in re.findall(r"<(\w+)>|(.)", a.keys, re.S):
    os.write(fd, (NAMES[tok[0]] if tok[0] else tok[1]).encode()); pump(0.15)
pump(a.wait)

def rgb(c, default):
    if c == "default": return default
    if re.fullmatch(r"[0-9a-fA-F]{6}", c): return tuple(int(c[i:i+2], 16) for i in (0, 2, 4))
    named = {"black": (0,0,0), "red": (205,0,0), "green": (0,205,0), "brown": (205,205,0), "blue": (0,0,238),
             "magenta": (205,0,205), "cyan": (0,205,205), "white": (229,229,229)}
    return named.get(c, default)

W, H = cols, rows * 2
px = [[(0, 0, 0)] * W for _ in range(H)]
for y in range(rows):
    line = screen.buffer[y]
    for x in range(cols):
        ch = line[x]
        fg, bg = rgb(ch.fg, (200, 200, 200)), rgb(ch.bg, (0, 0, 0))
        if ch.reverse: fg, bg = bg, fg
        if ch.data == "▀": top, bot = fg, bg
        elif ch.data == "▄": top, bot = bg, fg
        elif ch.data in (" ", ""): top = bot = bg
        elif ch.data == "█": top = bot = fg
        else: top, bot = fg, bg   # text: fg over bg, legible enough
        px[2 * y][x], px[2 * y + 1][x] = top, bot

def png(path, px, scale=4):
    h, w = len(px) * scale, len(px[0]) * scale
    raw = b"".join(b"\x00" + b"".join(bytes(px[y // scale][x // scale]) for x in range(w)) for y in range(h))
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                          + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))

png(a.out, px)
os.write(fd, b"q"); pump(0.3)
try: os.kill(pid, 9)
except ProcessLookupError: pass
print(a.out)
