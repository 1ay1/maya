#!/usr/bin/env python3
"""tests/profile_top.py NAME... : run each example at 214x60 (TERM=tmux-256color)
in a pty, `sample` it for 3 s, print the top self-time frames. All at once."""
import os, pty, select, signal, struct, subprocess, sys, termios, fcntl, time, threading

def run(name, build, secs=3.0):
    b = os.path.join(build, name if name.startswith("maya_") else "maya_" + name)
    env = {k: v for k, v in os.environ.items()
           if k not in ("NO_COLOR", "COLORTERM", "MAYA_COLOR", "CLICOLOR", "CLICOLOR_FORCE")}
    env["TERM"] = os.environ.get("PROF_TERM", "tmux-256color")
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(b, [b], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 60, 214, 0, 0))
    os.kill(pid, signal.SIGWINCH)
    stop = time.time() + secs + 2.5
    def pump():
        while time.time() < stop:
            if select.select([fd], [], [], 0.05)[0]:
                try:
                    if not os.read(fd, 1 << 16): return
                except OSError: return
    t = threading.Thread(target=pump); t.start()
    time.sleep(1.2)
    out = f"/tmp/prof_{os.path.basename(b)}.txt"
    subprocess.run(["sample", str(pid), str(int(secs)), "-file", out],
                   capture_output=True, timeout=secs + 20)
    t.join()
    os.close(fd)
    try: os.kill(pid, signal.SIGKILL)
    except ProcessLookupError: pass
    try: os.waitpid(pid, 0)
    except ChildProcessError: pass
    lines = open(out).read().split("Sort by top of stack")[1].splitlines()[1:14]
    rows = [l.strip() for l in lines if l.strip() and "Binary Images" not in l]
    return os.path.basename(b), rows

if __name__ == "__main__":
    build = os.environ.get("BUILD", "build-jaal")
    res = {}
    ths = [threading.Thread(target=lambda n=n: res.__setitem__(n, run(n, build))) for n in sys.argv[1:]]
    for t in ths: t.start()
    for t in ths: t.join()
    for n in sys.argv[1:]:
        name, rows = res[n]
        print("==", name)
        for r in rows[:9]:
            print("   ", r[:150])
