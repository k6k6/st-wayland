#!/bin/sh
# Normal build dependencies plus python3; no fonts, Wayland display or pty needed.
# Builds the actual application and harness in a disposable tree for each variant.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/st-fullscreen-alpha-test.XXXXXX")
trap 'if [ "${FULLSCREEN_ALPHA_TEST_KEEP:-0}" = 1 ]; then echo "Test tree: $work"; else rm -rf "$work"; fi' EXIT HUP INT TERM
cp "$root"/*.c "$root"/*.h "$root/Makefile" "$root/config.mk" "$work/"
cp -R "$root/patch" "$work/patch"
mkdir "$work/tools"
cp "$root/tools/fullscreen_alpha_test.c" "$work/tools/"
cd "$work"
cp patches.h patches.test-base.h
packages='wayland-client wayland-cursor xkbcommon cairo cairo-ft fontconfig freetype2 harfbuzz'
cflags=${CFLAGS:--O1 -g}
ldflags=${LDFLAGS:-}
for variant in alpha focus selection focus-selection gradient focus-selection-gradient disabled; do
	python3 - "$variant" <<'PY'
from pathlib import Path
import re, sys
v = sys.argv[1]
p = Path('patches.test-base.h').read_text()
settings = {
    'ALPHA_PATCH': v != 'disabled',
    'ALPHA_FOCUS_HIGHLIGHT_PATCH': 'focus' in v,
    'ALPHA_GRADIENT_PATCH': 'gradient' in v,
    'SELECTION_COLORS_PATCH': 'selection' in v,
    'SELECTIONBG_ALPHA_PATCH': 'selection' in v,
    # Split background drawing permits pixels to be tested without any fonts.
    'WIDE_GLYPHS_PATCH': True,
    'LIGATURES_PATCH': True,
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
	# Exclude wl.o: the test includes the real wl.c with its main renamed.
	${CC:-cc} $cflags -DVERSION=\"test\" -Iprotocols \
		$(pkg-config --cflags $packages) tools/fullscreen_alpha_test.c \
		st.o hb.o sixel.o sixel_hls.o protocols/*.o \
		$ldflags $(pkg-config --libs $packages) -lm -lutil -lrt -o fullscreen-alpha-test
	printf '%s: ' "$variant"
	./fullscreen-alpha-test
done
