#!/bin/sh
# Font/display-independent tests; all configuration/build changes are disposable.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/st-box-geometry-test.XXXXXX")
trap 'if [ "${BOX_GEOMETRY_TEST_KEEP:-0}" = 1 ]; then echo "Test tree: $work"; else rm -rf "$work"; fi' EXIT HUP INT TERM
cp "$root"/*.c "$root"/*.h "$root/Makefile" "$root/config.mk" "$work/"
cp -R "$root/patch" "$work/patch"
mkdir "$work/tools"
cp "$root/tools/box_geometry_test.c" "$root/tools/box_geometry_inventory.h" \
   "$root/tools/box_geometry_extra_test.h" "$work/tools/"
cd "$work"
cp patches.h patches.test-base.h
cat >>st.c <<'C'
void box_geometry_test_setchar(Rune u, const Glyph *g) { tsetchar(u, g, 0, 0); }
C
packages='wayland-client wayland-cursor xkbcommon cairo cairo-ft fontconfig freetype2 harfbuzz'
test_cflags=${BOX_GEOMETRY_TEST_CFLAGS:--O1 -g}
test_ldflags=${BOX_GEOMETRY_TEST_LDFLAGS:-}
for variant in ligatures no-ligatures no-branch disabled; do
	python3 - "$variant" <<'PY'
from pathlib import Path
import re, sys
v = sys.argv[1]
p = Path('patches.test-base.h').read_text()
for key, value in [('BOXDRAW_PATCH', v != 'disabled'), ('LIGATURES_PATCH', v != 'no-ligatures')]:
    p, n = re.subn(r'(#define ' + key + r')\s+\d+', lambda m: m[1] + ' ' + str(int(value)), p)
    assert n == 1
Path('patches.h').write_text(p)
config = Path('config.def.h').read_text().replace('const int boxdraw =', 'int boxdraw =')
config = config.replace('const int boxdraw_branch =', 'int boxdraw_branch =')
if v == 'no-branch': config = config.replace('int boxdraw_branch = 1;', 'int boxdraw_branch = 0;')
Path('config.h').write_text(config)
p = Path('st.h')
p.write_text(p.read_text().replace('extern const int boxdraw, boxdraw_bold, boxdraw_braille;',
    'extern int boxdraw; extern const int boxdraw_bold, boxdraw_braille;').replace(
    'extern const int boxdraw_branch;', 'extern int boxdraw_branch;'))
PY
	if ! make -s -j2 CFLAGS="$test_cflags" LDFLAGS="$test_ldflags" >"build-$variant.log" 2>&1; then
		cat "build-$variant.log" >&2
		exit 1
	fi
	${CC:-cc} $test_cflags $test_ldflags -DVERSION=\"test\" -Iprotocols \
		$(pkg-config --cflags $packages) tools/box_geometry_test.c \
		st.o hb.o sixel.o sixel_hls.o protocols/*.o \
		$(pkg-config --libs $packages) -lm -lutil -lrt -o box-geometry-test
	printf '%s: ' "$variant"
	./box-geometry-test
done
