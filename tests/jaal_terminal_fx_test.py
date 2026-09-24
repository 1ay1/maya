#!/usr/bin/env python3
"""tests/jaal_terminal_fx_test.py: the jaal host's terminal effects on a real pty.

The smoke harness proves a program draws, reacts, quits. This proves each
terminal EFFECT reaches the wire as the bytes maya's own loop would emit,
and that `suspend` hands the tty to a child and folds its exit back in as a
message (jaal D39): the child's own output appears, then the program's
"child exited 3" line.

    python3 tests/jaal_terminal_fx_test.py build-jaal/maya_jaal_terminal_fx
"""
import os, pty, select, signal, struct, fcntl, termios, sys, time

def run(binary):
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.execv(binary, [binary])
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
    buf = bytearray()

    def pump(secs):
        end = time.time() + secs
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                try:
                    chunk = os.read(fd, 65536)
                except OSError:
                    return
                if not chunk:
                    return
                buf.extend(chunk)

    def until(needle, secs=3.0):
        end = time.time() + secs
        while time.time() < end:
            if needle in buf:
                return True
            pump(0.05)
        return needle in buf

    def press(k, needle, secs=3.0):
        start = len(buf)
        os.write(fd, k)
        end = time.time() + secs
        while time.time() < end:
            if needle in buf[start:]:
                return bytes(buf[start:])
            pump(0.05)
        return None

    failures = []
    def check(name, ok):
        print(("  ok    " if ok else "  FAIL  ") + name)
        if not ok:
            failures.append(name)

    check("draws", until(b"terminal fx on jaal"))

    out = press(b"t", b"\x1b]0;terminal fx")
    check("set_title writes OSC 0 with the title", out is not None)

    out = press(b"c", b"\x1b]52;c;")
    check("write_clipboard writes OSC 52 (base64 'from jaal')",
          out is not None and b"ZnJvbSBqYWFs" in out)

    out = press(b"o", b"\x1b]9;jaal says hi\x1b\\")
    check("emit_host_sequence writes the sequence verbatim", out is not None)

    out = press(b"r", b"force_redraw")
    check("force_redraw repaints (and logs)", out is not None)

    out = press(b"i", b"reset_inline")
    check("reset_inline starts a fresh frame (and logs)", out is not None)

    # suspend: the child's own line reaches the tty, THEN the answer is
    # folded and drawn. Order matters: the answer can't beat the child.
    start = len(buf)
    os.write(fd, b"s")
    got_answer = until(b"child exited 3", 5.0)
    tail = bytes(buf[start:])
    child_at = tail.find(b"child ran on the real tty")
    answer_at = tail.find(b"child exited 3")
    check("suspend: the child writes to the real tty", child_at >= 0)
    check("suspend: the exit is folded back in as a message (D39)", got_answer)
    check("suspend: the answer comes after the child ran",
          child_at >= 0 and answer_at > child_at)

    # After suspend the TUI is live again: a key still works.
    out = press(b"t", b"set_title")
    check("after suspend, input still reaches the program", out is not None)

    os.write(fd, b"q")
    code = None
    end = time.time() + 3
    while time.time() < end:
        pump(0.05)
        p, st = os.waitpid(pid, os.WNOHANG)
        if p == pid:
            code = os.waitstatus_to_exitcode(st)
            break
    if code is None:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
    check("quits with 0", code == 0)

    name = os.path.basename(binary)
    print(f"{name}: {'ok' if not failures else 'FAILED'}")
    return 1 if failures else 0

def canvas(binary):
    """A theme that owns its canvas (Dracula, #282A36) must fill the frame.
    maya's loop wraps the view in apply_theme_canvas; a host that forgets
    leaves the terminal's own background showing through."""
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.environ["MAYA_COLOR"] = "truecolor"   # pin the tier: the caller may export NO_COLOR
        os.execv(binary, [binary, "--dracula"])
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
    buf = bytearray()
    end = time.time() + 3
    while time.time() < end and b"terminal fx on jaal" not in buf:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                buf.extend(os.read(fd, 65536))
            except OSError:
                break
    time.sleep(0.2)
    try:
        while select.select([fd], [], [], 0.1)[0]:
            buf.extend(os.read(fd, 65536))
    except OSError:
        pass
    os.write(fd, b"q")
    time.sleep(0.3)
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    os.waitpid(pid, 0)
    # A theme that owns its canvas must reach the screen at all, and fill
    # each frame row to the terminal's right edge. The first half is what
    # broke: RunConfig::theme was dropped at startup on both loops (see
    # Runtime::publish_theme_slot), so this starts in native and fails.
    import pyte
    screen = pyte.Screen(100, 30)
    pyte.ByteStream(screen).feed(bytes(buf))
    title_row = next((y for y in range(30)
                      if "terminal fx on jaal" in "".join(
                          screen.buffer[y][x].data for x in range(100))), None)
    bg = lambda y, x: screen.buffer[y][x].bg
    first_bg = bg(title_row, 0) if title_row is not None else None
    edge_bg = bg(title_row, 99) if title_row is not None else None
    ok = (title_row is not None and edge_bg not in ("default", None)
          and edge_bg == first_bg)
    print(("  ok    " if ok else "  FAIL  ") +
          "a canvas-owning theme fills each frame row to the right edge"
          + ("" if ok else f" (row {title_row}: col0={first_bg} col99={edge_bg})"))
    return 0 if ok else 1

if __name__ == "__main__":
    rc = run(sys.argv[1])
    rc |= canvas(sys.argv[1])
    sys.exit(rc)
