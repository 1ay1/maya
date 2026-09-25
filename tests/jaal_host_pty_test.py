#!/usr/bin/env python3
"""tests/jaal_host_pty_test.py — maya on jaal, driven in a REAL terminal.

Unit tests can't catch what this catches. Every bug below was found by this
harness and was invisible to the 67 unit tests jaal already had, because each
one lives in the seam between a real terminal, a real signal and a real
renderer:

  * keystrokes updated the model but never reached the screen: jaal folded a
    host event inside route(), so the next step() reported "no change" and
    the host never redrew (fixed in jaal: route() carries the change)
  * every keystroke drew the PREVIOUS model: maya's renderer defers a frame
    under load and expects its loop to retry within a few ms; jaal's loop
    didn't know (fixed: host.owes_frame() / host.wait_hint())
  * resizes were silently dropped: maya's terminal layer leaves SIGWINCH at
    SIG_IGN, and jaal treated that as "leave ignored" (fixed: an inherited
    SIG_IGN is honoured only for signals that STOP a process)
  * the host had no way to hear about a resize at all (fixed:
    host.on_signal)

It reads the SCREEN, through a terminal emulator (pyte), not the byte stream:
maya draws diffs with cursor moves, so the text on the wire is never the text
on the screen. And it waits for output to go QUIET rather than sleeping a
fixed time, so it tests behaviour instead of timing.

    python3 tests/jaal_host_pty_test.py build-app/maya_counter

Needs: pip install pyte. Exits non-zero on the first failure.
"""
import fcntl
import os
import pty
import re
import select
import signal
import struct
import sys
import termios
import time

try:
    import pyte
except ImportError:
    print("SKIP: needs pyte (pip install pyte)")
    sys.exit(0)

BIN = sys.argv[1] if len(sys.argv) > 1 else "build-app/maya_counter"
failures = 0


def check(cond, what):
    global failures
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        failures += 1


def set_size(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


pid, fd = pty.fork()
if pid == 0:
    os.execv(BIN, [BIN])

set_size(fd, 24, 80)
screen = pyte.Screen(80, 24)
stream = pyte.ByteStream(screen)


def settle(quiet=0.25, cap=3.0):
    """Feed output to the emulator until nothing arrives for `quiet` s."""
    end, last = time.time() + cap, time.time()
    while time.time() < end:
        ready, _, _ = select.select([fd], [], [], 0.02)
        if ready:
            try:
                stream.feed(os.read(fd, 65536))
                last = time.time()
            except OSError:
                return
        elif time.time() - last > quiet:
            return


def field(name):
    for line in screen.display:
        m = re.search(name + r": ?(-?[\dx]+)", line)
        if m:
            return m.group(1)
    return None


def ticks():
    v = field("ticks")
    return int(v) if v is not None else None


settle(0.4)
check(field("count") == "0", "first frame drawn, count 0")
check(field("terminal") == "80x24", "program told its size before first frame")

os.write(fd, b"+")
settle()
check(field("count") == "1", "a single key reaches the screen (route() redraw)")

os.write(fd, b"++")
settle()
check(field("count") == "3", "a burst of keys all land, none dropped")

os.write(fd, b"-")
settle()
check(field("count") == "2", "each key draws ITS model, not the previous one")

# timer: measure the cadence, not a count after a sleep
os.write(fd, b" ")
settle(0.1)
t0, s0 = ticks(), time.time()
settle(quiet=10, cap=2.0)
t1, s1 = ticks(), time.time()
n = (t1 or 0) - (t0 or 0)
period_ms = (s1 - s0) / n * 1000 if n else 0
check(6 <= n <= 9 and 200 <= period_ms <= 300,
      f"Sub::every(250ms) ticks at its period ({n} ticks, {period_ms:.0f} ms each)")

os.write(fd, b" ")
settle()
stopped_at = ticks()
settle(quiet=10, cap=0.8)
check(ticks() == stopped_at, "a Sub that goes away stops (no tick after unsubscribe)")

set_size(fd, 30, 100)
screen.resize(30, 100)
os.kill(pid, signal.SIGWINCH)
settle()
check(field("terminal") == "100x30", "SIGWINCH reaches the program as a resize")

os.write(fd, b"q")
t_end, rc = time.time() + 3, None
while time.time() < t_end:
    w = os.waitpid(pid, os.WNOHANG)
    if w[0] != 0:
        rc = os.waitstatus_to_exitcode(w[1])
        break
    settle(0.05, 0.1)
check(rc == 0, f"'q' quits with exit 0 (got {rc})")
if rc is None:
    os.kill(pid, signal.SIGKILL)

print(f"\njaal_host_pty_test: {'ok' if failures == 0 else f'{failures} FAILED'}")
sys.exit(1 if failures else 0)
