#!/usr/bin/env python3
"""tests/screen_after.py BIN "keys..." [--size 100x30] [--mouse]

Run BIN in a pty, wait for it to draw, send each key (escape names
<up> <down> <left> <right> <pgup> <pgdn> <home> <end> <wheelup> <wheeldown>
<tab> <esc> are understood), wait for the screen to settle, print the
screen, then quit with q. For checking a demo reacts the way it should
to keys a plain --keys string can't carry.
"""
import argparse, fcntl, os, pty, re, select, struct, termios, time
import pyte

NAMES = {"up": "\x1b[A", "down": "\x1b[B", "right": "\x1b[C", "left": "\x1b[D",
         "pgup": "\x1b[5~", "pgdn": "\x1b[6~", "home": "\x1b[H", "end": "\x1b[F",
         "tab": "\t", "esc": "\x1b", "enter": "\r",
         "wheelup": "\x1b[<64;10;10M", "wheeldown": "\x1b[<65;10;10M"}

ap = argparse.ArgumentParser()
ap.add_argument("binary")
ap.add_argument("keys", nargs="?", default="")
ap.add_argument("--size", default="100x30")
a = ap.parse_args()
cols, rows = (int(v) for v in a.size.split("x"))

env = {k: v for k, v in os.environ.items() if k not in ("NO_COLOR", "CLICOLOR")}
env["TERM"] = "xterm-256color"
pid, fd = pty.fork()
if pid == 0:
    os.execve(a.binary, [a.binary], env)
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
screen = pyte.Screen(cols, rows); stream = pyte.ByteStream(screen)

def settle(quiet=0.25, cap=3.0):
    end = time.time() + cap; last = time.time()
    while time.time() < end and time.time() - last < quiet:
        if select.select([fd], [], [], 0.02)[0]:
            try: b = os.read(fd, 1 << 20)
            except OSError: return
            if b"\x1b[5n" in b: os.write(fd, b"\x1b[0n")
            stream.feed(b); last = time.time()

settle(0.5)
for tok in re.findall(r"<(\w+)>|(.)", a.keys, re.S):
    os.write(fd, (NAMES[tok[0]] if tok[0] else tok[1]).encode())
    settle(0.1, 0.5)
settle()
print("\n".join(line.rstrip() for line in screen.display).rstrip())
os.write(fd, b"q"); settle(0.2, 1)
try: os.kill(pid, 9)
except ProcessLookupError: pass
