#!/bin/bash
for i in $(seq 1 30); do c=$(tput cols); [ "$c" != 80 ] && break; sleep 0.05; done
sleep 0.2
printf '\e[1mBold\e[0m \e[3mItalic\e[0m \e[4mUnderline\e[0m \e[4:3mUndercurl\e[0m \e[7mReverse\e[0m \e[2mFaint\e[0m \e[9mStruck\e[0m\n'
printf '\e[31mred \e[32mgreen \e[33myellow \e[34mblue \e[35mmagenta \e[36mcyan \e[0m \e[38;2;255;128;0mtruecolor\e[0m \e[48;2;30;60;120m bg \e[0m \e[41;97m redbg \e[0m\n'
printf 'Ligatures: -> => != === <= >= <!-- --> :: www fi fl ffi\n'
printf 'CJK: 中文字符测试 日本語 한국어  Emoji: 😀 🚀 ✓ ★\n'
printf 'Box: ┌─┬─┐ │ ├─┼─┤ └─┴─┘ ═══ ║ ╔═╗ ▀▄█▌▐░▒▓ Braille: ⠁⠃⠇⠏⠟⠿⡿⣿ ⣀⣤⣶⣿\n'
printf '\e[4:3m\e[58;2;255;0;0mcolored undercurl\e[0m  \e[4mnormal underline\e[0m \e[1;3;4mBIU\e[0m\n'
printf 'Long line: '; for i in $(seq 1 12); do printf 'abcdefghij'; done; printf '\n'
printf '\e[7m  reverse with spaces  \e[0m end\n'
printf '\e[1;31mBold red\e[0m \e[1;34mBold blue\e[0m \e[3;32mItalic green\e[0m \e[1;3;35mBoldItalic magenta\e[0m\n'
printf 'cursor here>'
sleep ${SLEEP:-2}
