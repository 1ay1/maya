#!/bin/sh
# tools/build_one.sh NAME : compile examples/NAME.cpp alone, to /tmp/mayabin/NAME.
#
# Links against build-app's prebuilt libmaya.a + libjaal.a, so several
# programs can be built at once without touching the shared build tree
# (ninja can't run twice in one build dir). Same flags as the CMake build.
set -e
NAME=$1
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDK=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26.4.sdk
mkdir -p /tmp/mayabin
c++ -I"$ROOT/include" -I"$ROOT/third_party/jaal/include" \
    -O3 -DNDEBUG -std=c++2c -arch arm64 -isysroot "$SDK" \
    "$ROOT/examples/$NAME.cpp" \
    "$ROOT/build-app/libmaya.a" "$ROOT/build-app/third_party/jaal/libjaal.a" \
    -o "/tmp/mayabin/$NAME"
echo "built /tmp/mayabin/$NAME"
