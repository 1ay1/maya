#!/usr/bin/env python3
"""tests/frame_phases.py BIN... : where a frame's time goes, per example.

Runs each binary in a pty (214x60, TERM=tmux-256color), holds a key for 3 s
(60 Hz, like autorepeat), and aggregates maya's own per-frame profiler
(MAYA_FRAME_PROF): build (element -> layout nodes), layout, paint, the
rest of the frame (diff + serialize + write), and node count.

    python3 tests/frame_phases.py build-app/maya_agent_session --key x
"""
import argparse, fcntl, os, pty, re, select, signal, statistics, struct, termios, time

LINE = re.compile(r"rt=([\d.]+) cf=([\d.]+) total=([\d.]+) nodes=(\d+).*?ph\[b=([\d.]+) l=([\d.]+) p=([\d.]+)\]")

def run(binary, key, secs):
    log = f"/tmp/frame_phases_{os.getpid()}_{os.path.basename(binary)}.log"
    if os.path.exists(log): os.remove(log)
    env = {k: v for k, v in os.environ.items() if k not in ("NO_COLOR", "COLORTERM", "MAYA_COLOR")}
    env.update(TERM="tmux-256color", MAYA_FRAME_PROF=log)
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(binary, [binary], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 60, 214, 0, 0))
    os.kill(pid, signal.SIGWINCH)
    def pump(s):
        end = time.time() + s
        while time.time() < end:
            if select.select([fd], [], [], 0.005)[0]:
                try: os.read(fd, 1 << 16)
                except OSError: return
    pump(1.2)
    start = os.path.getsize(log) if os.path.exists(log) else 0
    end = time.time() + secs
    while time.time() < end:
        try: os.write(fd, key.encode())
        except OSError: break
        pump(0.016)
    os.close(fd)
    try: os.kill(pid, 9)
    except ProcessLookupError: pass
    try: os.waitpid(pid, 0)
    except ChildProcessError: pass
    if not os.path.exists(log):
        print(f"{os.path.basename(binary):26s} (no profiler output: not a Program loop?)")
        return
    with open(log) as f:
        f.seek(start); rows = [m.groups() for m in map(LINE.search, f) if m]
    if not rows:
        print(f"{os.path.basename(binary):26s} (no frames)"); return
    col = lambda i: [float(r[i]) for r in rows]
    med = lambda xs: statistics.median(xs)
    b, l, p, tot, rt = col(4), col(5), col(6), col(2), col(0)
    rest = [t - (bb + ll + pp) for t, bb, ll, pp in zip(tot, b, l, p)]
    nodes = int(med([float(r[3]) for r in rows]))
    print(f"{os.path.basename(binary):26s} frames {len(rows):4d}  total med {med(tot):5.2f}ms p90 {sorted(tot)[int(len(tot)*.9)]:5.2f}"
          f"  | build {med(b):5.2f}  layout {med(l):5.2f}  paint {med(p):5.2f}  rest {med(rest):5.2f}  | nodes {nodes}",
          flush=True)

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("binaries", nargs="+")
    ap.add_argument("--key", default="j")
    ap.add_argument("--secs", type=float, default=3.0)
    a = ap.parse_args()
    for b in a.binaries:
        run(b, a.key, a.secs)
