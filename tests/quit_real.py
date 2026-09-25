#!/usr/bin/env python3
"""tests/quit_real.py BIN [--size COLSxROWS] [--rate KB/s]

Like quit_latency.py, but ANSWERS the terminal queries a real terminal
answers (DSR 5n -> 0n, cursor position 6n -> row;colR, primary DA c,
DECRQM ?Nn$p -> not recognised), so maya's frame-ack flow control and
startup probes run the way they do in a real terminal. Prints a timeline
of what happens after `q`: the time of each query seen and the exit.
"""
import argparse, fcntl, os, pty, re, select, signal, struct, termios, time

ap = argparse.ArgumentParser()
ap.add_argument("binary")
ap.add_argument("--size", default="214x60")
ap.add_argument("--rate", type=int, default=0, help="reader KB/s (0 = unlimited)")
ap.add_argument("--wait", type=float, default=2.0)
a = ap.parse_args()
cols, rows = (int(v) for v in a.size.split("x"))

env = {k: v for k, v in os.environ.items() if k not in ("NO_COLOR", "COLORTERM")}
env["TERM"] = "xterm-256color"
pid, fd = pty.fork()
if pid == 0:
    os.execve(a.binary, [a.binary], env)
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))

QUERY = re.compile(rb"\x1b\[(\d*)n|\x1b\[\?(\d+)\$p|\x1b\[c|\x1b\[>c|\x1b\[\?u")
per_tick = a.rate * 1024 / 200 if a.rate else 1 << 30
tail = b""
t_key = None
log = []

def answer(buf):
    global tail
    data = tail + buf
    for m in QUERY.finditer(data):
        q = m.group(0)
        if m.group(1) is not None:
            rep = b"\x1b[0n" if m.group(1) == b"5" else b"\x1b[1;1R" if m.group(1) == b"6" else b""
        elif m.group(2) is not None:
            rep = b"\x1b[?" + m.group(2) + b";0$y"
        elif q == b"\x1b[c":
            rep = b"\x1b[?62;22c"
        else:
            rep = b""
        if rep:
            os.write(fd, rep)
        if t_key is not None:
            log.append((time.perf_counter() - t_key, q))
    tail = data[-16:]

def pump(until):
    """Read at the given rate until `until`; returns (bytes, exited)."""
    n = 0
    while time.perf_counter() < until:
        tick_end = time.perf_counter() + 0.005
        want = per_tick
        while want > 0:
            r = select.select([fd], [], [], max(0, tick_end - time.perf_counter()))[0]
            if not r:
                break
            try:
                b = os.read(fd, int(min(want, 1 << 16)))
            except OSError:
                return n, True
            if not b:
                return n, True
            n += len(b); want -= len(b)
            answer(b)
        while time.perf_counter() < tick_end:
            time.sleep(0.001)
        if os.waitpid(pid, os.WNOHANG)[0]:
            return n, True
    return n, False

before, _ = pump(time.perf_counter() + a.wait)
t_key = time.perf_counter()
os.write(fd, b"q")
after, exited = pump(t_key + 15)
dt = (time.perf_counter() - t_key) * 1000
if not exited:
    os.kill(pid, signal.SIGKILL)
print(f"{os.path.basename(a.binary):22s} {a.size:>8s} rate {a.rate or 'inf'}: "
      f"exit {dt:7.0f} ms, {after/1024:6.0f} KB after key, {before/a.wait/1024:6.0f} KB/s before"
      + ("" if exited else "  NEVER EXITED"))
counts = {}
for t, q in log:
    counts.setdefault(q, []).append(t)
for q, ts in counts.items():
    print(f"    {q!r:14s} x{len(ts):<4d} first {ts[0]*1000:6.0f} ms  last {ts[-1]*1000:6.0f} ms")
