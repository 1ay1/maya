#!/bin/sh
# tools/build_one.sh NAME : compile examples/jaal_NAME.cpp alone, to /tmp/jaalbin/NAME.
#
# Links against build-jaal's prebuilt libmaya.a + libjaal.a, so several
# programs can be built at once without touching the shared build tree
# (ninja can't run twice in one build dir). Same flags as the CMake build.
set -e
NAME=$1
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDK=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26.4.sdk
mkdir -p /tmp/jaalbin
c++ -DMAYA_WITH_JAAL=1 -I"$ROOT/include" -I"$ROOT/third_party/jaal/include" \
    -O3 -DNDEBUG -std=c++2c -arch arm64 -isysroot "$SDK" \
    "$ROOT/examples/jaal_$NAME.cpp" \
    "$ROOT/build-jaal/libmaya.a" "$ROOT/build-jaal/third_party/jaal/libjaal.a" \
    -o "/tmp/jaalbin/$NAME"
echo "built /tmp/jaalbin/$NAME"
