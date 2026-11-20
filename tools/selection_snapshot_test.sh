#!/bin/sh
# No display, compositor, fonts or pty needed. Test the real wl.c callbacks with
# nonblocking pipes, in both automatic and independent clipboard configurations.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/st-selection-snapshot-test.XXXXXX")
trap 'if [ "${SELECTION_SNAPSHOT_TEST_KEEP:-0}" = 1 ]; then echo "Test tree: $work"; else rm -rf "$work"; fi' EXIT HUP INT TERM
cp "$root"/*.c "$root"/*.h "$root/Makefile" "$root/config.mk" "$work/"
cp -R "$root/patch" "$work/patch"
mkdir "$work/tools"
cp "$root/tools/selection_snapshot_test.c" "$work/tools/"
cd "$work"
cp patches.h patches.test-base.h
packages='wayland-client wayland-cursor xkbcommon fontconfig freetype2 pixman-1'
cflags=${CFLAGS:--O1 -g}
ldflags=${LDFLAGS:-}
for clipboard in 0 1; do
	python3 - "$clipboard" <<'PY'
from pathlib import Path
import re, sys
p = Path('patches.test-base.h').read_text()
# Compile the char * consumers too, to guard compatibility of the borrowed views.
for key, value in {'CLIPBOARD_PATCH': sys.argv[1],
                   'OPENCOPIED_PATCH': '1', 'RIGHTCLICKTOPLUMB_PATCH': '1'}.items():
    p, n = re.subn(r'(#define ' + key + r')\s+\d+',
                   lambda m: m[1] + ' ' + value, p)
    assert n == 1, key
Path('patches.h').write_text(p)
PY
	if ! make -s -j2 CFLAGS="$cflags" LDFLAGS="$ldflags" >"build-$clipboard.log" 2>&1; then
		cat "build-$clipboard.log" >&2
		exit 1
	fi
	${CC:-cc} $cflags -DVERSION=\"test\" -Iprotocols \
		$(pkg-config --cflags $packages) tools/selection_snapshot_test.c \
		st.o patch/vec.o sixel.o sixel_hls.o protocols/*.o \
		$ldflags -Wl,--wrap=free -Wl,--wrap=write \
		$(pkg-config --libs $packages) -lfcft -lm -lutil -lrt -o selection-snapshot-test
	printf 'CLIPBOARD_PATCH=%s:\n' "$clipboard"
	./selection-snapshot-test
done
