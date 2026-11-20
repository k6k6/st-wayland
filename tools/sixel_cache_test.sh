#!/bin/sh
# Real SIXEL parser and pixman cache, no Wayland display or fonts needed.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/st-sixel-cache-test.XXXXXX")
trap 'if [ "${SIXEL_CACHE_TEST_KEEP:-0}" = 1 ]; then echo "Test tree: $work"; else rm -rf "$work"; fi' EXIT HUP INT TERM
cp "$root"/*.c "$root"/*.h "$root/Makefile" "$root/config.mk" "$work/"
cp -R "$root/patch" "$work/patch"
mkdir "$work/tools"
cp "$root/tools/sixel_cache_test.c" "$work/tools/"
cd "$work"
cp patches.h patches.test-base.h
packages='wayland-client wayland-cursor xkbcommon fontconfig freetype2 pixman-1'
cflags=${CFLAGS:--O1 -g}
ldflags=${LDFLAGS:-}
for variant in enabled no-anysize disabled; do
	python3 - "$variant" <<'PY'
from pathlib import Path
import re, sys
v = sys.argv[1]
p = Path('patches.test-base.h').read_text()
for key, value in [('SIXEL_PATCH', v != 'disabled'), ('ANYSIZE_PATCH', v != 'no-anysize')]:
    p, n = re.subn(r'(#define ' + key + r')\s+\d+', lambda m: m[1] + ' ' + str(int(value)), p)
    assert n == 1
Path('patches.h').write_text(p)
PY
	sixel_sources='sixel.c sixel_hls.c'
	sixel_objects='sixel.o sixel_hls.o'
	if [ "$variant" = disabled ]; then
		sixel_sources=
		sixel_objects=
	fi
	if ! make -s -j2 SIXEL_C="$sixel_sources" CFLAGS="$cflags" LDFLAGS="$ldflags" >"build-$variant.log" 2>&1; then
		cat "build-$variant.log" >&2
		exit 1
	fi
	${CC:-cc} $cflags -DVERSION=\"test\" -Iprotocols \
		$(pkg-config --cflags $packages) tools/sixel_cache_test.c \
		st.o patch/vec.o $sixel_objects protocols/*.o \
		$ldflags $(pkg-config --libs $packages) -lfcft -lm -lutil -lrt -o sixel-cache-test
	printf '%s: ' "$variant"
	./sixel-cache-test
done
