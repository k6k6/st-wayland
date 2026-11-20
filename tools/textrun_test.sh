#!/bin/sh
# Uses fcft's public headers directly: some installations omit the private
# tllist pkg-config dependency even though dynamic linking works correctly.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/st-textrun-test.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
for budget in 8388608 1024; do
	${CC:-cc} ${CFLAGS:--O1 -g} -DTEXTRUN_MAXBYTES="$budget" \
		$(pkg-config --cflags pixman-1) "$root/tools/textrun_test.c" \
		-lfcft $(pkg-config --libs pixman-1) ${LDFLAGS:-} -o "$work/test"
	"$work/test"
done
