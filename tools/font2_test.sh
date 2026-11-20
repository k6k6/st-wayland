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
if [ -n "${FONT2_TEST_BASELINE:-}" ]; then
	mkdir "$work/baseline"
	cp "$FONT2_TEST_BASELINE"/*.c "$FONT2_TEST_BASELINE"/*.h \
		"$FONT2_TEST_BASELINE/Makefile" "$FONT2_TEST_BASELINE/config.mk" "$work/baseline/"
	cp -R "$FONT2_TEST_BASELINE/patch" "$work/baseline/patch"
	mkdir "$work/baseline/tools"
	cp "$root/tools/font2_test.c" "$work/baseline/tools/"
fi
cd "$work"
cp patches.h patches.test-base.h
packages='wayland-client wayland-cursor xkbcommon fontconfig freetype2 pixman-1'
cflags="${CFLAGS:--O1 -g} -DFCFT_CACHE_MAXGLYPHS=512"
ldflags=${LDFLAGS:-}

for variant in off empty multi multi-no-ligatures no-bold no-italic no-styles; do
	python3 - "$variant" <<'PY'
from pathlib import Path
import re
import sys
variant = sys.argv[1]
patches = Path('patches.test-base.h').read_text()
patches = re.sub(r'(#define FONT2_PATCH)\s+\d+', r'\g<1> ' + ('0' if variant == 'off' else '1'), patches)
patches = re.sub(r'(#define LIGATURES_PATCH)\s+\d+', r'\g<1> ' + ('0' if variant == 'multi-no-ligatures' else '1'), patches)
for name, disabled in [('BOLD', variant in ('no-bold', 'no-styles')),
                       ('ITALIC', variant in ('no-italic', 'no-styles')),
                       ('ROMAN', variant == 'no-styles')]:
    patches = re.sub(r'(#define DISABLE_' + name + r'_FONTS_PATCH)\s+\d+',
                     lambda m: m[1] + ' ' + str(int(disabled)), patches)
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
	if ! make -s -j2 CFLAGS="$cflags" LDFLAGS="$ldflags" >"build-$variant.log" 2>&1; then
		cat "build-$variant.log" >&2
		exit 1
	fi
	# Deliberately exclude wl.o: the harness includes the real wl.c itself.
	${CC:-cc} $cflags -DVERSION=\"test\" -Iprotocols \
		$(pkg-config --cflags $packages) tools/font2_test.c \
		st.o patch/vec.o sixel.o sixel_hls.o protocols/*.o \
		$ldflags $(pkg-config --libs $packages) -lfcft -lm -lutil -lrt -o font2-test
	printf '%s: ' "$variant"
	./font2-test
	if [ -n "${FONT2_TEST_BASELINE:-}" ]; then
		./font2-test --pixels >"pixels-$variant.bin"
		cp patches.h config.h baseline/
		(
			cd baseline
			make -s -j2 CFLAGS="$cflags" LDFLAGS="$ldflags" >"build-$variant.log" 2>&1
			${CC:-cc} $cflags -DVERSION=\"test\" -Iprotocols \
				$(pkg-config --cflags $packages) tools/font2_test.c \
				st.o patch/vec.o sixel.o sixel_hls.o protocols/*.o \
				$ldflags $(pkg-config --libs $packages) -lfcft -lm -lutil -lrt -o font2-test
			./font2-test --pixels >"pixels-$variant.bin"
		)
		cmp "pixels-$variant.bin" "baseline/pixels-$variant.bin"
		printf '%s: frozen eager-build pixels match\n' "$variant"
	fi
done
