#!/bin/sh
# tests/smoke_all.sh — run smoke.py over every example.
#
# One line per program, and a non-zero exit if any fails. Each entry names
# the keys that visibly change that program's screen (--keys), whether it
# reacts to keys at all (--no-input-change), and its quit key.
#
#   sh tests/smoke_all.sh [build-dir]
#
# Add a line here when an example is ported: an example that isn't in this
# list isn't being checked.
#
# Always write --keys="..." (with =). A key string starting with '-' (a
# program whose "zoom out" is '-') is otherwise parsed as a flag, and the
# program "fails" for a reason that has nothing to do with it.

BUILD=${1:-build-app}
HERE=$(dirname "$0")
# The harness reads the screen through pyte: use a python3 that has it
# (a non-login shell may find the system one first).
PY=python3
for cand in python3 /opt/homebrew/opt/python@3.13/libexec/bin/python3 /opt/homebrew/bin/python3 /usr/local/bin/python3 /usr/bin/python3; do
    if command -v "$cand" >/dev/null 2>&1 && "$cand" -c "import pyte" 2>/dev/null; then PY=$cand; break; fi
done
pass=0
fail=0
failed=""

run() {
    name=$1; shift
    bin="$BUILD/maya_$name"
    if [ ! -x "$bin" ]; then
        echo "  MISSING  $name ($bin not built)"
        fail=$((fail + 1)); failed="$failed $name"
        return
    fi
    if out=$($PY "$HERE/smoke.py" "$bin" "$@" 2>&1); then
        echo "  ok       $name"
        pass=$((pass + 1))
    else
        echo "  FAIL     $name"
        # Show the checks that failed. If none did, the harness itself broke
        # (bad arguments, a crash before the first check): show its output
        # instead of an empty FAIL, which is how the viewport run once
        # "failed" with nothing to say why.
        if echo "$out" | grep -q "FAIL "; then
            echo "$out" | grep -E "FAIL " | sed 's/^/             /'
        else
            echo "$out" | tail -5 | sed 's/^/             /'
        fi
        fail=$((fail + 1)); failed="$failed $name"
    fi
}

echo "smoke-testing every example"
#   program                 keys that change the screen        quit
run counter           --keys="+-"
run adaptive          --no-input-change
run grid              --no-input-change
run pretty            --no-input-change
run editor_ide        --keys="jk"
run editor_widgets    --keys="lj"
run editor_widgets2   --keys="lj"
run editor_widgets3   --keys="lj"
run editor_workbench  --keys="pbj"
run floating          --keys=" "
run stopwatch         --keys=" l"
run editor_live       --keys="x"                         --quit="$(printf '\021')"
run proc_table        --keys="jsr"
run viewport          --keys="-fj"
run widgets           --keys="$(printf '\t') r"
run agent_stats       --keys="2f-"
run motion_showcase   --keys="c1"                        --animates="5"
run agent             --keys="$(printf '\r') t"              --animates="$(printf '\r')"
run messenger         --keys="xyz"                       --quit="$(printf '\003')"
run agent_session     --keys="xy"                        --quit="$(printf '\003')" --animates="$(printf '\r')"
run terminal_fx            --keys="tcori"

# Canvas-style demos (pixels / glyphs). Ray tracers render every frame by
# design, so their idle budget is the cost of a frame, not zero.
run breakout               --keys="hl"
run snake                  --keys="dw"
run life                   --keys=" r"
run matrix                 --keys="2+"
run particles              --keys="2 "
run sorts                  --keys="2 "
run mandelbrot             --keys="2+"
run doom_fire              --keys="2+"
run doomfire2              --keys="2+"
run fluid                  --keys="2"
run spectrum               --keys="34"
run dashboard              --keys="2w"
run raymarch               --keys="23"                        --idle-cpu=3
run space3d                --keys="bd"                        --idle-cpu=8
run fps                    --keys="wd"                        --idle-cpu=2

# Dashboards and apps.
run chat                   --keys="ab"
run deploy                 --keys="3f"
run hacker                 --keys="2e"
run ide                    --keys="b12"
run markup                 --keys="jj"
run music                  --keys="nj"
run space                  --keys="d2"
run stocks                 --keys="jt"
run sysmon                 --keys="ls"
run scroll_2d              --keys="jj"
run scroll_clip            --keys="jj"
run scroll_slice           --keys="jj"
run scroll_styles          --keys="jj"

# Deeper than the smoke checks: each terminal effect's bytes on the wire,
# and suspend's answer folded back in (jaal D39).
if $PY "$HERE/terminal_fx_test.py" "$BUILD/maya_terminal_fx" >/dev/null; then
    pass=$((pass + 1))
else
    fail=$((fail + 1)); failed="$failed terminal_fx_test"
    echo "terminal_fx_test: FAILED (run it directly for details)"
fi

# Runs to completion by itself (a 3 s inline progress bar): it must exit 0
# and leave the finished card in the scrollback.
if $PY "$HERE/inline_progress_test.py" "$BUILD/maya_inline_progress" >/dev/null 2>&1; then
    echo "  ok       inline_progress"; pass=$((pass + 1))
else echo "  FAIL     inline_progress"; fail=$((fail + 1)); failed="$failed inline_progress"; fi

# The navigation-frame contract: every Down in a key-repeat burst is drawn.
if $PY "$HERE/nav_frames_test.py" "$BUILD/maya_navcheck" >/dev/null 2>&1; then
    echo "  ok       navcheck"; pass=$((pass + 1))
else echo "  FAIL     navcheck"; fail=$((fail + 1)); failed="$failed navcheck"; fi

# Print-only examples (no runtime): they must run to completion.
for p in stat_sheet_demo editor_widget_check; do
    if "$BUILD/maya_$p" >/dev/null 2>&1; then echo "  ok       $p"; pass=$((pass + 1))
    else echo "  FAIL     $p (exit $?)"; fail=$((fail + 1)); failed="$failed $p"; fi
done

echo "$pass passed, $fail failed${failed:+ :$failed}"
[ "$fail" -eq 0 ]
