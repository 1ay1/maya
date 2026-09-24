#!/bin/sh
# tests/kitty_cpu.sh BIN [secs]: run BIN fullscreen in a real kitty window and
# report CPU as time DELTAS (ps %cpu on macOS is a decaying average and lies
# for a process a few seconds old). Prints program and kitty CPU in %.
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); SECS=${2:-5}
K=/Applications/kitty.app/Contents/MacOS/kitty
$K --start-as=fullscreen -o font_size=${FONT:-13} -o remember_window_size=no --title kittycpu "$BIN" >/dev/null 2>&1 &
sleep 2.5
P=$(pgrep -f "^$BIN" | head -1)
KP=$(ps -o ppid= -p "$P" | tr -d ' ')
secs() { ps -o time= -p "$1" | awk -F: '{ if (NF==3) print $1*3600+$2*60+$3; else print $1*60+$2 }'; }
p0=$(secs "$P"); k0=$(secs "$KP"); sleep "$SECS"; p1=$(secs "$P"); k1=$(secs "$KP")
size=$(ps -o command= -p "$P" >/dev/null; echo)
echo "$(basename "$BIN"): program $(echo "($p1-$p0)*100/$SECS" | bc)%  kitty $(echo "($k1-$k0)*100/$SECS" | bc)%"
kill "$P" 2>/dev/null; sleep 0.3; kill "$KP" 2>/dev/null; true
