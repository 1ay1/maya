#!/usr/bin/env python3
"""tests/view_fault_test.py BIN — a throwing view() is a fault, not an abort.

  stop (default)  the program exits with jaal's fault exit code (70), and
                  the terminal is given back (alt screen left, cursor shown)
  skip            the program stays alive on its last good frame and still
                  takes keys

Before the host caught it, the exception unwound out of the loop: SIGABRT,
no report, and nothing said which program phase failed.
"""
import os, pty, select, struct, sys, termios, fcntl, time

BIN = sys.argv[1] if len(sys.argv) > 1 else "build-app/maya_view_fault"
BIN = os.path.abspath(BIN)
fails = []

def check(ok, what):
    print(("  ok    " if ok else "  FAIL  ") + what)
    if not ok:
        fails.append(what)

def run(keys, env_extra=None, wait=3.0):
    env = {k: v for k, v in os.environ.items() if k not in ("NO_COLOR", "CLICOLOR")}
    env["TERM"] = "xterm-256color"
    env.update(env_extra or {})
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(BIN, [BIN], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    out = bytearray()
    def pump(t):
        end = time.time() + t
        while time.time() < end:
            if select.select([fd], [], [], 0.02)[0]:
                try:
                    b = os.read(fd, 1 << 20)
                except OSError:
                    # The child closed the pty: it is exiting, or already has.
                    # Wait for its real status rather than inventing one —
                    # returning a sentinel here made "q quits with 0" fail
                    # against the string "eof", which is not an exit status.
                    _, st = os.waitpid(pid, 0)
                    return st
                out.extend(b)
                for _ in range(b.count(b"\x1b[5n")):
                    os.write(fd, b"\x1b[0n")
            done, st = os.waitpid(pid, os.WNOHANG)
            if done:
                return st
        return None
    st = pump(1.0)
    for k in keys:
        if st is not None:
            break
        os.write(fd, k)
        st = pump(0.6)
    if st is None:
        st = pump(wait)
    alive = st is None
    if alive:
        os.kill(pid, 9)
        os.waitpid(pid, 0)
    return alive, st, bytes(out)

print(f"{os.path.basename(BIN)}:")

# stop (the default policy)
alive, st, out = run([b"x"])
check(not alive, "a throwing view() ends the program (no hang)")
if not alive and isinstance(st, int):
    check(os.WIFEXITED(st), "it EXITS rather than dying on a signal")
    if os.WIFEXITED(st):
        check(os.WEXITSTATUS(st) == 70, f"with jaal's fault exit code 70 (got {os.WEXITSTATUS(st)})")
check(b"\x1b[?1049l" in out, "the alt screen is left")
check(b"\x1b[?25h" in out, "the cursor is shown again")

# skip: stay alive on the last good frame, keep taking keys
alive, st, out = run([b"+", b"x", b"+", b"+"], {"MAYA_VIEW_FAULT_SKIP": "1"}, wait=1.2)
check(alive, "under fault_policy::skip it keeps running")
check(b"fault in view" in out, "jaal reports the fault, naming the view site")

# and it still quits cleanly afterwards
alive, st, out = run([b"+", b"x", b"q"], {"MAYA_VIEW_FAULT_SKIP": "1"}, wait=2.0)
check(not alive and isinstance(st, int) and os.WIFEXITED(st) and os.WEXITSTATUS(st) == 0,
      "after a skipped view fault, q still quits with 0")

print(f"{os.path.basename(BIN)}: " + ("ok" if not fails else f"{len(fails)} FAILED"))
sys.exit(1 if fails else 0)
