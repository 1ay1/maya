#!/usr/bin/env python3
"""tests/jaal_smoke.py — the checks EVERY maya-on-jaal program must pass.

jaal_host_pty_test.py is a deep test of one program. This is the wide one:
it knows nothing about a program's model or keys, and checks only what any
terminal app owes its user, so it runs unchanged against every example as it
migrates:

  1. it draws a first frame           (starts, takes the terminal, renders)
  2. input changes the screen         (a key reaches update() AND the screen)
  3. it survives a resize             (SIGWINCH -> relayout, no crash)
  4. it stays responsive when idle    (no busy loop: CPU near zero)
  5. it quits, with exit 0            (via its quit key, else Ctrl+C)
  6. it gives the terminal back       (alt screen left, cursor shown)

Point 2 is the one that matters most and the one unit tests can't see: the
"keystroke updates the model but never reaches the screen" bug lived exactly
there. Point 4 catches an idle loop that spins (a wait timeout of zero, a
Sub that re-arms every step). Point 6 is maya's RAII terminal type-state,
which must still run under jaal's teardown.

    python3 tests/jaal_smoke.py <binary> [--keys "+-"] [--quit q]

Reads the SCREEN through a terminal emulator (pyte) and waits for output to
go quiet rather than sleeping fixed times. Needs: pip install pyte.
"""
import argparse
import fcntl
import os
import pty
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

ap = argparse.ArgumentParser()
ap.add_argument("binary")
ap.add_argument("--keys", default="+-jk ",
                help="keys to try, one at a time, until the screen changes")
ap.add_argument("--quit", default="q", help="the program's quit key")
ap.add_argument("--no-input-change", action="store_true",
                help="the program doesn't react to keys (display-only)")
ap.add_argument("--min-frames", type=int, default=8,
                help="distinct screens required in 1.2 s for --animates (default 8). "
                     "A slow mover (snake steps every few frames) animates at fewer; "
                     "a frozen one shows 1")
ap.add_argument("--idle-cpu", type=float, default=0.10,
                help="max share of a core while idle (default 0.10). An animation that "
                     "renders every frame by design (a ray tracer) costs more than that "
                     "without spinning; a spin is ~1.0")
ap.add_argument("--animates", metavar="KEY", default=None,
                help="press KEY, then send NOTHING: the screen must keep "
                     "changing (an animation must run without input)")
args = ap.parse_args()

name = os.path.basename(args.binary)
failures = []


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        failures.append(what)


