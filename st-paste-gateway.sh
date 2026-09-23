#!/bin/sh
# st-paste-gateway.sh — st 剪贴板网关：文本直通，图片 base64 直传 pty 给上层应用
#
# 用法（stdout 即 pty 输入，原样喂给前台 shell/nvim/tmux）：
#   1) st + externalpipein（推荐）：config.def.h 绑
#        static char *pasteimgcmd[] = { "/bin/sh", "-c",
#          "$HOME/bin/st-paste-gateway.sh", "externalpipein", NULL };
#        { TERMMOD, XK_I, externalpipein, { .v = pasteimgcmd } },
#   2) sxhkd / 桌面热键：~/bin/st-paste-gateway.sh --type
#   3) 手动测试：~/bin/st-paste-gateway.sh [--no-bracket] [--type] [--dir DIR]
#
# 依赖：wl-paste/wl-copy（wl-clipboard，必需），base64（coreutils，图片编码必需），
#       file（校验，可选），libnotify notify-send（可选），
#       wtype（仅 --type Wayland 下，可选）/ xdotool（仅 --type X11 回退）
# 约定：stdout 只写要喂给 pty 的载荷；日志/报错一律走 stderr。

set -u
DIR="${PASTE_DIR:-$HOME/Pictures/st-paste}"
BRACKET=1
TYPE_MODE=0

while [ $# -gt 0 ]; do
	case "$1" in
		--no-bracket) BRACKET=0; shift ;;
		--type) TYPE_MODE=1; shift ;;
		--dir) DIR="$2"; shift 2 ;;
		--dir=*) DIR="${1#--dir=}"; shift ;;
		-h|--help)
			echo "usage: $0 [--no-bracket] [--type] [--dir DIR]" >&2; exit 0 ;;
		*) echo "$0: unknown arg: $1" >&2; exit 2 ;;
	esac
done

log() { printf '%s\n' "$*" >&2; }
have() { command -v "$1" >/dev/null 2>&1; }
notify() { have notify-send && notify-send "st-paste" "$1" 2>/dev/null || log "$1"; }

# stdout 载荷输出：包 bracketed-paste，让 nvim/readline 识别为粘贴而非打字
emit() {
	# $1 = raw payload
	if [ "$BRACKET" = 1 ]; then
		printf '\033[200~%s\033[201~' "$1"
	else
		printf '%s' "$1"
	fi
}

# shell 单引号转义：foo'bar -> 'foo'\''bar'
sq() {
	# $1 = path
	escaped=$(printf '%s' "$1" | sed "s/'/'\\\\''/g")
	printf "'%s'" "$escaped"
}

do_type() {
	# --type 模式：把内容打进焦点窗口（给 sxhkd/桌面热键用），不再走 stdout。
	# Wayland 下优先 wtype，X11 回退 xdotool。
	# bracket 序列不能用 type 打，会被应用转义；--type 下只打裸路径/文本。
	if have wtype; then
		printf '%s' "$1" | wtype - 2>/dev/null
	elif have xdotool; then
		printf '%s' "$1" | xdotool type --clearmodifiers --file - 2>/dev/null
	else
		log "need wtype (Wayland) or xdotool (X11) for --type"; exit 3
	fi
}

if ! have wl-paste; then log "need wl-paste (wl-clipboard)"; exit 3; fi

TARGETS=$(wl-paste --list-types 2>/dev/null || true)

has_target() { printf '%s' "$TARGETS" | grep -qi "$1"; }

# Wayland mime 名与 X11 atom 名不同：文本可能是 text/plain（含 charset 后缀）、
# 文件管理器可能是 TEXT/STRING/UTF8_STRING，统一按正则匹配。
HAS_TXT=0; HAS_IMG=0; HAS_URI=0
has_target "text/plain\|UTF8_STRING\|TEXT\|^STRING$" && HAS_TXT=1
has_target "image/png" && HAS_IMG=1
has_target "text/uri-list" && HAS_URI=1

