#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/st-vec-test.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
${CC:-cc} ${CFLAGS:--O1 -g} -DVEC_CACHE_TEST $(pkg-config --cflags pixman-1 freetype2) \
    "$root/tools/vec_test.c" "$root/patch/vec.c" \
    $(pkg-config --libs pixman-1 freetype2) ${LDFLAGS:-} -lm -o "$work/test"
"$work/test" "$@"
