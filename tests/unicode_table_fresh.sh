#!/bin/sh
# tests/unicode_table_fresh.sh — the generated width table matches its source.
#
# include/maya/text/unicode_width_table.hpp is generated from the UCD files
# pinned under data/. Nothing stops someone editing the header by hand, or
# updating data/ and forgetting to regenerate — and the table is exactly the
# kind of file nobody reads in review, so a wrong entry can live for a long
# time. agentty#55 was one: Korean measured 4 columns per syllable instead of
# 2, and the bug outlived several releases.
#
# This regenerates into a temp dir and diffs. No build needed.
#
#   sh tests/unicode_table_fresh.sh

cd "$(dirname "$0")/.." || exit 1

HEADER="include/maya/text/unicode_width_table.hpp"

PY=$(command -v python3 || command -v python) || {
    echo "  SKIP  no python interpreter; cannot verify the generated table"
    exit 0
}

[ -f "$HEADER" ] || { echo "  FAIL  $HEADER is missing"; exit 1; }

# Regenerate into a scratch copy of the tree so the real header is never
# touched: a test that rewrites the source it is checking can't fail twice.
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT INT TERM

mkdir -p "$tmp/scripts" "$tmp/data" "$tmp/include/maya/text"
cp scripts/gen_unicode_width.py "$tmp/scripts/" || exit 1
cp data/EastAsianWidth.txt data/emoji-data.txt data/UnicodeData.txt \
   "$tmp/data/" 2>/dev/null || {
    echo "  SKIP  UCD sources not present under data/"
    exit 0
}

if ! "$PY" "$tmp/scripts/gen_unicode_width.py" >/dev/null 2>"$tmp/err"; then
    echo "  FAIL  the generator does not run:"
    sed 's/^/          /' "$tmp/err"
    exit 1
fi

if diff -u "$HEADER" "$tmp/$HEADER" > "$tmp/diff" 2>/dev/null; then
    echo "  ok    the width table matches data/ + gen_unicode_width.py"
    echo "unicode_table_fresh: ok"
    exit 0
fi

echo "  FAIL  $HEADER is stale or hand-edited."
echo "        Regenerate with: python3 scripts/gen_unicode_width.py"
echo "        First 40 lines of the difference:"
head -40 "$tmp/diff" | sed 's/^/          /'
echo "unicode_table_fresh: FAILED"
exit 1
