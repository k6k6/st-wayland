#!/bin/sh
# Run from any directory. Build products stay outside the source tree.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/st-shape-cache-test.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
packages='cairo cairo-ft fontconfig freetype2 harfbuzz'
for budget in 8388608 1024; do
	${CC:-cc} ${CFLAGS:--O1 -g} -DSHAPECACHE_MAXBYTES="$budget" \
		$(pkg-config --cflags $packages) "$root/tools/shape_cache_test.c" \
		$(pkg-config --libs $packages) ${LDFLAGS:-} -lm -o "$work/test"
	"$work/test"
done
