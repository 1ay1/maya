#!/usr/bin/env python3
"""tests/backpressure_bench.py: CPU of a maya program when the TERMINAL is slow.

A real terminal emulator parses and paints every byte, so it drains the pty
far slower than a test harness does. When it falls behind, the tty buffer
fills and the program's writes block (EAGAIN). This reads the pty at a fixed
rate (like a terminal) and reports the PROGRAM's CPU, which is what the user
sees as "100% and laggy".

    python3 tests/backpressure_bench.py build-jaal/maya_doom_fire --rate 2000
"""
import argparse, fcntl, os, pty, select, signal, struct, sys, termios, time

def cpu_seconds(pid):
    out = os.popen(f"ps -o time= -p {pid}").read().strip()
    if not out:
        return None
    parts = out.split(":")
    s = float(parts[-1]); m = int(parts[-2]) if len(parts) > 1 else 0
    h = int(parts[-3]) if len(parts) > 2 else 0
    return h * 3600 + m * 60 + s

def run(binary, rate_kbs, secs, cols, rows):
    env = dict(os.environ, TERM="xterm-256color", MAYA_COLOR="truecolor")
    env.pop("NO_COLOR", None)
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(binary, [binary], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    time.sleep(0.5)
    budget_per_tick = rate_kbs * 1024 / 100 if rate_kbs > 0 else None   # 10 ms ticks
    got = 0
    c0 = cpu_seconds(pid); t0 = time.perf_counter()
    while time.perf_counter() - t0 < secs:
        tick_end = time.perf_counter() + 0.01
        want = int(budget_per_tick) if budget_per_tick else 1 << 20
        while want > 0:
            r, _, _ = select.select([fd], [], [], max(0, tick_end - time.perf_counter()))
            if not r:
                break
            try:
                b = os.read(fd, min(want, 4096))
            except OSError:
                b = b""
            if not b:
                break
            got += len(b); want -= len(b)
        time.sleep(max(0, tick_end - time.perf_counter()))
    c1 = cpu_seconds(pid); el = time.perf_counter() - t0
    try:
        os.write(fd, b"q")
    except OSError:
        pass
    time.sleep(0.3)
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    os.waitpid(pid, 0)
    cpu = (c1 - c0) / el * 100 if c0 is not None and c1 is not None else float("nan")
    label = f"{rate_kbs} KB/s" if rate_kbs > 0 else "unlimited"
    print(f"{os.path.basename(binary):24s} reader {label:>12s}: program cpu {cpu:5.0f}%"
          f"  drained {got/el/1024:8.0f} KB/s", flush=True)

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("binaries", nargs="+")
    ap.add_argument("--rate", type=int, action="append",
                    help="reader KB/s (repeatable); 0 = unlimited")
    ap.add_argument("--secs", type=float, default=3.0)
    ap.add_argument("--cols", type=int, default=160)
    ap.add_argument("--rows", type=int, default=45)
    a = ap.parse_args()
    for b in a.binaries:
        for r in (a.rate or [0, 4000, 1000]):
            run(b, r, a.secs, a.cols, a.rows)
