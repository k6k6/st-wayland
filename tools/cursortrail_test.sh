#!/bin/sh
# Test the actual Wayland renderer in a disposable build, without a display/pty.
# Requires normal build dependencies, python3 and DejaVu Sans Mono.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/st-cursortrail-test.XXXXXX")
trap 'if [ "${CURSORTRAIL_TEST_KEEP:-0}" = 1 ]; then echo "Test tree: $work"; else rm -rf "$work"; fi' EXIT HUP INT TERM
cp "$root"/*.c "$root"/*.h "$root/Makefile" "$root/config.mk" "$work/"
cp -R "$root/patch" "$work/patch"
mkdir "$work/tools"
cp "$root/tools/cursortrail_test.c" "$work/tools/"
cd "$work"
cp patches.h patches.test-base.h
packages='wayland-client wayland-cursor xkbcommon cairo cairo-ft fontconfig freetype2 harfbuzz'
cflags=${CFLAGS:--O1 -g}
ldflags=${LDFLAGS:-}
for variant in enabled minimal blink-dynamic-sync opaque disabled; do
	python3 - "$variant" <<'PY'
from pathlib import Path
import re, sys
v = sys.argv[1]
p = Path('patches.test-base.h').read_text()
settings = {
    'CURSOR_TRAIL_PATCH': v != 'disabled',
    'FONT2_PATCH': False,
    'LIGATURES_PATCH': v != 'minimal',
    'WIDE_GLYPHS_PATCH': v != 'minimal',
    'BLINKING_CURSOR_PATCH': v == 'blink-dynamic-sync',
    'DYNAMIC_CURSOR_COLOR_PATCH': v == 'blink-dynamic-sync',
    'SYNC_PATCH': v == 'blink-dynamic-sync',
    'ALPHA_PATCH': v != 'opaque',
    'ANYSIZE_PATCH': v != 'opaque',
}
for key, value in settings.items():
    p, n = re.subn(r'(#define ' + key + r')\s+\d+',
                   lambda m: m[1] + ' ' + str(int(value)), p)
    assert n == 1, key
Path('patches.h').write_text(p)
Path('config.h').write_text(Path('config.def.h').read_text())
PY
	if ! make -s -j2 CFLAGS="$cflags" LDFLAGS="$ldflags" >"build-$variant.log" 2>&1; then
		cat "build-$variant.log" >&2
		exit 1
	fi
	${CC:-cc} $cflags -DVERSION=\"test\" -Iprotocols \
		$(pkg-config --cflags $packages) tools/cursortrail_test.c \
		st.o hb.o sixel.o sixel_hls.o protocols/*.o \
		$ldflags $(pkg-config --libs $packages) -lm -lutil -lrt -o cursortrail-test
	printf '%s: ' "$variant"
	./cursortrail-test
done
