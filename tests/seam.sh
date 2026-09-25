#!/bin/sh
# tests/seam.sh — the layering rule, as a test.
#
# maya is the view layer and the terminal device; jaal is the runtime;
# include/maya/host/ is where they meet. The rule that makes "maya is to jaal
# what Ink is to React" a fact instead of a slogan:
#
#   NOTHING outside include/maya/host/ may depend on jaal.
#
# A prose mention in a comment is fine — that's how a reader finds the seam.
# An #include or a jaal:: name is not. If this fails, someone reached across
# the seam, and the view layer just stopped being usable without a runtime.
#
#   sh tests/seam.sh
#
# Runs in CI. No build needed: it reads the tree.

cd "$(dirname "$0")/.." || exit 1
status=0

fail() { echo "  FAIL  $1"; status=1; }

# 1. The compiled view layer knows nothing of jaal, so libmaya.a links
#    without it and a non-terminal host could reuse the whole renderer.
hits=$(grep -rl 'jaal' src/ 2>/dev/null)
if [ -n "$hits" ]; then
    fail "src/ mentions jaal:"
    echo "$hits" | sed 's/^/          /'
else
    echo "  ok    src/ is jaal-free"
fi

# 2. Headers outside the seam may NAME jaal in prose, but must not include
#    it or use its types.
leaks=""
for f in $(grep -rl 'jaal' include/maya --include='*.hpp' 2>/dev/null); do
    case "$f" in
        include/maya/host/*) continue ;;      # the seam itself
    esac
    # Strip // comments, then look for real dependencies.
    if sed 's://.*::' "$f" | grep -qE '#[[:space:]]*include[[:space:]]*<jaal|jaal::'; then
        leaks="$leaks $f"
    fi
done
if [ -n "$leaks" ]; then
    fail "these headers depend on jaal outside the seam:"
    for f in $leaks; do echo "          $f"; done
else
    echo "  ok    only maya/host/ depends on jaal"
fi

# 3. The seam is where we say it is.
for f in interop effects sources device terminal run; do
    [ -f "include/maya/host/$f.hpp" ] || fail "missing include/maya/host/$f.hpp"
done
[ $status -eq 0 ] && echo "  ok    the seam is six files"

# 4. maya has no runtime of its own. `Runtime` was the terminal DEVICE, a
#    name that contradicted the whole design; it is `Device` now.
if grep -rn 'detail::Runtime' include/ src/ 2>/dev/null | grep -qv '^Binary'; then
    fail "detail::Runtime is back — the device is not the runtime (jaal is)"
else
    echo "  ok    no 'Runtime' in maya: jaal is the runtime"
fi

# 5. "app" is what someone WRITES with maya, not a layer inside it. The
#    layers are the device, the renderer, the view layer and the host; a
#    directory called app/ means one of them lost its name again.
if [ -d include/maya/app ] || [ -d src/app ]; then
    fail "an app/ directory is back — name the layer, not the product"
else
    echo "  ok    no app/ layer: device, renderer, view, host"
fi

[ $status -eq 0 ] && echo "seam: ok" || echo "seam: FAILED"
exit $status
