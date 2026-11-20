#!/bin/sh
# Build the real st.o in a disposable tree; discard unrelated sections at link.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/st-blink-bounds.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
cp "$root"/*.c "$root"/*.h "$root/Makefile" "$root/config.mk" "$work/"
cp -R "$root/patch" "$work/patch"
for reflow in 0 1; do
    python3 - "$work/patches.h" "$reflow" <<'PY'
from pathlib import Path
import re, sys
p = Path(sys.argv[1])
s, n = re.subn(r'(#define REFLOW_PATCH)\s+\d+', r'\g<1> ' + sys.argv[2], p.read_text())
assert n == 1
p.write_text(s)
PY
    rm -f "$work/st.o"
    make -s -C "$work" st.o CFLAGS="${CFLAGS:--O1 -g} -ffunction-sections -fdata-sections"
    ${CC:-cc} ${CFLAGS:--O1 -g} $(pkg-config --cflags pixman-1) -I"$work" "$root/tools/blink_bounds_test.c" \
        "$work/st.o" -Wl,--gc-sections ${LDFLAGS:-} -o "$work/test"
    printf 'REFLOW_PATCH=%s: ' "$reflow"
    "$work/test"
done
