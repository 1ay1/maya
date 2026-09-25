#!/usr/bin/env python3
"""tests/slow_link.py BIN... : a terminal on the far end of a slow link.

A pty whose reader behaves like a remote terminal over ssh: it takes bytes at
a fixed rate (the link), parses them in order, and answers Device Status
Report queries (`ESC[5n` -> `ESC[0n`) only when it has READ up to them - so
an answer means "everything before this reached the screen", exactly as a
real terminal behind a slow connection behaves. It measures what a user
feels:

  fps        frames that reached the screen per second
  input->screen  ms from pressing a key to the screen showing its effect
  q -> clear ms from pressing q to the program exiting AND the screen having
             received everything the program wrote (the backlog the user
             still has to watch)

    python3 tests/slow_link.py build-app/maya_doom_fire build-app/maya_doom_fire --rate 300
"""
import argparse, fcntl, os, pty, select, signal, struct, termios, time

def run(binary, rate_kbs, answer):
    """The program writes into a pty; an `sshd` stand-in reads the pty as
    fast as it can into an unbounded buffer (sshd + the TCP send buffer);
    the `terminal` drains that buffer at the link rate, and only answers a
    DSR once it has drained up to it. That is the ssh shape: the pty never
    pushes back, and the queue builds up where the program can't see it."""
    env = {k: v for k, v in os.environ.items() if k not in ("NO_COLOR", "COLORTERM")}
    env["TERM"] = "xterm-256color"
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(binary, [binary], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 60, 214, 0, 0))
    os.kill(pid, signal.SIGWINCH)
    rate = rate_kbs * 1024 if rate_kbs else float("inf")
    link = bytearray()           # sshd + network: everything not yet on screen
    delivered = 0.0              # credit for bytes the link may deliver
    frames = 0; tail = b""; exited = False
    last = time.perf_counter()
    def step():
        nonlocal delivered, frames, tail, exited, last
        # sshd: slurp whatever the program wrote
        while select.select([fd], [], [], 0)[0]:
            try: d = os.read(fd, 1 << 16)
            except OSError: exited = True; break
            if not d: exited = True; break
            link.extend(d)
        # the link: deliver at `rate`
        now = time.perf_counter()
        delivered += (now - last) * rate; last = now
        n = len(link) if rate == float("inf") else min(len(link), int(delivered))
        if n:
            chunk = bytes(link[:n]); del link[:n]; delivered -= n if rate != float("inf") else 0
            buf = tail + chunk
            frames += buf.count(b"\x1b[?2026l")
            if answer and not exited:
                for _ in range(buf.count(b"\x1b[5n")):
                    try: os.write(fd, b"\x1b[0n")
                    except OSError: pass
            tail = buf[-8:]
        elif not link:
            delivered = min(delivered, rate * 0.01 if rate != float("inf") else 0)
        time.sleep(0.001)
    def pump(secs):
        end = time.perf_counter() + secs
        while time.perf_counter() < end and not exited: step()
    pump(2.0)
    frames = 0
    pump(3.0)
    fps = frames / 3.0
    backlog_at_q = len(link)
    # q -> the screen stops: the program has exited and the link has
    # delivered everything it wrote before exiting.
    t0 = time.perf_counter()
    try: os.write(fd, b"q")
    except OSError: pass
    while time.perf_counter() - t0 < 20:
        step()
        if (exited or os.waitpid(pid, os.WNOHANG)[0] == pid) and not link:
            break
    q_ms = (time.perf_counter() - t0) * 1000
    try: os.kill(pid, 9)
    except ProcessLookupError: pass
    try: os.waitpid(pid, 0)
    except ChildProcessError: pass
    try: os.close(fd)
    except OSError: pass
    return fps, q_ms, backlog_at_q

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("binaries", nargs="+")
    ap.add_argument("--rate", type=int, action="append")
    ap.add_argument("--no-answer", action="store_true", help="a terminal that ignores DSR")
    a = ap.parse_args()
    for b in a.binaries:
        for r in (a.rate or [0, 1000, 300]):
            fps, q, backlog = run(b, r, not a.no_answer)
            label = f"{r} KB/s" if r else "unlimited"
            print(f"{os.path.basename(b):24s} link {label:>10s}: {fps:5.1f} fps on screen, "
                  f"backlog when q pressed {backlog/1024:7.0f} KB, q -> screen stops {q:7.0f} ms",
                  flush=True)
