#!/usr/bin/env python3
"""tests/inline_progress_test.py BIN — the inline progress card runs to 100%
by itself, exits 0, and leaves its summary on screen (inline mode:
it's in the scrollback, not an alt screen)."""
import os, pty, select, sys, time

pid, fd = pty.fork()
if pid == 0:
    os.execv(sys.argv[1], [sys.argv[1]])
buf, end, status = b"", time.time() + 10, None
while time.time() < end:
    if select.select([fd], [], [], 0.05)[0]:
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            chunk = b""
        if b"\x1b[5n" in chunk:
            os.write(fd, b"\x1b[0n")
        buf += chunk
    done, st = os.waitpid(pid, os.WNOHANG)
    if done:
        status = st
        break
if status is None:
    os.kill(pid, 9)
    sys.exit("inline_progress: still running after 10 s")
if not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
    sys.exit(f"inline_progress: exit status {status}")
if b"completed successfully" not in buf:
    sys.exit("inline_progress: never printed its summary")
if b"\x1b[?1049h" in buf:
    sys.exit("inline_progress: took the alt screen (should be inline)")
print("inline_progress: ok")
