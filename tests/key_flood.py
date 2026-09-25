#!/usr/bin/env python3
"""tests/key_flood.py BIN... : CPU cost per keypress under a flood.

Sends keys at a fixed rate (default 500/s, well past autorepeat) for a few
seconds into a real pty at 214x60, keeps the output drained, and reports
CPU microseconds per key, frames drawn, and bytes per key. The loop's
per-event overhead is what this isolates: every example does the same work
per key on both runtimes, so any difference is the runtime.

    python3 tests/key_flood.py build-app/maya_widgets build-app/maya_widgets
"""
import argparse, fcntl, os, pty, select, signal, struct, subprocess, termios, time

def cpu(pid):
    t = subprocess.run(["ps", "-o", "time=", "-p", str(pid)], capture_output=True,
                       text=True, timeout=2).stdout.strip()
    return sum(float(x) * 60 ** i for i, x in enumerate(reversed(t.split(":")))) if t else None

def flood(binary, key, rate, secs, reps):
    results = []
    for _ in range(reps):
        env = {k: v for k, v in os.environ.items() if k not in ("NO_COLOR", "COLORTERM", "MAYA_COLOR")}
        env["TERM"] = "tmux-256color"
        pid, fd = pty.fork()
        if pid == 0:
            os.execve(binary, [binary], env)
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 60, 214, 0, 0))
        os.kill(pid, signal.SIGWINCH)
        def drain(until):
            n = f = 0
            while True:
                left = until - time.perf_counter()
                if left <= 0: return n, f
                if select.select([fd], [], [], min(left, 0.0005))[0]:
                    try: d = os.read(fd, 1 << 16)
                    except OSError: return n, f
                    n += len(d); f += d.count(b"\x1b[?2026h") + d.count(b"\x1b[H")
        drain(time.perf_counter() + 1.2)
        c0 = cpu(pid); t0 = time.perf_counter(); sent = 0; nbytes = 0; frames = 0
        period = 1.0 / rate
        while time.perf_counter() - t0 < secs:
            try: os.write(fd, key)
            except OSError: break
            sent += 1
            n, f = drain(t0 + sent * period)
            nbytes += n; frames += f
        n, f = drain(time.perf_counter() + 0.3); nbytes += n; frames += f
        c1 = cpu(pid)
        os.close(fd)
        try: os.kill(pid, 9)
        except ProcessLookupError: pass
        try: os.waitpid(pid, 0)
        except ChildProcessError: pass
        results.append(((c1 - c0) / sent * 1e6, frames, nbytes / sent))
    results.sort()
    return results[len(results) // 2], sent

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("binaries", nargs="+")
    ap.add_argument("--key", default="j")
    ap.add_argument("--rate", type=int, default=500)
    ap.add_argument("--secs", type=float, default=3.0)
    ap.add_argument("--reps", type=int, default=3)
    a = ap.parse_args()
    for b in a.binaries:
        (us, frames, bpk), sent = flood(b, a.key.encode(), a.rate, a.secs, a.reps)
        print(f"{os.path.basename(b):28s} {us:7.1f} us CPU/key   {frames:5d} frames for {sent} keys   {bpk:7.0f} B/key",
              flush=True)