def set_size(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


pid, fd = pty.fork()
if pid == 0:
    # The agent's shell (and some CI) exports NO_COLOR, which maya honours:
    # a colour-only animation would then have nothing to animate. Pin a
    # tier so the checks see what a real terminal sees.
    env = {k: v for k, v in os.environ.items() if k not in ("NO_COLOR", "CLICOLOR")}
    env.setdefault("MAYA_COLOR", "256")
    os.execve(args.binary, [args.binary], env)

ROWS, COLS = 24, 80
set_size(fd, ROWS, COLS)
screen = pyte.Screen(COLS, ROWS)
stream = pyte.ByteStream(screen)
raw = bytearray()          # everything written, for the terminal-restore check


def settle(quiet=0.3, cap=3.0):
    end, last = time.time() + cap, time.time()
    while time.time() < end:
        ready, _, _ = select.select([fd], [], [], 0.02)
        if ready:
            try:
                data = os.read(fd, 65536)
            except OSError:
                return
            if not data:
                return
            raw.extend(data)
            stream.feed(data)
            last = time.time()
        elif time.time() - last > quiet:
            return


def text():
    return "\n".join(screen.display)


def alive():
    return os.waitpid(pid, os.WNOHANG)[0] == 0


print(f"{name}:")

# 1. first frame
settle(0.5)
first = text()
check(first.strip() != "", "draws a first frame")

# 2. input changes the screen
if args.no_input_change:
    print("  skip  input changes the screen (--no-input-change)")
else:
    changed = False
    for k in args.keys:
        before = text()
        os.write(fd, k.encode())
        settle()
        if text() != before:
            changed = True
            break
    check(changed, f"a key reaches the screen (tried {args.keys!r})")

# 2b. animation runs with no input. A widget that animates asks for its next
#     frame during view(); the HOST must schedule it. If it doesn't, the
#     animation draws one frame and freezes until the next keypress — the bug
#     class maya's loop documents at length. Press one key, then send
#     nothing, and count distinct screens. (Calibrated: with the host's frame
#     scheduling removed, motion_showcase shows 1 frame; with it, 22.)
if args.animates:
    import hashlib
    os.write(fd, args.animates.encode())
    seen, end = set(), time.time() + 1.2
    # Hash glyphs AND colours: a colour-only animation (a fire, a fluid, a
    # gradient of half-blocks) never changes screen.display's text, and
    # hashing text alone reported it as frozen - for maya's own loop too.
    def look():
        rows = []
        for y in range(screen.lines):
            row = screen.buffer[y]
            rows.append("".join(f"{c.data}{c.fg}{c.bg}" for c in row.values()))
        return "\n".join(rows)
    while time.time() < end:
        settle(0.02, 0.04)
        seen.add(hashlib.md5(look().encode()).hexdigest())
    check(len(seen) >= args.min_frames,
          f"animates with no input ({len(seen)} distinct frames in 1.2s after {args.animates!r})")

# 3. resize
set_size(fd, 30, 100)
screen.resize(30, 100)
os.kill(pid, signal.SIGWINCH)
settle()
check(alive(), "survives a resize")

# 4. idle CPU: a spinning loop burns a whole core. Measured from the
#    child's own thread times via `ps -o cputime`, sampled across a quiet
#    window. (An earlier version trusted a coarse %cpu reading; a program
#    folding 5M messages/s showed as 12% and passed. Checked against a known
#    spinner: see the note at the end of this file.)
def child_cpu():
    out = os.popen(f"ps -o cputime= -p {pid}").read().strip()
    if not out:
        return None
    secs = 0.0
    for part in out.replace("-", ":").split(":"):
        secs = secs * 60 + float(part)
    return secs

settle(0.3)
c0, w0 = child_cpu(), time.time()
time.sleep(2.0)
settle(0.1, 0.2)
c1, w1 = child_cpu(), time.time()
if c0 is not None and c1 is not None:
    share = (c1 - c0) / (w1 - w0)
    check(share < args.idle_cpu,
          f"idle, it doesn't spin ({share*100:.1f}% of a core over {w1-w0:.1f}s)")
else:
    print("  skip  idle CPU (couldn't read it)")

# 5. quit
os.write(fd, args.quit.encode())
deadline, rc = time.time() + 3, None
while time.time() < deadline:
    w = os.waitpid(pid, os.WNOHANG)
    if w[0] != 0:
        rc = os.waitstatus_to_exitcode(w[1])
        break
    settle(0.05, 0.1)
if rc is None:                      # its quit key didn't work: try Ctrl+C
    os.write(fd, b"\x03")
    deadline = time.time() + 3
    while time.time() < deadline:
        w = os.waitpid(pid, os.WNOHANG)
        if w[0] != 0:
            rc = os.waitstatus_to_exitcode(w[1])
            break
        settle(0.05, 0.1)
check(rc is not None, f"quits (exit {rc})")
check(rc == 0 or rc == 130, f"exits cleanly (0, or 130 for Ctrl+C) — got {rc}")
if rc is None:
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)

# 6. the terminal is given back: whatever it switched on, it switched off.
#    Alt screen (?1049) and a hidden cursor (?25l) are the two that leave a
#    user's shell unusable.
tail = bytes(raw)
if b"\x1b[?1049h" in tail:
    check(tail.rfind(b"\x1b[?1049l") > tail.rfind(b"\x1b[?1049h"),
          "leaves the alt screen on exit")
if b"\x1b[?25l" in tail:
    check(tail.rfind(b"\x1b[?25h") > tail.rfind(b"\x1b[?25l"),
          "shows the cursor again on exit")

print(f"{name}: {'ok' if not failures else f'{len(failures)} FAILED'}")
sys.exit(1 if failures else 0)

# Calibration (keep this true): a program whose update() returns
# Cmd::send(...) forever must FAIL check 4. Before jaal 765efd6 such a program
# really did idle at ~11% — the loop slept a rounded-up millisecond after
# every step — so it passed; the harness caught THAT, and now catches the
# spin itself at ~100%.
