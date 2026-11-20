#!/bin/sh
# Compare the same parser/reflow workload with packed and full-width history.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/st-history-test.XXXXXX")
trap 'if [ "${HISTORY_TEST_KEEP:-0}" = 1 ]; then echo "Test tree: $work"; else rm -rf "$work"; fi' EXIT HUP INT TERM
cp "$root"/*.c "$root"/*.h "$root/Makefile" "$root/config.mk" "$work/"
cp -R "$root/patch" "$work/patch"
mkdir "$work/current"
cp "$root/st.c" "$root/st.h" "$root/wl.c" "$work/current/"
cp -R "$root/patch" "$work/current/patch"
mkdir "$work/tools"
cp "$root/tools/history_test.c" "$work/tools/"
cd "$work"
cp patches.h patches.test-base.h
packages='wayland-client wayland-cursor xkbcommon fontconfig freetype2 pixman-1'
cflags=${CFLAGS:--O1 -g}
ldflags=${LDFLAGS:-}
for config in enabled minimal; do
  modes='raw compact'
  if [ -n "${HISTORY_TEST_REFERENCE:-}" ]; then modes="$modes reference"; fi
  for mode in $modes; do
    source_dir="$work/current"
    if [ "$mode" = reference ]; then source_dir="$HISTORY_TEST_REFERENCE"; fi
    cp "$source_dir/st.c" "$source_dir/st.h" "$source_dir/wl.c" .
    cp -R "$source_dir/patch/." patch/
    python3 - "$config" "$mode" <<'PY'
from pathlib import Path
import re, sys
config, mode = sys.argv[1:]
s = Path('patches.test-base.h').read_text()
for key in ('SIXEL_PATCH', 'UNDERCURL_PATCH', 'KEYBOARDSELECT_PATCH'):
    if config == 'minimal':
        s, n = re.subn(r'(#define ' + key + r')\s+\d+', r'\g<1> 0', s)
        assert n == 1
Path('patches.h').write_text(s)
s = Path('patch/reflow.c').read_text()
if mode == 'raw':
    a = s.index('static int\nthistlength(')
    b = s.index('\nstatic void\nthiststore(', a)
    s = s[:a] + 'static int\nthistlength(Line line, int col)\n{ (void)line; return col; }\n' + s[b:]
Path('patch/reflow.c').write_text(s)
PY
    touch wl.c
    sixel_sources='sixel.c sixel_hls.c'
    sixel_objects='sixel.o sixel_hls.o'
    if [ "$config" = minimal ]; then sixel_sources=''; sixel_objects=''; fi
    make -s -j2 CFLAGS="$cflags" LDFLAGS="$ldflags" SIXEL_C="$sixel_sources" >"build-$config-$mode.log" 2>&1 ||
      { cat "build-$config-$mode.log" >&2; exit 1; }
    ${CC:-cc} $cflags -Dmain=st_program_main -Iprotocols -DVERSION=\"test\" \
      $(pkg-config --cflags $packages) -c wl.c -o wl-test.o
    readview='-DHISTORY_READVIEW'
    if [ "$mode" = reference ]; then readview=''; fi
    reflowcompact=''
    if [ "$mode" = compact ]; then reflowcompact='-DHISTORY_REFLOW_COMPACT'; fi
    ${CC:-cc} $cflags -DHISTORY_COMPACT -DHISTORY_ALLOCATIONS $readview $reflowcompact -Iprotocols -DVERSION=\"test\" \
      $(pkg-config --cflags $packages) tools/history_test.c wl-test.o \
      patch/vec.o $sixel_objects protocols/*.o $ldflags \
      $(pkg-config --libs $packages) -lfcft -lm -lutil -lrt -o history-test
    ./history-test >"$config-$mode.state" 2>"$config-$mode.err" ||
      { cat "$config-$mode.err" >&2; exit 1; }
    printf '%s/%s: ' "$config" "$mode"
    cat "$config-$mode.err"
  done
  cmp "$config-raw.state" "$config-compact.state"
  if [ -n "${HISTORY_TEST_REFERENCE:-}" ]; then
    cmp "$config-reference.state" "$config-compact.state"
  fi
  python3 - "$config" <<'PYTEST'
from pathlib import Path
import re, sys
config = sys.argv[1]
raw, packed = [int(re.search(r'requested=(\d+)', Path(f'{config}-{mode}.err').read_text())[1])
               for mode in ('raw', 'compact')]
assert packed < raw / 2, (raw, packed)
PYTEST
  printf '%s: identical parser, history, image and selection digests; compression saves >50%%\n' "$config"
done
