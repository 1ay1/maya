#!/usr/bin/env python3
"""tests/wire_golden.py BIN [BIN2] — the exact bytes a program writes for a
fixed, deterministic session (fixed size, fixed keys, answers to DSR), with
timing-dependent parts normalised. With two binaries, diffs them: a refactor
that claims "no behaviour change" should print IDENTICAL.

Only meaningful for programs without clocks or randomness in their output
(counter, markup, scroll_* , editor_*, proc_table...)."""
import os, pty, select, sys, time, fcntl, termios, struct, hashlib, re

def session(binary, keys, cols=100, rows=30, inline=False):
    env = {k: v for k, v in os.environ.items() if k not in ("NO_COLOR", "CLICOLOR", "COLORTERM")}
    env["TERM"] = "xterm-256color"
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(binary, [binary], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    out = bytearray()
    def settle(quiet=0.3, cap=4.0):
        end = time.time() + cap; last = time.time()
        while time.time() < end and time.time() - last < quiet:
            if select.select([fd], [], [], 0.02)[0]:
                try: b = os.read(fd, 1 << 20)
                except OSError: return
                out.extend(b)
                for _ in range(b.count(b"\x1b[5n")): os.write(fd, b"\x1b[0n")
                if b"\x1b[6n" in b: os.write(fd, b"\x1b[1;1R")
                last = time.time()
    settle(0.6)
    for k in keys:
        os.write(fd, k); settle()
    os.write(fd, b"q"); settle(0.3, 2)
    try: os.kill(pid, 9)
    except ProcessLookupError: pass
    # normalise: DSR probes appear per frame (count depends on ack timing)
    data = bytes(out).replace(b"\x1b[5n", b"")
    return data

KEYS = [b"j", b"j", b"+", b"\x1b[B", b"\x1b[B", b"k", b"-", b" "]

a = session(sys.argv[1], KEYS)
if len(sys.argv) == 2:
    print(len(a), hashlib.sha1(a).hexdigest())
else:
    b = session(sys.argv[2], KEYS)
    if a == b:
        print(f"IDENTICAL ({len(a)} bytes)")
    else:
        i = next(i for i in range(min(len(a), len(b))) if a[i] != b[i]) if any(x != y for x, y in zip(a, b)) else min(len(a), len(b))
        print(f"DIFFER: {len(a)} vs {len(b)} bytes, first difference at byte {i}")
        print("  a:", a[max(0, i-40):i+40])
        print("  b:", b[max(0, i-40):i+40])
        sys.exit(1)
