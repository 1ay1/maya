#!/usr/bin/env python3
# input_latency_gate.py — a regression gate for key-to-first-byte latency.
#
# Spawns a maya app under a PTY, sends N keystrokes, measures the wall time
# from write() to the first byte back per key, and fails if median or p95
# breach the thresholds. Numbers are set generously — the interesting event
# is a REGRESSION (median doubles, p95 jumps 5x), not a hardware floor.
#
# Measured baseline on this machine, maya_counter Release, no load:
#   median  ~0.2 ms   p95  ~0.4 ms   min ~0.03 ms
# The thresholds below carry ~15x headroom over median and ~12x over p95, so
# a CI runner or a shared box with higher jitter still passes. If they trip,
# something in the input pipeline (device drain, event parse, jaal fold,
# view() build, maya diff-and-write) regressed by an order of magnitude.
#
# The point of THIS test is NOT the microbench numbers — those live in
# jaal_bench and are already tight. The point is that the END-TO-END pipe
# (PTY read -> Sub::on_key -> Msg -> update -> view -> Element -> renderer
# -> PTY write) stays fast under real conditions, including tty setup and
# escape parsing. If a change in maya (e.g. a slow renderer walk) or a
# change in jaal (e.g. an extra reconcile per key) doubles this number,
# users feel it as sluggishness that microbenchmarks would not surface.

import argparse, fcntl, os, pty, select, signal, statistics, struct, sys, termios, time


def drain(fd, quiet_s=0.05, cap_s=1.0):
    """Read until the fd has been quiet for quiet_s (or cap_s elapses)."""
    end = time.perf_counter() + cap_s
    total = bytearray()
    while time.perf_counter() < end:
        r, _, _ = select.select([fd], [], [], quiet_s)
        if not r:
            break
        try:
            chunk = os.read(fd, 1 << 16)
        except OSError:
            break
        if not chunk:
            break
        total += chunk
    return bytes(total)


def measure(binary, presses, key):
    pid, fd = pty.fork()
    if pid == 0:
        env = dict(os.environ, TERM="xterm-256color", MAYA_COLOR="truecolor")
        env.pop("NO_COLOR", None)
        os.execve(binary, [binary], env)

    # 40x120 is a reasonable terminal size; too small and layout may reject
    # a widget, too large and cost is unrepresentatively high.
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))

    # Let the app finish its opening handshake (mode enable, initial
    # frame). Uses a generous quiet window and cap since some apps stream
    # setup for a while.
    drain(fd, quiet_s=0.20, cap_s=3.0)

    latencies_ms = []
    for i in range(presses):
        t0 = time.perf_counter()
        try:
            os.write(fd, key)
        except OSError:
            break  # child exited (a quit key hit it)
        r, _, _ = select.select([fd], [], [], 1.0)
        if not r:
            continue  # no reply within a second: something is wrong, skip
        got_ms = (time.perf_counter() - t0) * 1000.0
        # Consume whatever else the frame emits so the next iteration
        # doesn't see stale bytes as its "first reply".
        drain(fd, quiet_s=0.03, cap_s=0.5)
        latencies_ms.append(got_ms)

    # Clean up. `q` is the counter example's quit key; for others we fall
    # back to SIGTERM.
    try:
        os.write(fd, b"q")
    except OSError:
        pass
    end = time.perf_counter() + 0.5
    while time.perf_counter() < end:
        try:
            done, _ = os.waitpid(pid, os.WNOHANG)
            if done == pid:
                break
        except ChildProcessError:
            break
        time.sleep(0.02)
    else:
        os.kill(pid, signal.SIGTERM)
        os.waitpid(pid, 0)
    os.close(fd)

    return latencies_ms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary", help="A maya app to drive (defaults to counter).")
    ap.add_argument("--presses",       type=int,   default=60)
    ap.add_argument("--key",           default="+", help="Key to send each press.")
    ap.add_argument("--median-ms-max", type=float, default=3.0,
                    help="Fail if the median > this. Default 3 ms (~15x baseline).")
    ap.add_argument("--p95-ms-max",    type=float, default=5.0,
                    help="Fail if p95 > this. Default 5 ms (~12x baseline).")
    ap.add_argument("--min-samples",   type=int,   default=30,
                    help="Fewer than this and we say the run itself failed.")
    ap.add_argument("--verbose",       action="store_true")
    a = ap.parse_args()

    if not os.access(a.binary, os.X_OK):
        print(f"input_latency_gate: {a.binary} is not an executable", file=sys.stderr)
        return 2

    key = a.key.encode()
    lat = measure(a.binary, a.presses, key)

    if len(lat) < a.min_samples:
        print(f"input_latency_gate: only {len(lat)} samples (need {a.min_samples}); "
              "the binary is exiting early or not responding.", file=sys.stderr)
        return 1

    lat_sorted = sorted(lat)
    med = statistics.median(lat_sorted)
    p95 = lat_sorted[int(0.95 * (len(lat_sorted) - 1))]
    mn, mx = lat_sorted[0], lat_sorted[-1]

    print(f"input_latency_gate: {len(lat)} keys, min={mn:.2f}ms  "
          f"median={med:.2f}ms  p95={p95:.2f}ms  max={mx:.2f}ms")
    if a.verbose:
        print("  samples (ms):", ", ".join(f"{x:.2f}" for x in lat))

    fails = []
    if med > a.median_ms_max:
        fails.append(f"median {med:.2f}ms > {a.median_ms_max}ms")
    if p95 > a.p95_ms_max:
        fails.append(f"p95 {p95:.2f}ms > {a.p95_ms_max}ms")
    if fails:
        print("input_latency_gate FAILED: " + "; ".join(fails), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
