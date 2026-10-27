/* Link against actual st.o; no display or terminal initialization required. */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "st.h"

/* Normally owned by wl.c; only storage is supplied by this headless harness. */
Term term;

int
main(void)
{
	Glyph cells[2][2];
	Line lines[] = { cells[0], cells[1] };
	int dirty[2];
	term.line = lines;
	term.dirty = dirty;
	for (int size = 1; size <= 2; size++) {
		term.row = term.col = size;
		for (int y = 0; y < size; y++) {
			for (int x = 0; x < size; x++) {
				memset(cells, 0, sizeof cells);
				memset(dirty, 0, sizeof dirty);
				cells[y][x].mode = ATTR_BLINK | ATTR_BOLD;
				assert(tattrset(ATTR_BLINK) == 1);
				tsetdirtattr(ATTR_BLINK);
				for (int r = 0; r < size; r++)
					assert(dirty[r] == (r == y));
				cells[y][x].mode &= ~ATTR_BLINK;
				memset(dirty, 0, sizeof dirty);
				assert(tattrset(ATTR_BLINK) == 0);
				assert(tattrset(ATTR_BOLD) == 1);
				tsetdirtattr(ATTR_BLINK);
				for (int r = 0; r < size; r++)
					assert(dirty[r] == 0);
			}
		}
		/* All matching rows, including the last, must be dirtied. */
		memset(cells, 0, sizeof cells);
		memset(dirty, 0, sizeof dirty);
		for (int y = 0; y < size; y++)
			cells[y][size - 1].mode = ATTR_BLINK;
		tsetdirtattr(ATTR_BLINK);
		for (int y = 0; y < size; y++)
			assert(dirty[y] == 1);
	}
	puts("blink bounds: 1x1 and all 2x2 cells, cleared attributes and dirty rows PASS");
	return 0;
}
