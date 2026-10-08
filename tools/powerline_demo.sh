#!/bin/sh
# Run inside st-wayland; zoom while this stays visible. No extra fonts required
# for the separators; the Nerd icon control line intentionally uses real fonts.
# POWERLINE_DEMO_SLEEP=N makes this noninteractive (0 exits immediately).
set -eu
trap 'printf "\033[0m"' EXIT HUP INT TERM
printf '\033[0m\nCell-aligned Powerline: zoom in/out and inspect joins and cell edges.\n\n'
printf '  Filled:   \033[36m███████████\033[0m\n'
printf '  Outlined: \033[36m       \033[0m\n'
printf '  Bold:     \033[1;36m███████████\033[0m\n'
printf '  Italic:   \033[3;36m███████████\033[0m\n'
printf '  Both:     \033[1;3;36m███████████\033[0m\n\n'
printf '  \033[44;97m NORMAL \033[34;42m\033[30m main  st-wayland \033[32;43m\033[30m UTF-8 \033[33;40m\033[0m\n'
printf '  \033[36m\033[46;30m rounded caps \033[0;36m\033[0m   \033[35m\033[45;97m left cap \033[0;35m\033[0m\n\n'
printf '  Normal text / shaping control: fi fl ffi -> => != ===\n'
printf '  Font-only Nerd icons:      (must not turn into geometry)\n'
printf '  Tiny cells may antialias diagonals; blocks must stay solid.\n\n'
if [ "${POWERLINE_DEMO_SLEEP+x}" = x ]; then
	sleep "$POWERLINE_DEMO_SLEEP"
else
	printf 'Press Enter when finished inspecting (Ctrl+C also exits). '
	IFS= read -r answer || :
fi
