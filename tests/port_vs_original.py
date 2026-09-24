#!/usr/bin/env python3
"""tests/port_vs_original.py: every jaal port against the maya original.

Same terminal for both (TERM, size, no COLORTERM unless given), measured:
  idle   CPU % over 2 s with no input (animation ports keep animating)
  key    median / p90 ms from a keypress to the first byte of the redraw
  KB/key bytes written per keypress (what an ssh link has to carry)
  busy   CPU % while keys arrive every 16 ms (holding a key down)

    python3 tests/port_vs_original.py build-jaal [--term tmux-256color]
"""
import argparse, fcntl, os, pty, select, signal, statistics, struct, subprocess, sys, termios, time

KEYS = {  # a harmless key each program reacts to (defaults to 'j')
    "counter": "+", "stopwatch": " ", "agent": "t", "agent_session": "x",
    "messenger": "x", "motion_showcase": "1", "editor_live": "j",
}

def cpu(pid):
    t = subprocess.run(["ps", "-o", "time=", "-p", str(pid)], capture_output=True,
                       text=True, timeout=2).stdout.strip()
    if not t:
        return None
    return sum(float(x) * 60 ** i for i, x in enumerate(reversed(t.split(":"))))

class Child:
    def __init__(self, binary, term, cols, rows):
        env = {k: v for k, v in os.environ.items()
               if k not in ("NO_COLOR", "COLORTERM", "MAYA_COLOR", "CLICOLOR", "CLICOLOR_FORCE")}
        env["TERM"] = term
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execve(binary, [binary], env)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        os.kill(self.pid, signal.SIGWINCH)

    def read_for(self, secs):
        n = 0; end = time.perf_counter() + secs
        while True:
            left = end - time.perf_counter()
            if left <= 0: return n
            if select.select([self.fd], [], [], left)[0]:
                try: d = os.read(self.fd, 1 << 16)
                except OSError: return n
                if not d: return n
                n += len(d)

    def quiet(self, gap=0.08, cap=1.5):
        total = 0; end = time.perf_counter() + cap
        while time.perf_counter() < end:
            n = self.read_for(gap)
            if n == 0: break
            total += n
        return total

    def close(self):
        try: os.close(self.fd)
        except OSError: pass
        try: os.kill(self.pid, signal.SIGKILL)
        except ProcessLookupError: pass
        for _ in range(40):
            try:
                if os.waitpid(self.pid, os.WNOHANG)[0] == self.pid: return
            except ChildProcessError: return
            time.sleep(0.05)

def measure(binary, key, term, cols, rows):
    c = Child(binary, term, cols, rows)
    try:
        c.read_for(1.2); c.quiet()
        # idle
        c0 = cpu(c.pid); t0 = time.perf_counter(); c.read_for(2.0)
        c1 = cpu(c.pid); idle = (c1 - c0) / (time.perf_counter() - t0) * 100
        # key latency
        lat = []; sizes = []
        for _ in range(10):
            t = time.perf_counter(); os.write(c.fd, key.encode())
            if select.select([c.fd], [], [], 1.0)[0]:
                lat.append((time.perf_counter() - t) * 1000)
                sizes.append(c.quiet(0.05, 1.0))
            else:
                lat.append(None)
            c.read_for(0.03)
        # busy: a held key (60 Hz autorepeat) for 2 s
        c0 = cpu(c.pid); t0 = time.perf_counter()
        while time.perf_counter() - t0 < 2.0:
            os.write(c.fd, key.encode()); c.read_for(0.016)
        c1 = cpu(c.pid); busy = (c1 - c0) / (time.perf_counter() - t0) * 100
        ok = sorted(x for x in lat if x is not None)
        return dict(idle=idle, busy=busy,
                    med=statistics.median(ok) if ok else None,
                    p90=ok[max(0, int(len(ok) * 0.9) - 1)] if ok else None,
                    miss=len(lat) - len(ok),
                    kb=statistics.median(sizes) / 1024 if sizes else 0.0)
    finally:
        c.close()

def fmt(m):
    f = lambda v, w=5: f"{v:{w}.1f}" if v is not None else " " * (w - 1) + "-"
    return f"idle{f(m['idle'])}% busy{f(m['busy'])}% key{f(m['med'])}/{f(m['p90'])}ms {m['kb']:6.1f}KB"

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("build")
    ap.add_argument("--term", default="tmux-256color")
    ap.add_argument("--cols", type=int, default=214)
    ap.add_argument("--rows", type=int, default=60)
    ap.add_argument("--only", nargs="*")
    a = ap.parse_args()
    ports = sorted(f[len("maya_jaal_"):] for f in os.listdir(a.build)
                   if f.startswith("maya_jaal_") and os.path.exists(os.path.join(a.build, "maya_" + f[len("maya_jaal_"):])))
    if a.only: ports = [p for p in ports if p in a.only]
    print(f"TERM={a.term} {a.cols}x{a.rows}")
    for p in ports:
        k = KEYS.get(p, "j")
        o = measure(os.path.join(a.build, "maya_" + p), k, a.term, a.cols, a.rows)
        j = measure(os.path.join(a.build, "maya_jaal_" + p), k, a.term, a.cols, a.rows)
        worse = []
        if j["idle"] > o["idle"] + 1.5: worse.append("idle")
        if j["busy"] > o["busy"] * 1.2 + 2: worse.append("busy")
        if j["med"] and o["med"] and j["med"] > o["med"] * 1.3 + 1: worse.append("latency")
        if j["kb"] > o["kb"] * 1.2 + 0.5: worse.append("bytes")
        if j["miss"] > o["miss"]: worse.append("missed keys")
        print(f"{p:18s} maya {fmt(o)}\n{'':18s} jaal {fmt(j)}  {'WORSE: ' + ','.join(worse) if worse else 'ok'}",
              flush=True)
