#!/usr/bin/env python3
"""Cursor-only jumps over a static TUI; no dependencies. Ctrl+C exits."""
import shutil
import sys
import time

def write(text):
    sys.stdout.write(text)
    sys.stdout.flush()

write("\x1b[?1049h\x1b[2J\x1b[2 q\x1b[?25h")
try:
    while True:
        cols, rows = shutil.get_terminal_size()
        write("\x1b[?25l\x1b[H\x1b[2JCursor trail: block / bar / underline (Ctrl+C exits)")
        for y in range(3, max(4, rows - 2)):
            write(f"\x1b[{y};3H" + (" . : - = + * # " * (cols // 15 + 1))[:max(1, cols - 6)])
        positions = [(5, 5), (5, max(8, cols - 8)),
                     (max(6, rows - 5), max(8, cols - 8)),
                     (max(6, rows - 5), 5), (max(6, rows // 2), max(8, cols // 2))]
        for style in (2, 6, 4):
            write(f"\x1b[{style} q\x1b[?25h")
            for y, x in positions:
                write(f"\x1b[{min(y, rows)};{min(x, cols)}H")
                time.sleep(0.75)
except KeyboardInterrupt:
    pass
finally:
    write("\x1b[0m\x1b[2 q\x1b[?25h\x1b[?1049l")
