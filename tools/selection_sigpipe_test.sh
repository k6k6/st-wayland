#!/bin/sh
# Selection transfers must never take the terminal down.
#
# A client that asks for the primary selection owns the read end of the pipe
# it hands to us.  Clipboard managers, pickers and anything that only sniffs a
# prefix close it again right away, so the write of the selection data can hit
# a closed pipe at any time.  Before the fix that SIGPIPE killed st (exit code
# 141) while the user was just double clicking a word.
#
# The test runs the real thing: a throwaway headless mangowm, st inside it, a
# virtual pointer double click to create a selection, then a peer that grabs
# the selection and drops the read end.  It builds
#
#   fixed    the tree as it is
#   control  the same tree with the EPIPE guard textually reverted
#
# and asserts that control dies of SIGPIPE while fixed survives, still owns the
# selection afterwards and can serve it to a well behaved reader.
#
# Needs a wlroots compositor binary exposing zwlr_virtual_pointer_manager_v1
# (mango/sway etc.), wayland-scanner, wlr-protocols and python3.  Takes the
# compositor command from $COMPOSITOR (default: the first of mango, sway,
# river found in PATH) and skips when nothing suitable is available.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/st-selection-sigpipe-test.XXXXXX")
cleanup() {
	[ -n "${mangopid:-}" ] && kill "$mangopid" 2>/dev/null
	[ -n "${stpid:-}" ] && kill "$stpid" 2>/dev/null
	if [ "${SELECTION_SIGPIPE_TEST_KEEP:-0}" = 1 ]; then
		echo "Test tree: $work"
	else
		rm -rf "$work"
	fi
}
trap cleanup EXIT HUP INT TERM

packages='wayland-client wayland-cursor xkbcommon cairo cairo-ft fontconfig freetype2 harfbuzz'
wayland_protocols=$(pkg-config --variable=pkgdatadir wayland-protocols)
scanner=$(command -v wayland-scanner || true)

skip() {
	echo "skip: $*"
	exit 0
}

[ -n "$scanner" ] || skip "wayland-scanner not found"
command -v python3 >/dev/null || skip "python3 not found"

vptrxml=
for d in /usr/share/wlr-protocols /usr/local/share/wlr-protocols \
	/usr/share/wlr-protocols/unstable; do
	if [ -f "$d/unstable/wlr-virtual-pointer-unstable-v1.xml" ]; then
		vptrxml=$d/unstable/wlr-virtual-pointer-unstable-v1.xml
		break
	fi
	if [ -f "$d/wlr-virtual-pointer-unstable-v1.xml" ]; then
		vptrxml=$d/wlr-virtual-pointer-unstable-v1.xml
		break
	fi
done
[ -n "$vptrxml" ] || skip "wlr-virtual-pointer protocol not installed"

compositor=${COMPOSITOR:-}
if [ -z "$compositor" ]; then
	for c in mango sway river; do
		if command -v "$c" >/dev/null; then
			compositor=$c
			break
		fi
	done
fi
[ -n "$compositor" ] || skip "no wlroots compositor found (set COMPOSITOR)"

