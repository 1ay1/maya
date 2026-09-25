#!/bin/sh
# tests/kitty_quit.sh BIN [font_size]: press q in a REAL fullscreen kitty and time
# how long until the program exits. Uses kitty remote control to send the key.
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
K=/Applications/kitty.app/Contents/MacOS/kitty
SOCK=/tmp/kq.$$
$K --start-as=fullscreen -o font_size=${2:-13} -o remember_window_size=no \
   -o allow_remote_control=yes --listen-on unix:$SOCK --title kittyquit "$BIN" >/dev/null 2>&1 &
KP=$!
sleep 3
P=$(pgrep -f "^$BIN" | head -1)
[ -z "$P" ] && { echo "program not running"; kill $KP; exit 1; }
python3 - "$P" "$SOCK" "$K" <<'EOF'
import os, subprocess, sys, time
pid, sock, k = int(sys.argv[1]), sys.argv[2], sys.argv[3]
t0 = time.perf_counter()
cmd = ["send-key", "q"] if os.environ.get("MODE") == "key" else ["send-text", "q"]
subprocess.run([k, "@", "--to", f"unix:{sock}", *cmd], capture_output=True)
t1 = time.perf_counter()
while True:
    try: os.kill(pid, 0)
    except ProcessLookupError: break
    if time.perf_counter() - t0 > 20: print("never exited"); break
    time.sleep(0.002)
t2 = time.perf_counter()
print(f"send-text took {(t1-t0)*1000:.0f} ms; program exited {(t2-t0)*1000:.0f} ms after q")
EOF
sleep 0.5; kill $KP 2>/dev/null; rm -f $SOCK; true
