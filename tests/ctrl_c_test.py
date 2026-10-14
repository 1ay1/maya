#!/usr/bin/env python3
# tests/ctrl_c_test.py — ^C ends a program that doesn't handle it, like SIGINT.
#
# Raw mode turns ^C into a key byte, so the kernel never sees SIGINT. The
# terminal host has to apply the same rule: unhandled ^C exits 130 with the
# terminal restored. A program that does handle ^C keeps running.
#
#   python3 tests/ctrl_c_test.py build/maya_counter [build/maya_chat]
import os, pty, select, signal, sys, time

def run(binary, keys=b"\x03", wait=2.0):
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(binary, [binary])
    out = b""
    t = time.time()
    while time.time() - t < 0.8:              # let it draw the first frame
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try: out += os.read(fd, 65536)
            except OSError: break
    os.write(fd, keys)
    status, t = None, time.time()
    while time.time() - t < wait:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try: out += os.read(fd, 65536)
            except OSError: pass
        p, s = os.waitpid(pid, os.WNOHANG)
        if p: status = s; break
    if status is None:
        os.kill(pid, signal.SIGKILL); os.waitpid(pid, 0)
        return None, out
    return os.waitstatus_to_exitcode(status), out

failures = 0
def check(ok, what):
    global failures
    print(("  ok    " if ok else "  FAIL  ") + what)
    if not ok: failures += 1

code, out = run(sys.argv[1] if len(sys.argv) > 1 else "build/maya_counter")
check(code == 130, f"unhandled ^C exits 130 (got {code})")
check(b"\x1b[?1049l" in out or b"\x1b[?25h" in out, "the terminal is restored")

if len(sys.argv) > 2:
    # a program whose on(KeyEvent) takes ^C quits the way IT chose (exit 0)
    code, _ = run(sys.argv[2])
    check(code == 0, f"a program that handles ^C decides (got {code})")

print("ctrl_c_test: " + ("ok" if not failures else f"{failures} FAILED"))
sys.exit(1 if failures else 0)
