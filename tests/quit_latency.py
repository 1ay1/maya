#!/usr/bin/env python3
"""tests/quit_latency.py BIN... : how long after 'q' does the program exit?

Answers the terminal's queries (DSR, cursor position, DA) like a real
terminal, then reads each binary's output in a pty at a FIXED rate
(like a terminal over ssh: default 1000 KB/s), so a high-throughput
animation keeps the tty buffer full, the way it is for a real user. After
2 s it presses q and measures:
  exit   ms until the process exits
  bytes  bytes the terminal still had to swallow after the keypress
         (what the user watches scroll by before the screen clears)

    python3 tests/quit_latency.py build-app/maya_doom_fire build-app/maya_doom_fire
"""
import argparse, fcntl, os, pty, select, signal, struct, termios, time

def run(binary, rate_kbs, key, cols=214, rows=60):
    env = {k: v for k, v in os.environ.items() if k not in ("NO_COLOR", "COLORTERM")}
    env["TERM"] = "xterm-256color"
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(binary, [binary], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    os.kill(pid, signal.SIGWINCH)
    per_tick = rate_kbs * 1024 / 200 if rate_kbs else 1 << 30   # 5 ms ticks
    def pump(until, count):
        n = 0
        while time.perf_counter() < until:
            tick_end = time.perf_counter() + 0.005
            want = per_tick
            while want > 0 and select.select([fd], [], [], max(0, tick_end - time.perf_counter()))[0]:
                try: d = os.read(fd, int(min(want, 65536)))
                except OSError: return n, True
                if not d: return n, True
                # Answer the terminal queries a real terminal answers, so the
                # frame-ack flow control is exercised (an unanswered DSR makes
                # maya fall back to its no-ack timeout: a different code path).
                for _ in range(d.count(b"\x1b[5n")): os.write(fd, b"\x1b[0n")
                if b"\x1b[6n" in d: os.write(fd, b"\x1b[1;1R")
                if b"\x1b[c"  in d: os.write(fd, b"\x1b[?62;22c")
                n += len(d); want -= len(d)
            time.sleep(max(0, tick_end - time.perf_counter()))
            if count and os.waitpid(pid, os.WNOHANG)[0] == pid: return n, True
        return n, False
    pump(time.perf_counter() + 2.0, False)
    t0 = time.perf_counter()
    os.write(fd, key)
    after, done = pump(t0 + 10.0, True)
    t_exit = time.perf_counter() - t0
    if not done:
        os.kill(pid, 9)
    try: os.waitpid(pid, 0)
    except ChildProcessError: pass
    os.close(fd)
    return (t_exit * 1000 if done else None), after

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("binaries", nargs="+")
    ap.add_argument("--rate", type=int, action="append", help="reader KB/s; 0 = unlimited")
    ap.add_argument("--key", default="q")
    ap.add_argument("--size", default="214x60", help="COLSxROWS")
    a = ap.parse_args()
    cols, rows = (int(v) for v in a.size.split("x"))
    for b in a.binaries:
        for r in (a.rate or [0, 1000, 300]):
            ms, after = run(b, r, a.key.encode(), cols, rows)
            label = f"{r} KB/s" if r else "unlimited"
            print(f"{os.path.basename(b):24s} reader {label:>10s}: exit after "
                  f"{('%6.0f ms' % ms) if ms is not None else ' >10 s'}   {after/1024:7.0f} KB still to draw",
                  flush=True)
