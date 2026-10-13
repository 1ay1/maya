#!/usr/bin/env python3
"""tests/ssh_quit.py BIN [--rate KB/s] : `q` over a REAL ssh session.

Runs `ssh -tt localhost BIN` in a pty (so sshd, a real pty on the far side
and TCP are all in the path, exactly like a remote session), reads the
ssh client's output at a fixed rate (the user's link / terminal), waits,
presses q, and reports when the bytes stop arriving (the screen stops) and
whether the program's exit (the alt-screen leave, ESC[?1049l) arrived.
"""
import argparse, fcntl, os, pty, select, signal, struct, termios, time

def run(binary, rate_kbs, warm):
    pid, fd = pty.fork()
    if pid == 0:
        os.execvp("ssh", ["ssh", "-tt", "-o", "BatchMode=yes", "localhost",
                          f"TERM=xterm-256color {binary}"])
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 60, 214, 0, 0))
    rate = rate_kbs * 1024 if rate_kbs else None
    def read_for(secs, stop_at=None):
        got = bytearray(); end = time.perf_counter() + secs; credit = 0.0; last = time.perf_counter()
        while time.perf_counter() < end:
            now = time.perf_counter()
            if rate:
                credit = min(credit + (now - last) * rate, rate * 0.02); last = now
                if credit < 512: time.sleep(0.002); continue
            r, _, _ = select.select([fd], [], [], 0.005)
            if not r: continue
            try: d = os.read(fd, int(min(credit, 65536)) if rate else 65536)
            except OSError: break
            if not d: break
            credit -= len(d); got += d
            # the far terminal answers flow-control queries when it reaches them
            for _ in range(d.count(b"\x1b[5n")):
                try: os.write(fd, b"\x1b[0n")
                except OSError: pass
            if stop_at and stop_at in got: return got, time.perf_counter()
        return got, None
    read_for(warm)
    t0 = time.perf_counter()
    os.write(fd, b"q")
    got, t_exit = read_for(20.0, stop_at=b"\x1b[?1049l")
    try: os.kill(pid, signal.SIGKILL)
    except ProcessLookupError: pass
    try: os.waitpid(pid, 0)
    except ChildProcessError: pass
    return (t_exit - t0) * 1000 if t_exit else None, len(got)

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("binaries", nargs="+")
    ap.add_argument("--rate", type=int, action="append")
    ap.add_argument("--warm", type=float, default=3.0)
    a = ap.parse_args()
    for b in a.binaries:
        for r in (a.rate or [0, 1000, 300]):
            ms, n = run(os.path.abspath(b), r, a.warm)
            lab = f"{r} KB/s" if r else "unlimited"
            print(f"{os.path.basename(b):24s} ssh, link {lab:>10s}: q -> screen restored "
                  f"{('%7.0f ms' % ms) if ms is not None else '   >20 s'}  ({n/1024:6.0f} KB drawn after q)",
                  flush=True)