# 文本抓取：依次试 mime，wl-paste 无 -t 时按 compositor 推断类型，显式 -t 更稳。
clip_text() {
	wl-paste --no-newline -t text/plain 2>/dev/null && return 0
	wl-paste --no-newline -t "UTF8_STRING" 2>/dev/null && return 0
	wl-paste --no-newline -t "STRING" 2>/dev/null && return 0
	wl-paste --no-newline -t "TEXT" 2>/dev/null && return 0
	wl-paste --no-newline 2>/dev/null
}

# 文本优先：浏览器/微信同时给文本+预览图时避免误存图
if [ "$HAS_TXT" = 1 ]; then
	TXT=$(clip_text || true)
	if [ -n "$TXT" ]; then
		if [ "$TYPE_MODE" = 1 ]; then do_type "$TXT"; else emit "$TXT"; fi
		exit 0
	fi
	# 文本 target 存在但取空（e.g. 只要了图片），继续往下走图片分支
fi

# 文件管理器复制文件：text/uri-list -> file:// URL 解码成路径
if [ "$HAS_URI" = 1 ] && [ "$HAS_IMG" = 0 ]; then
	URIS=$(wl-paste --no-newline -t text/uri-list 2>/dev/null || true)
	if [ -n "$URIS" ]; then
		PATHS=""
		TMPF="${TMPDIR:-/tmp}/st-paste-uris.$$"
		printf '%s\n' "$URIS" > "$TMPF"
		while IFS= read -r u || [ -n "$u" ]; do
			[ -z "$u" ] && continue
			case "$u" in \#*) continue;; esac
			# 去 file:// 前缀 + %XX 解码
			p=${u#file://localhost}
			p=${p#file://}
			if have python3; then
				p=$(python3 -c 'import sys,urllib.parse;print(urllib.parse.unquote(sys.argv[1]))' "$p")
			fi
			if [ -z "$PATHS" ]; then PATHS="$(sq "$p")"; else PATHS="$PATHS $(sq "$p")"; fi
		done < "$TMPF"
		rm -f "$TMPF"
		if [ -n "$PATHS" ]; then
			if [ "$TYPE_MODE" = 1 ]; then do_type "$PATHS"; else emit "$PATHS"; fi
			exit 0
		fi
	fi
fi

# 位图：截图/浏览器/微信复制的 image/png → base64 直传 pty（不再落盘存文件）
if [ "$HAS_IMG" = 1 ]; then
	TMPF="${TMPDIR:-/tmp}/st-paste-img.$$"
	if ! wl-paste --no-newline -t image/png > "$TMPF" 2>/dev/null; then
		rm -f "$TMPF"; log "clipboard has no image/png data"; exit 1
	fi
	if [ ! -s "$TMPF" ]; then rm -f "$TMPF"; log "clipboard image empty"; exit 1; fi
	if have file && ! file "$TMPF" | grep -qi "PNG image"; then
		rm -f "$TMPF"; log "clipboard data is not PNG"; exit 1
	fi
	B64=$(base64 -w0 "$TMPF" 2>/dev/null || base64 "$TMPF" | tr -d '\n')
	rm -f "$TMPF"
	if [ -z "$B64" ]; then log "base64 encode failed"; exit 1; fi
	# pty 一次喂几 MB 会卡上层应用，超阈值只告警（PASTE_MAX_B64 调整，0=不限）
	SZ=${#B64}
	LIM=${PASTE_MAX_B64:-2000000}
	if [ "$LIM" != 0 ] && [ "$SZ" -gt "$LIM" ]; then
		notify "图片 base64 约 ${SZ} 字符，超过 ${LIM}，仍继续粘贴（可能卡顿）"
	fi
	if [ "$TYPE_MODE" = 1 ]; then do_type "$B64"; else emit "$B64"; fi
	exit 0
fi

log "剪贴板无可用内容 (TARGETS: $TARGETS)"
exit 1
