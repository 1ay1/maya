#!/bin/sh
# tests/canvas_smoke.sh [build-dir] : every canvas demo, original vs jaal port.
#
# The ports are generated (tools/port_canvas.py), so a demo and its port run
# the same code; this proves the RUNTIME under them behaves the same: draws,
# reacts to a key, keeps animating with no input, survives a resize, doesn't
# spin idle, quits on q with exit 0, restores the terminal.
set -u
BUILD=${1:-build-jaal}
HERE=$(cd "$(dirname "$0")" && pwd)
pass=0; fail=0; failed=""

# name  keys-that-change-the-screen  key-to-press-before-watching-it-animate
# ("-" = the demo doesn't animate without the mouse: skip that check)
while read -r name keys anim; do
    [ -z "$name" ] && continue
    for variant in "$name" "jaal_$name"; do
        bin="$BUILD/maya_$variant"
        if [ ! -x "$bin" ]; then echo "  MISSING  $variant"; fail=$((fail+1)); failed="$failed $variant"; continue; fi
        [ "$anim" = "SPACE" ] && anim=" "
        if [ "$anim" = "-" ]; then set -- --keys="$keys"; else set -- --keys="$keys" --animates="$anim"; fi
        # A ray tracer renders every frame by design: a real cost, not a spin
        # (a spin is a whole core). Its idle bar is set above what it costs.
        [ "$name" = raymarch ] || [ "$name" = space3d ] && set -- "$@" --idle-cpu=0.9
        if out=$(python3 "$HERE/jaal_smoke.py" "$bin" "$@" 2>&1); then
            echo "  ok       $variant"; pass=$((pass+1))
        else
            echo "  FAIL     $variant"; echo "$out" | grep FAIL | sed 's/^/           /'
            fail=$((fail+1)); failed="$failed $variant"
        fi
    done
done <<'EOF'
doom_fire   12   1
breakout    hl   SPACE
dashboard   12   1
fluid       12   -
fps         m    -
life        rc   r
mandelbrot  12   1
matrix      12   1
particles   12   2
raymarch    12   1
snake       jl   l
sorts       +-   +
space3d     ad   w
spectrum    12   1
EOF

echo "$pass passed, $fail failed${failed:+ :$failed}"
[ "$fail" -eq 0 ]
