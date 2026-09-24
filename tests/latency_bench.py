#!/usr/bin/env python3
"""tests/latency_bench.py: how fast does each example FEEL?

For every binary given: time to first frame, keypress -> screen latency
(median / p90 over N presses), and idle CPU. Runs in a real pty, like a user.

    python3 tests/latency_bench.py build-jaal/maya_* [--presses 20] [--key j]
"""
import argparse, fcntl, os, pty, resource, select, signal, statistics, struct, sys, termios, time

def cpu_of(pid):
    # ps gives cumulative cpu time "M:SS.ss"
    out = os.popen(f"ps -o time= -p {pid}").read().strip()
    if not out:
        return None
    m, s = out.split(":")
    return int(m) * 60 + float(s)

def drain(fd, secs):
    """Read for up to secs; return (bytes, time of last byte)."""
    buf = bytearray(); last = None
    end = time.perf_counter() + secs
    while True:
        left = end - time.perf_counter()
        if left <= 0:
            break
        r, _, _ = select.select([fd], [], [], left)
        if not r:
            break
        try:
            chunk = os.read(fd, 1 << 16)
        except OSError:
            break
        if not chunk:
            break
        buf += chunk; last = time.perf_counter()
    return bytes(buf), last

def settle(fd, quiet=0.15, cap=3.0):
    """Read until the output has been quiet for `quiet` s."""
    total = bytearray(); end = time.perf_counter() + cap
    while time.perf_counter() < end:
        b, _ = drain(fd, quiet)
        if not b:
            return bytes(total)
        total += b
    return bytes(total)

def bench(binary, keys, presses, quit_key):
    env = dict(os.environ, TERM="xterm-256color", MAYA_COLOR="truecolor")
    env.pop("NO_COLOR", None)
    t0 = time.perf_counter()
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(binary, [binary], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))
    # first frame: first byte that looks like a draw (after setup escapes)
    first = None; startup_bytes = 0
    end = time.perf_counter() + 5
    buf = bytearray()
    while time.perf_counter() < end:
        r, _, _ = select.select([fd], [], [], 0.02)
        if r:
            try:
                c = os.read(fd, 1 << 16)
            except OSError:
                break
            buf += c
            # any printable text past the escape preamble counts as a frame
            if first is None and any(32 < ch < 127 for ch in c.replace(b"\x1b", b" ")
                                     if True) and len(buf) > 200:
                first = time.perf_counter() - t0
        elif first is not None:
            break
    settle(fd)

    lat = []; bytes_per = []
    for i in range(presses):
        k = keys[i % len(keys)].encode()
        t = time.perf_counter()
        try:
            os.write(fd, k)
        except OSError:
            break                                  # it exited (a key quit it)
        # latency = time to first output byte after the key
        r, _, _ = select.select([fd], [], [], 1.0)
        if not r:
            lat.append(None); continue
        got = time.perf_counter() - t
        b = settle(fd, quiet=0.05, cap=1.0)
        lat.append(got); bytes_per.append(len(b))

    # Idle: no input for 2 s. Frames (synchronized-update brackets) and
    # bytes per second: what the terminal emulator has to chew through.
    c0 = cpu_of(pid)
    idle_bytes, _ = drain(fd, 2.0)
    c1 = cpu_of(pid)
    idle = None if c0 is None or c1 is None else (c1 - c0) / 2.0 * 100
    fps = idle_bytes.count(b"\x1b[?2026h") / 2.0
    kbps = len(idle_bytes) / 2.0 / 1024

    try:
        os.write(fd, quit_key.encode())
    except OSError:
        pass
    code = None; end = time.perf_counter() + 2
    while time.perf_counter() < end:
        drain(fd, 0.05)
        p, st = os.waitpid(pid, os.WNOHANG)
        if p == pid:
            code = os.waitstatus_to_exitcode(st); break
    if code is None:
        os.kill(pid, signal.SIGKILL); os.waitpid(pid, 0); code = "KILLED"

    ok = [x for x in lat if x is not None]
    miss = len(lat) - len(ok)
    ms = lambda x: f"{x*1000:6.1f}" if x is not None else "     -"
    med = statistics.median(ok) if ok else None
    p90 = sorted(ok)[int(len(ok) * 0.9) - 1] if len(ok) >= 2 else med
    kb = (statistics.median(bytes_per) / 1024) if bytes_per else 0
    name = os.path.basename(binary).replace("maya_", "")
    print(f"{name:24s} key med{ms(med)} p90{ms(p90)}ms miss{miss:3d} {kb:7.1f}KB/key"
          f" | idle {fps:5.1f}fps {kbps:8.1f}KB/s cpu {idle if idle is None else round(idle)}%"
          f" | exit {code}",
          flush=True)

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("binaries", nargs="+")
    ap.add_argument("--presses", type=int, default=20)
    ap.add_argument("--keys", default="jkjk")
    ap.add_argument("--quit", default="q")
    a = ap.parse_args()
    for b in a.binaries:
        bench(b, a.keys, a.presses, a.quit)
