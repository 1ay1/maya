#!/usr/bin/env python3
"""tests/cpu_at.py BIN [TERM...]: program CPU, bytes/s and fps at 214x60,
for each TERM given, with no COLORTERM (so a 256-colour TERM means level 2,
the ssh / tmux case). Always kills the child; never blocks longer than ~5 s
per TERM.
"""
import fcntl, os, pty, select, signal, struct, subprocess, sys, termios, time

def cpu(pid):
    t = subprocess.run(["ps", "-o", "time=", "-p", str(pid)],
                       capture_output=True, text=True, timeout=2).stdout.strip()
    if not t:
        return None
    parts = t.split(":")
    return sum(float(x) * 60 ** i for i, x in enumerate(reversed(parts)))

def run(binary, term, cols=214, rows=60, secs=3.0):
    env = {k: v for k, v in os.environ.items()
           if k not in ("NO_COLOR", "COLORTERM", "MAYA_COLOR", "CLICOLOR", "CLICOLOR_FORCE")}
    env["TERM"] = term
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(binary, [binary], env)
    try:
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        os.kill(pid, signal.SIGWINCH)
        def pump(until, count):
            n = f = 0
            while time.time() < until:
                if select.select([fd], [], [], 0.05)[0]:
                    try:
                        d = os.read(fd, 1 << 16)
                    except OSError:
                        break
                    if not d:
                        break
                    if count:
                        n += len(d); f += d.count(b"\x1b[?2026h")
            return n, f
        pump(time.time() + 1.0, False)
        c0 = cpu(pid); t0 = time.time()
        n, f = pump(t0 + secs, True)
        c1 = cpu(pid); el = time.time() - t0
        c = f"{(c1 - c0) / el * 100:4.0f}%" if c0 is not None and c1 is not None else "  ? "
        print(f"{os.path.basename(binary):22s} TERM={term:16s} cpu {c}  "
              f"{n / el / 1024:6.0f} KB/s  {f / el:5.1f} fps", flush=True)
    finally:
        # Close OUR end first: a child killed mid-write to a full pty can
        # sit in exit until the master is gone, and waitpid would hang.
        os.close(fd)
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        for _ in range(40):
            if os.waitpid(pid, os.WNOHANG)[0] == pid:
                break
            time.sleep(0.05)

if __name__ == "__main__":
    b = sys.argv[1]
    for term in (sys.argv[2:] or ["tmux-256color", "xterm-256color"]):
        run(b, term)
