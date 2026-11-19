#!/bin/sh
# Real renderer with captured Wayland requests, in a disposable build.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/st-render-state-test.XXXXXX")
trap 'if [ "${RENDER_STATE_TEST_KEEP:-0}" = 1 ]; then echo "Test tree: $work"; else rm -rf "$work"; fi' EXIT HUP INT TERM
cp "$root"/*.c "$root"/*.h "$root/Makefile" "$root/config.mk" "$work/"
cp -R "$root/patch" "$work/patch"
mkdir "$work/tools"
cp "$root/tools/render_state_test.c" "$work/tools/"
cd "$work"
cp patches.h patches.test-base.h
packages='wayland-client wayland-cursor xkbcommon cairo cairo-ft fontconfig freetype2 harfbuzz'
cflags=${CFLAGS:--O1 -g}
ldflags=${LDFLAGS:-}
for variant in enabled no-ligatures no-wide minimal no-reflow; do
	python3 - "$variant" <<'PY'
from pathlib import Path
import re, sys
v = sys.argv[1]
p = Path('patches.test-base.h').read_text()
for key, value in [('FONT2_PATCH', False), ('REFLOW_PATCH', v != 'no-reflow'),
                   ('LIGATURES_PATCH', v not in ('no-ligatures', 'minimal')),
                   ('WIDE_GLYPHS_PATCH', v not in ('no-wide', 'minimal')),
                   ('ANYSIZE_PATCH', v != 'minimal'), ('ALPHA_PATCH', v != 'minimal')]:
    p, n = re.subn(r'(#define ' + key + r')\s+\d+', lambda m: m[1] + ' ' + str(int(value)), p)
    assert n == 1
Path('patches.h').write_text(p)
Path('config.h').write_text(Path('config.def.h').read_text())
PY
	if ! make -s -j2 CFLAGS="$cflags" LDFLAGS="$ldflags" >"build-$variant.log" 2>&1; then
		cat "build-$variant.log" >&2
		exit 1
	fi
	${CC:-cc} $cflags -finstrument-functions -DVERSION=\"test\" -Iprotocols \
		$(pkg-config --cflags $packages) tools/render_state_test.c \
		st.o hb.o sixel.o sixel_hls.o protocols/*.o \
		$ldflags $(pkg-config --libs $packages) -lm -lutil -lrt -o render-state-test
	printf '%s: ' "$variant"
	./render-state-test
done
