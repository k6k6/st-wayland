#!/bin/sh
# Headless integration tests; never modifies configuration/build products in source.
# Requires the normal Wayland build dependencies and python3, but no fonts/display.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/st-powerline-test.XXXXXX")
trap 'if [ "${POWERLINE_TEST_KEEP:-0}" = 1 ]; then echo "Test tree: $work"; else rm -rf "$work"; fi' EXIT HUP INT TERM
cp "$root"/*.c "$root"/*.h "$root/Makefile" "$root/config.mk" "$work/"
cp -R "$root/patch" "$work/patch"
mkdir "$work/tools"
cp "$root/tools/powerline_test.c" "$work/tools/"
cd "$work"
cp patches.h patches.test-base.h
# Expose the real static terminal entry point only in this disposable build.
cat >>st.c <<'C'
void powerline_test_setchar(Rune u, const Glyph *g) { tsetchar(u, g, 0, 0); }
C
packages='wayland-client wayland-cursor xkbcommon cairo cairo-ft fontconfig freetype2 harfbuzz'
for variant in ligatures no-ligatures disabled; do
	python3 - "$variant" <<'PY'
from pathlib import Path
import re, sys
v = sys.argv[1]
p = Path('patches.test-base.h').read_text()
for key, value in [('BOXDRAW_PATCH', v != 'disabled'), ('LIGATURES_PATCH', v != 'no-ligatures')]:
    p, n = re.subn(r'(#define ' + key + r')\s+\d+', lambda m: m[1] + ' ' + str(int(value)), p)
    assert n == 1
Path('patches.h').write_text(p)
# Make only the copied config gate mutable to exercise 0 and 1 in one build.
config = Path('config.def.h').read_text().replace('const int boxdraw =', 'int boxdraw =')
Path('config.h').write_text(config)
h = Path('st.h').read_text().replace('extern const int boxdraw, boxdraw_bold, boxdraw_braille;',
    'extern int boxdraw; extern const int boxdraw_bold, boxdraw_braille;')
Path('st.h').write_text(h)
PY
	if ! make -s -j2 CFLAGS='-O1 -g' LDFLAGS='' >"build-$variant.log" 2>&1; then
		cat "build-$variant.log" >&2
		exit 1
	fi
	${CC:-cc} -O1 -g -DVERSION=\"test\" -Iprotocols \
		$(pkg-config --cflags $packages) tools/powerline_test.c \
		st.o hb.o sixel.o sixel_hls.o protocols/*.o \
		$(pkg-config --libs $packages) -lm -lutil -lrt -o powerline-test
	printf '%s: ' "$variant"
	./powerline-test
 done
