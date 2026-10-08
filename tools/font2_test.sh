#!/bin/sh
# Requires build dependencies, python3 and the DejaVu Sans/Serif/Mono fonts.
# All configuration changes and build products stay in a temporary tree.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/st-font2-test.XXXXXX")
trap 'if [ "${FONT2_TEST_KEEP:-0}" = 1 ]; then echo "Test tree: $work"; else rm -rf "$work"; fi' EXIT HUP INT TERM

for family in 'DejaVu Sans' 'DejaVu Serif' 'DejaVu Sans Mono'; do
	if [ "$(fc-match -f '%{family}' "$family")" != "$family" ]; then
		echo "Required test font not found: $family" >&2
		exit 1
	fi
done
cp "$root"/*.c "$root"/*.h "$root/Makefile" "$root/config.mk" "$work/"
cp -R "$root/patch" "$work/patch"
mkdir "$work/tools"
cp "$root/tools/font2_test.c" "$work/tools/"
cd "$work"
cp patches.h patches.test-base.h
packages='wayland-client wayland-cursor xkbcommon cairo cairo-ft fontconfig freetype2 harfbuzz'

for variant in off empty multi multi-no-ligatures; do
	python3 - "$variant" <<'PY'
from pathlib import Path
import re
import sys
variant = sys.argv[1]
patches = Path('patches.test-base.h').read_text()
patches = re.sub(r'(#define FONT2_PATCH)\s+\d+', r'\g<1> ' + ('0' if variant == 'off' else '1'), patches)
patches = re.sub(r'(#define LIGATURES_PATCH)\s+\d+', r'\g<1> ' + ('0' if variant == 'multi-no-ligatures' else '1'), patches)
Path('patches.h').write_text(patches)
config = Path('config.def.h').read_text()
fonts = '' if variant in ('off', 'empty') else '''
    "DejaVu Sans:pixelsize=10",
    "DejaVu Serif:size=12",
    "DejaVu Sans:size=9:dpi=144",
    "DejaVu Sans",
'''
config, count = re.subn(r'(static char \*font2\[\] = \{).*?(\};)', lambda m: m[1] + fonts + m[2], config, count=1, flags=re.S)
assert count == 1
Path('config.h').write_text(config)
PY
	if ! make -s -j2 CFLAGS='-O1 -g' LDFLAGS='' >"build-$variant.log" 2>&1; then
		cat "build-$variant.log" >&2
		exit 1
	fi
	# Deliberately exclude wl.o: the harness includes the real wl.c itself.
	${CC:-cc} -O1 -g -DVERSION=\"test\" -Iprotocols \
		$(pkg-config --cflags $packages) tools/font2_test.c \
		st.o hb.o sixel.o sixel_hls.o protocols/*.o \
		$(pkg-config --libs $packages) -lm -lutil -lrt -o font2-test
	printf '%s: ' "$variant"
	./font2-test
done