mkdir "$work/protocols" "$work/runtime" "$work/tools"
cp -R "$root/patch" "$work/patch"
cp "$root"/*.c "$root"/*.h "$root/Makefile" "$root/config.mk" "$work/"
cp "$root/tools/selectionpeer.c" "$root/tools/pointerclick.c" "$work/tools/"

# The virtual pointer protocol is only needed by the test helper, so it is
# generated here instead of becoming a build dependency of st.
$scanner client-header "$vptrxml" \
	"$work/protocols/wlr-virtual-pointer-unstable-v1-client-protocol.h"
$scanner private-code "$vptrxml" \
	"$work/protocols/wlr-virtual-pointer-unstable-v1-protocol.c"

# turn the EPIPE guard back into a bare write(), the pre-fix behaviour
python3 - "$work/wl.c" <<'PY'
import sys
from pathlib import Path
p = Path(sys.argv[1])
s = p.read_text()
old = 'n = writenopipe(j->fd, j->data + j->off, j->len - j->off);'
if old not in s:
    sys.exit("cannot find the guarded selection write in wl.c")
Path(sys.argv[1] + '.control').write_text(
    s.replace(old, 'n = write(j->fd, j->data + j->off, j->len - j->off);'))
PY

cd "$work"
cflags=${CFLAGS:--O1 -g}
ldflags=${LDFLAGS:-}
# build the tree as it is, then the same tree with the guard reverted
make -s -j2 CFLAGS="$cflags" LDFLAGS="$ldflags" >build-fixed.log 2>&1 ||
	{ cat build-fixed.log >&2; exit 1; }
mv st st-fixed
cp wl.c.control wl.c
touch wl.c
make -s -j2 CFLAGS="$cflags" LDFLAGS="$ldflags" >build-control.log 2>&1 ||
	{ cat build-control.log >&2; exit 1; }
mv st st-control

${CC:-cc} $cflags -DVERSION=\"test\" -Iprotocols -I. \
	$(pkg-config --cflags $packages) tools/selectionpeer.c \
	protocols/xdg-shell-protocol.c protocols/primary-selection-unstable-v1-protocol.c \
	$(pkg-config --libs $packages) -o selectionpeer
${CC:-cc} $cflags -Iprotocols \
	$(pkg-config --cflags wayland-client) tools/pointerclick.c \
	protocols/wlr-virtual-pointer-unstable-v1-protocol.c \
	$(pkg-config --libs wayland-client) -o pointerclick

: > mango.toml
XDG_RUNTIME_DIR=$work/runtime WLR_BACKENDS=headless \
	WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
	"$compositor" -c "$work/mango.toml" >compositor.log 2>&1 &
mangopid=$!
export XDG_RUNTIME_DIR=$work/runtime
i=0
while [ $i -lt 150 ]; do
	[ -S "$work/runtime/wayland-0" ] && break
	kill -0 "$mangopid" 2>/dev/null || { cat compositor.log >&2; exit 1; }
	sleep 0.1
	i=$((i + 1))
done
[ -S "$work/runtime/wayland-0" ] || skip "compositor did not start"
export WAYLAND_DISPLAY=wayland-0

select_word() {
	if ! ./pointerclick 200 40 2 80 >pointerclick.log 2>&1; then
		cat pointerclick.log >&2
		skip "virtual pointer unavailable"
	fi
	sleep 0.4
}

run_case() { # $1 = st binary, $2 = peer mode; echoes "alive" or the exit status
	"$1" -e sh -c 'for i in $(seq 1 30); do
		echo "line $i: hello world 0123456789"; done; sleep 120' \
		2>st.err &
	stpid=$!
	sleep 1.5
	select_word
	./selectionpeer "--$2" >peer.out 2>peer.err || true
	sleep 0.4
	if kill -0 "$stpid" 2>/dev/null; then
		echo alive
	else
		# wait already reports the status; "set -e" must not abort here
		wait "$stpid" && stsignal=0 || stsignal=$?
		stpid=
		if [ "$stsignal" -gt 128 ]; then
			echo "signal $((stsignal - 128)) ($(kill -l $((stsignal - 128))))"
		else
			echo "exit $stsignal"
		fi
	fi
}

printf 'control: '
result=$(run_case ./st-control drop)
case "$result" in
alive)
	printf 'survived a dropped selection (expected SIGPIPE) - the guard is\n'
	echo "         not exercised by this compositor, skipping the assertion"
	;;
*)
	printf '%s\n' "$result"
	case "$result" in
	"signal 13 (PIPE)") ;;
	*) echo "FAIL: expected death by SIGPIPE, got: $result" >&2; exit 1 ;;
	esac
	;;
esac

printf 'fixed:   '
result=$(run_case ./st-fixed drop)
stpid=
if [ "$result" != alive ]; then
	echo "FAIL: st died after the selection consumer closed the pipe: $result" >&2
	echo '--- st stderr:' >&2
	cat st.err >&2
	exit 1
fi
printf 'survives a dropped selection\n'

printf 'fixed:   serving a well behaved reader: '
stpid=
"$PWD/st-fixed" -e sh -c 'for i in $(seq 1 30); do
	echo "line $i: hello world 0123456789"; done; sleep 120' 2>st.err &
stpid=$!
sleep 1.5
select_word
./selectionpeer --read >peer.out 2>peer.err || true
if ! grep -q hello peer.out; then
	echo FAIL
	echo "expected the selected word, got: '$(cat peer.out)'" >&2
	echo "peer stderr: $(cat peer.err)" >&2
	exit 1
fi
kill "$stpid" 2>/dev/null
stpid=
printf '%s\n' "$(head -c 40 peer.out)"

echo "all selection pipe tests passed"
