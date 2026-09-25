#!/usr/bin/env python3
"""tests/jaal_nav_frames_test.py BIN : every navigation key is drawn.

Sends 20 Down arrows in ONE write (a key-repeat burst, as a fast terminal
delivers it) and records every "row N" the program drew. Navigation keys
get their own frame, so rows 1..20 must ALL appear, in order. With the
rule removed, the burst is folded before drawing and only the last row
shows (checked: planted, it fails).
"""
import fcntl, os, pty, re, select, struct, sys, termios, time
import pyte

def main(binary):
    env = {k: v for k, v in os.environ.items() if k != "NO_COLOR"}
    env["TERM"] = "xterm-256color"
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(binary, [binary], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 10, 40, 0, 0))
    screen = pyte.Screen(40, 10); stream = pyte.ByteStream(screen)
    drawn = []
    def pump(s, record=False):
        end = time.time() + s
        while time.time() < end:
            if select.select([fd], [], [], 0.01)[0]:
                try: d = os.read(fd, 1 << 16)
                except OSError: return
                # Feed frame by frame (each ends with ?2026l): the diff only
                # sends changed cells, so read the ROW off the screen state
                # after every frame, not off the bytes.
                for part in re.split(rb"(?<=\x1b\[\?2026l)", d):
                    stream.feed(part)
                    if record and part.endswith(b"\x1b[?2026l"):
                        m = re.search(r"row (\d+)", screen.display[0])
                        if m: drawn.append(int(m.group(1)))
                for _ in range(d.count(b"\x1b[5n")):
                    os.write(fd, b"\x1b[0n")     # a terminal acknowledging frames
    pump(0.5)
    os.write(fd, b"\x1b[B" * 20)                  # 20 Down arrows, one write
    pump(1.5, record=True)
    os.write(fd, b"q"); pump(0.3)
    try: os.kill(pid, 9)
    except ProcessLookupError: pass
    os.waitpid(pid, 0)
    seen = sorted(set(drawn))
    ok = seen == list(range(1, 21))
    print(("  ok    " if ok else "  FAIL  ") +
          f"every Down in a 20-key burst is drawn (saw {len(seen)} of 20 rows: "
          f"{seen[:6]}{'...' if len(seen) > 6 else ''})")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
