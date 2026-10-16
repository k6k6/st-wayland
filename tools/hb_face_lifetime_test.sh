#!/bin/sh
# Run from any directory. Build products stay outside the source tree.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/st-hb-face-lifetime-test.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
packages='cairo cairo-ft fontconfig freetype2 harfbuzz xkbcommon'
${CC:-cc} ${CFLAGS:--O1 -g} $(pkg-config --cflags $packages) \
	"$root/tools/hb_face_lifetime_test.c" \
	$(pkg-config --libs $packages) ${LDFLAGS:-} -lm -o "$work/test"
"$work/test"
