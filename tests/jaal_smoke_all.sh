#!/bin/sh
# tests/jaal_smoke_all.sh — run jaal_smoke.py over every maya-on-jaal example.
#
# One line per program, and a non-zero exit if any fails. Each entry names
# the keys that visibly change that program's screen (--keys), whether it
# reacts to keys at all (--no-input-change), and its quit key.
#
#   sh tests/jaal_smoke_all.sh [build-dir]
#
# Add a line here when an example is ported: an example that isn't in this
# list isn't being checked.
#
# Always write --keys="..." (with =). A key string starting with '-' (a
# program whose "zoom out" is '-') is otherwise parsed as a flag, and the
# program "fails" for a reason that has nothing to do with it.

BUILD=${1:-build-jaal}
HERE=$(dirname "$0")
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
    if out=$(python3 "$HERE/jaal_smoke.py" "$bin" "$@" 2>&1); then
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

echo "maya on jaal: smoke-testing every ported example"
#   program                 keys that change the screen        quit
run jaal_counter           --keys="+-"
run jaal_basic             --keys="+-r"
run jaal_adaptive          --no-input-change
run jaal_grid              --no-input-change
run jaal_pretty            --no-input-change
run jaal_editor_ide        --keys="jk"
run jaal_editor_widgets    --keys="lj"
run jaal_editor_widgets2   --keys="lj"
run jaal_editor_widgets3   --keys="lj"
run jaal_editor_workbench  --keys="pbj"
run jaal_floating          --keys=" "
run jaal_stopwatch         --keys=" l"
run jaal_editor_live       --keys="x"                         --quit="$(printf '\021')"
run jaal_proc_table        --keys="jsr"
run jaal_viewport          --keys="-fj"
run jaal_widgets           --keys="$(printf '\t') r"
run jaal_agent_stats       --keys="2f-"
run jaal_motion_showcase   --keys="c1"                        --animates="5"

echo "$pass passed, $fail failed${failed:+ :$failed}"
[ "$fail" -eq 0 ]
