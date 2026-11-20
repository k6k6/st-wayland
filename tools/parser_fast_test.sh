#!/bin/sh
# Compare fast ASCII dispatch with ordinary UTF-8/tputc dispatch in the real
# parser. All builds and printer capture files remain in a disposable tree.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/st-parser-fast-test.XXXXXX")
trap 'if [ "${PARSER_TEST_KEEP:-0}" = 1 ]; then echo "Test tree: $work"; else rm -rf "$work"; fi' EXIT HUP INT TERM
cp "$root"/*.c "$root"/*.h "$root/Makefile" "$root/config.mk" "$work/"
cp -R "$root/patch" "$work/patch"
mkdir "$work/tools"
cp "$root/tools/parser_fast_test.c" "$work/tools/"
if [ -n "${PARSER_TEST_BASELINE:-}" ]; then
  cp "$PARSER_TEST_BASELINE" "$work/st.reference.c"
  # Parser state also lives in included patch sources. Keep those from the
  # same frozen revision while using this test's configuration and ABI.
  cp -R "$(dirname -- "$PARSER_TEST_BASELINE")/patch" "$work/reference-patch"
  python3 - "$work/st.reference.c" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
p.write_text(p.read_text().replace('#include "patch/', '#include "reference-patch/'))
PY
fi
cd "$work"
cp patches.h patches.test-base.h
packages='wayland-client wayland-cursor xkbcommon fontconfig freetype2 pixman-1'
cflags=${CFLAGS:--O2 -g}
ldflags=${LDFLAGS:-}
for variant in enabled no-reflow minimal sync; do
  python3 - "$variant" <<'PY'
from pathlib import Path
import re, sys
v = sys.argv[1]
s = Path('patches.test-base.h').read_text()
for key, value in [('REFLOW_PATCH', v != 'no-reflow'), ('SYNC_PATCH', v == 'sync'),
                   ('OSC133_PATCH', v == 'sync')]:
    s, n = re.subn(r'(#define ' + key + r')\s+\d+', lambda m: m[1] + ' ' + str(int(value)), s)
    assert n == 1
if v == 'minimal':
    for key in ('SIXEL_PATCH', 'UNDERCURL_PATCH', 'BOXDRAW_PATCH', 'KEYBOARDSELECT_PATCH'):
        s, n = re.subn(r'(#define ' + key + r')\s+\d+', lambda m: m[1] + ' 0', s)
        assert n == 1
Path('patches.h').write_text(s)
PY
  sixel_sources='sixel.c sixel_hls.c'; sixel_objects='sixel.o sixel_hls.o'
  if [ "$variant" = minimal ]; then sixel_sources=''; sixel_objects=''; fi
  make -s -j2 CFLAGS="$cflags" LDFLAGS="$ldflags" SIXEL_C="$sixel_sources" >"build-$variant.log" 2>&1 ||
    { cat "build-$variant.log" >&2; exit 1; }
  ${CC:-cc} $cflags -Dmain=st_program_main -Iprotocols -DVERSION=\"test\" \
    $(pkg-config --cflags $packages) -c wl.c -o wl-test.o
  modes='reference fast'
  if [ -f st.reference.c ]; then modes="$modes baseline"; fi
  for mode in $modes; do
    define=''
    case "$mode" in
      reference) define='-DPARSER_REFERENCE';;
      baseline) define='-DPARSER_BASELINE';;
    esac
    ${CC:-cc} $cflags $define -Iprotocols -DVERSION=\"test\" \
      $(pkg-config --cflags $packages) tools/parser_fast_test.c wl-test.o \
      patch/vec.o $sixel_objects protocols/*.o $ldflags \
      $(pkg-config --libs $packages) -lfcft -lm -lutil -lrt -o parser-$mode
    ./parser-$mode >"$variant-$mode.state" 2>"$variant-$mode.err" ||
      { cat "$variant-$mode.err" >&2; exit 1; }
  done
  cmp "$variant-reference.state" "$variant-fast.state"
  cmp "$variant-reference.err" "$variant-fast.err"
  if [ -f st.reference.c ]; then
    cmp "$variant-baseline.state" "$variant-fast.state"
    cmp "$variant-baseline.err" "$variant-fast.err"
  fi
  printf '%s: identical parser, cursor, selection, history, SIXEL and printer state\n' "$variant"
  if [ "${PARSER_TEST_BENCH:-0}" = 1 ] && [ "$variant" = enabled ]; then
    for mode in reference fast fast reference; do
      printf '%s: ' "$mode"
      ./parser-$mode bench
    done
  fi
done
