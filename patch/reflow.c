/* History stores a prefix including one copy of its repeated final cell.
 * Read-only scans use a compact view; display/editing expands rows in place.
 * No expanded row moves again until the event loop calls thistcompact(). */
static int histaccessed; /* nothing to compact during ordinary output/typing */

static int
thistlength(Line line, int col)
{
	int len = col;

	while (len > 1 && !memcmp(&line[len - 2], &line[col - 1], sizeof(Glyph)))
		len--;
	return len;
}

static void
thiststore(int i, Line line, int col)
{
	int len = thistlength(line, col);

	/* Shrinking an expanded row in place leaves a small prefix on almost
	 * every old page. Allocate its packed copy separately so those pages
	 * can actually be returned after searching or reflowing the history. */
	Line old = term.hist[i];
	if (len < abs(term.histlen[i])) {
		term.hist[i] = xmalloc(len * sizeof(Glyph));
		memcpy(term.hist[i], line, len * sizeof(Glyph));
		free(old);
	} else {
		term.hist[i] = xrealloc(old, len * sizeof(Glyph));
		memcpy(term.hist[i], line, len * sizeof(Glyph));
	}
	term.histlen[i] = len;
	term.histcols[i] = col;
}

Line
thistline(int i)
{
	int len = term.histlen[i], col = term.histcols[i], x;
	Line line = term.hist[i];

	if (len > 0) {
		if (len < col) {
			line = term.hist[i] = xrealloc(line, col * sizeof(Glyph));
			for (x = len; x < col; x++)
				line[x] = line[len - 1];
		}
		term.histlen[i] = -col;
		histaccessed = 1;
	}
	return line;
}

static LineView
tlineview(int y)
{
	if (y < term.scr) {
		int i = (term.histi + y - term.scr + 1 + HISTSIZE) % HISTSIZE;
		return (LineView){term.hist[i], abs(term.histlen[i])};
	}
	return (LineView){term.line[y - term.scr], term.col};
}

static const Glyph *
tviewcell(LineView view, int x)
{
	return &view.line[MIN(x, view.stored - 1)];
}

static void
tviewcopy(Line dst, LineView view, int x, int count)
{
	int copied = MIN(count, MAX(view.stored - x, 0));

	if (copied)
		memcpy(dst, view.line + x, copied * sizeof(Glyph));
	for (int i = copied; i < count; i++)
		dst[i] = view.line[view.stored - 1];
}

/* The same logical line ending as tlinelen(), without reading/allocating
 * the repeated tail one cell at a time. */
static int
tviewlen(LineView view)
{
	int i = term.col - 1;
	while (i >= 0) {
		const Glyph *g = tviewcell(view, i);
		if (IS_SET(MODE_ALTSCREEN) ? (g->mode & ATTR_WRAP) || g->u != ' '
		                          : (g->mode & (ATTR_SET | ATTR_WRAP)))
			break;
		i = i >= view.stored ? view.stored - 1 : i - 1;
	}
	return i + 1;
}

/* Safe boundary: all event handlers and painters have returned. Keep visible
 * rows and the keyboard selection's persistent cursors at stable addresses. */
void
thistcompact(void)
{
	int i, y, first = term.histi + HISTSIZE + 1 - term.scr;
	size_t freed = 0;

	if (!histaccessed)
		return;
	histaccessed = 0;
	for (i = 0; i < HISTSIZE; i++) {
		if (term.histlen[i] >= 0)
			continue;
		y = (i - first + 2 * HISTSIZE) % HISTSIZE;
		if (y < term.scr && y < term.row && !IS_SET(MODE_ALTSCREEN)) {
			histaccessed = 1;
			continue;
		}
		#if KEYBOARDSELECT_PATCH
		if (kbds_in_use && (term.hist[i] == kbds_c.line || term.hist[i] == kbds_oc.line)) {
			histaccessed = 1;
			continue;
		}
		#endif
		int col = -term.histlen[i];
		Line old = term.hist[i];
		int len = thistlength(old, col);
		if (len < col) {
			term.hist[i] = xmalloc(len * sizeof(Glyph));
			memcpy(term.hist[i], old, len * sizeof(Glyph));
			free(old);
			freed += (col - len) * sizeof(Glyph);
		}
		term.histlen[i] = len;
	}
	/* A search can expand all history while visible/pinned rows keep the
	 * heap's end live. Return freed interior pages after a bulk compaction,
	 * not for the few rows touched by ordinary scrolling. */
	if (freed >= 1024 * 1024)
		malloc_trim(0);
}

void
tloaddefscreen(int clear, int loadcursor)
{
	int col, row, alt = IS_SET(MODE_ALTSCREEN);

	if (alt) {
		if (clear) {
			tclearregion(0, 0, term.col-1, term.row-1, 1);
			#if SIXEL_PATCH
			tdeleteimages();
			#endif // SIXEL_PATCH
		}
		col = term.col, row = term.row;
		tswapscreen();
	}
	if (loadcursor)
		tcursor(CURSOR_LOAD);
	if (alt)
		tresizedef(col, row);
}

void
tloadaltscreen(int clear, int savecursor)
{
	int col, row, def = !IS_SET(MODE_ALTSCREEN);

	if (savecursor)
		tcursor(CURSOR_SAVE);
	if (def) {
		col = term.col, row = term.row;
		kscrolldown(&((Arg){ .i = term.scr }));
		tswapscreen();
		tresizealt(col, row);
	}
	if (clear) {
		tclearregion(0, 0, term.col-1, term.row-1, 1);
		#if SIXEL_PATCH
		tdeleteimages();
		#endif // SIXEL_PATCH
	}
}

void
selmove(int n)
{
	sel.ob.y += n, sel.nb.y += n;
	sel.oe.y += n, sel.ne.y += n;
}

void
tclearglyph(Glyph *gp, int usecurattr)
{
	if (usecurattr) {
		gp->fg = term.c.attr.fg;
		gp->bg = term.c.attr.bg;
	} else {
		gp->fg = defaultfg;
		gp->bg = defaultbg;
	}
	gp->mode = ATTR_NULL;
	gp->u = ' ';
}

#if SIXEL_PATCH
void
treflow_moveimages(int oldy, int newy)
{
	ImageList *im;

	for (im = term.images; im; im = im->next) {
		if (im->y == oldy)
			im->reflow_y = newy;
	}
}
#endif // SIXEL_PATCH

/* Reflow keeps completed rows packed, including those which might eventually
 * become visible. Only one unfinished output row needs full-width storage. */
static void
treflowstore(LineView *dst, Line line, int col)
{
	int len = thistlength(line, col);

	dst->line = xrealloc(dst->line, len * sizeof(Glyph));
	memcpy(dst->line, line, len * sizeof(Glyph));
	dst->stored = len;
}

static Line
treflowexpand(LineView view, int col)
{
	Line line = view.line;

	if (view.stored < col) {
		line = xrealloc(line, col * sizeof(Glyph));
		for (int x = view.stored; x < col; x++)
			line[x] = line[view.stored - 1];
	}
	return line;
}

void
treflow(int col, int row)
{
	int i, j;
	int oce, nce, bot, scr;
	int ox = 0, oy = -term.histf, nx = 0, ny = -1, len;
	int cy = -1; /* proxy for new y coordinate of cursor */
	int buflen, nlines;
	LineView *buf, view = {0};
	Line bufline, line;
	Glyph blank = {0};
	#if SIXEL_PATCH
	ImageList *im, *next;

	for (im = term.images; im; im = im->next)
		im->reflow_y = INT_MIN; /* unset reflow_y */
	#endif // SIXEL_PATCH

	/* y coordinate of cursor line end */
	for (oce = term.c.y; oce < term.row - 1 &&
	                     tiswrapped(term.line[oce]); oce++);

	nlines = HISTSIZE + row;
	buf = xmalloc(nlines * sizeof(*buf));
	memset(buf, 0, nlines * sizeof(*buf));
	bufline = xmalloc(col * sizeof(Glyph));
	tclearglyph(&blank, 0);
	do {
		if (!nx)
			++ny;
		if (!ox) {
			view = tlineview(oy + term.scr);
			len = tviewlen(view);
		}
		if (oy == term.c.y) {
			if (!ox)
				len = MAX(len, term.c.x + 1);
			/* update cursor */
			if (cy < 0 && term.c.x - ox < col - nx) {
				term.c.x = nx + term.c.x - ox, cy = ny;
				UPDATEWRAPNEXT(0, col);
			}
		}
		/* get reflowed lines in buf */
		if (col - nx > len - ox) {
			tviewcopy(&bufline[nx], view, ox, len - ox);
			nx += len - ox;
			if (len == 0 || !(tviewcell(view, len - 1)->mode & ATTR_WRAP)) {
				for (j = nx; j < col; j++)
					bufline[j] = blank;
				#if SIXEL_PATCH
				treflow_moveimages(oy+term.scr, ny);
				#endif // SIXEL_PATCH
				nx = 0;
			} else if (nx > 0) {
				bufline[nx - 1].mode &= ~ATTR_WRAP;
			}
			ox = 0, oy++;
		} else if (col - nx == len - ox) {
			tviewcopy(&bufline[nx], view, ox, col - nx);
			#if SIXEL_PATCH
			treflow_moveimages(oy+term.scr, ny);
			#endif // SIXEL_PATCH
			ox = 0, oy++, nx = 0;
		} else/* if (col - nx < len - ox) */ {
			tviewcopy(&bufline[nx], view, ox, col - nx);
			if (bufline[col - 1].mode & ATTR_WIDE) {
				bufline[col - 2].mode |= ATTR_WRAP;
				bufline[col - 1] = blank;
				ox--;
			} else {
				bufline[col - 1].mode |= ATTR_WRAP;
			}
			#if SIXEL_PATCH
			treflow_moveimages(oy+term.scr, ny);
			#endif // SIXEL_PATCH
			ox += col - nx;
			nx = 0;
		}
		if (!nx)
			treflowstore(&buf[ny % nlines], bufline, col);
	} while (oy <= oce);
	if (nx) {
		for (j = nx; j < col; j++)
			bufline[j] = blank;
		treflowstore(&buf[ny % nlines], bufline, col);
	}
	free(bufline);

	/* free extra lines */
	for (i = row; i < term.row; i++)
		free(term.line[i]);
	/* resize to new height */
	term.line = xrealloc(term.line, row * sizeof(Line));

	buflen = MIN(ny + 1, nlines);
	bot = MIN(ny, row - 1);
	scr = MAX(row - term.row, 0);
	/* update y coordinate of cursor line end */
	nce = MIN(oce + scr, bot);
	/* update cursor y coordinate */
	term.c.y = nce - (ny - cy);
	if (term.c.y < 0) {
		j = nce, nce = MIN(nce + -term.c.y, bot);
		term.c.y += nce - j;
		while (term.c.y < 0) {
			free(buf[ny-- % nlines].line);
			buflen--;
			term.c.y++;
		}
	}
	/* allocate new rows */
	for (i = row - 1; i > nce; i--) {
		if (i >= term.row)
			term.line[i] = xmalloc(col * sizeof(Glyph));
		else
			term.line[i] = xrealloc(term.line[i], col * sizeof(Glyph));
		for (j = 0; j < col; j++)
			tclearglyph(&term.line[i][j], 0);
	}
	/* fill visible area */
	for (/*i = nce */; i >= term.row; i--, ny--, buflen--)
		term.line[i] = treflowexpand(buf[ny % nlines], col);
	for (/*i = term.row - 1 */; i >= 0; i--, ny--, buflen--) {
		free(term.line[i]);
		term.line[i] = treflowexpand(buf[ny % nlines], col);
	}
	/* fill lines in history buffer and update term.histf */
	for (/*i = -1 */; buflen > 0 && i >= -HISTSIZE; i--, ny--, buflen--) {
		j = (term.histi + i + 1 + HISTSIZE) % HISTSIZE;
		free(term.hist[j]);
		term.hist[j] = buf[ny % nlines].line;
		term.histlen[j] = buf[ny % nlines].stored;
		term.histcols[j] = col;
	}
	term.histf = -i - 1;
	term.scr = MIN(term.scr, term.histf);
	/* the rest of the history holds no content: release it (lazy alloc) */
	for (/*i = -term.histf - 1 */; i >= -HISTSIZE; i--) {
		j = (term.histi + i + 1 + HISTSIZE) % HISTSIZE;
		free(term.hist[j]);
		term.hist[j] = NULL;
		term.histlen[j] = term.histcols[j] = 0;
	}

	#if SIXEL_PATCH
	/* move images to the final position */
	for (im = term.images; im; im = next) {
		next = im->next;
		if (im->reflow_y == INT_MIN) {
			delete_image(im);
		} else {
			im->y = im->reflow_y - term.histf + term.scr - (ny + 1);
			if (im->y - term.scr < -HISTSIZE || im->y - term.scr >= row)
				delete_image(im);
		}
	}

	/* expand images into new text cells */
	for (im = term.images; im; im = im->next) {
		j = MIN(im->x + im->cols, col);
		line = TLINE(im->y);
		for (i = im->x; i < j; i++) {
			if (!(line[i].mode & ATTR_SET))
				line[i].mode |= ATTR_SIXEL;
		}
	}
	#endif // SIXEL_PATCH

	for (; buflen > 0; ny--, buflen--)
		free(buf[ny % nlines].line);
	free(buf);
}

void
rscrolldown(int n)
{
	int i;
	Line temp;

	/* can never be true as of now
	if (IS_SET(MODE_ALTSCREEN))
		return; */

	if ((n = MIN(n, term.histf)) <= 0)
		return;

	for (i = term.c.y + n; i >= n; i--) {
		temp = term.line[i];
		term.line[i] = term.line[i-n];
		term.line[i-n] = temp;
	}
	for (/*i = n - 1 */; i >= 0; i--) {
		temp = term.line[i];
		term.line[i] = thistline(term.histi);
		term.hist[term.histi] = NULL;
		term.histlen[term.histi] = term.histcols[term.histi] = 0;
		free(temp);
		term.histi = (term.histi - 1 + HISTSIZE) % HISTSIZE;
	}
	term.c.y += n;
	term.histf -= n;
	if ((i = term.scr - n) >= 0) {
		term.scr = i;
	} else {
		#if SIXEL_PATCH
		scroll_images(n - term.scr);
		#endif // SIXEL_PATCH
		term.scr = 0;
		if (sel.ob.x != -1 && !sel.alt)
			selmove(-i);
	}
}

void
tresizedef(int col, int row)
{
	int i, j;

	/* return if dimensions haven't changed */
	if (term.col == col && term.row == row) {
		tfulldirt();
		return;
	}
	if (col != term.col) {
		if (!sel.alt)
			selremove();
		treflow(col, row);
	} else {
		/* slide screen up if otherwise cursor would get out of the screen */
		if (term.c.y >= row) {
			tscrollup(0, term.row - 1, term.c.y - row + 1, SCROLL_RESIZE);
			term.c.y = row - 1;
		}
		for (i = row; i < term.row; i++)
			free(term.line[i]);

		/* resize to new height */
		term.line = xrealloc(term.line, row * sizeof(Line));
		/* allocate any new rows */
		for (i = term.row; i < row; i++) {
			term.line[i] = xmalloc(col * sizeof(Glyph));
			for (j = 0; j < col; j++)
				tclearglyph(&term.line[i][j], 0);
		}
		/* scroll down as much as height has increased */
		rscrolldown(row - term.row);
	}
	/* update terminal size */
	term.col = col, term.row = row;
	/* reset scrolling region */
	term.top = 0, term.bot = row - 1;
	/* dirty all lines */
	tfulldirt();
}

void
tresizealt(int col, int row)
{
	int i, j;
	#if SIXEL_PATCH
	ImageList *im, *next;
	#endif // SIXEL_PATCH

	/* return if dimensions haven't changed */
	if (term.col == col && term.row == row) {
		tfulldirt();
		return;
	}
	if (sel.alt)
		selremove();
	/* slide screen up if otherwise cursor would get out of the screen */
	for (i = 0; i <= term.c.y - row; i++)
		free(term.line[i]);
	if (i > 0) {
		/* ensure that both src and dst are not NULL */
		memmove(term.line, term.line + i, row * sizeof(Line));
		#if SIXEL_PATCH
		scroll_images(-i);
		#endif // SIXEL_PATCH
		term.c.y = row - 1;
	}
	for (i += row; i < term.row; i++)
		free(term.line[i]);
	/* resize to new height */
	term.line = xrealloc(term.line, row * sizeof(Line));
	/* resize to new width */
	for (i = 0; i < MIN(row, term.row); i++) {
		term.line[i] = xrealloc(term.line[i], col * sizeof(Glyph));
		for (j = term.col; j < col; j++)
			tclearglyph(&term.line[i][j], 0);
	}
	/* allocate any new rows */
	for (/*i = MIN(row, term.row) */; i < row; i++) {
		term.line[i] = xmalloc(col * sizeof(Glyph));
		for (j = 0; j < col; j++)
			tclearglyph(&term.line[i][j], 0);
	}
	/* update cursor */
	if (term.c.x >= col) {
		term.c.state &= ~CURSOR_WRAPNEXT;
		term.c.x = col - 1;
	} else {
		UPDATEWRAPNEXT(1, col);
	}
	/* update terminal size */
	term.col = col, term.row = row;
	/* reset scrolling region */
	term.top = 0, term.bot = row - 1;

	#if SIXEL_PATCH
	/* delete or clip images if they are not inside the screen */
	for (im = term.images; im; im = next) {
		next = im->next;
		if (im->x >= term.col || im->y >= term.row || im->y < 0) {
			delete_image(im);
		} else {
			if ((im->cols = MIN(im->x + im->cols, term.col) - im->x) <= 0)
				delete_image(im);
		}
	}
	#endif // SIXEL_PATCH

	/* dirty all lines */
	tfulldirt();
}

/* release all scrollback lines; caller resets histf/histi/scr */
void
tfreehist(void)
{
	int i;

	for (i = 0; i < HISTSIZE; i++) {
		free(term.hist[i]);
		term.hist[i] = NULL;
		term.histlen[i] = term.histcols[i] = 0;
	}
	histaccessed = 0;
	malloc_trim(0); /* actually return the freed scrollback to the kernel */
}

void
kscrolldown(const Arg* a)
{
	int n = a->i;

	if (!term.scr || IS_SET(MODE_ALTSCREEN))
		return;

	if (n < 0)
		n = MAX(term.row / -n, 1);

	if (n <= term.scr) {
		term.scr -= n;
	} else {
		n = term.scr;
		term.scr = 0;
	}

	if (sel.ob.x != -1 && !sel.alt)
		selmove(-n); /* negate change in term.scr */
	#if SYNC_PATCH
	tsync_end(); /* as tfulldirt() does: the user is not kept waiting */
	#endif // SYNC_PATCH
	tscrolldirt(0, term.row - 1, n);

	#if SIXEL_PATCH
	scroll_images(-1*n);
	#endif // SIXEL_PATCH

	#if OPENURLONCLICK_PATCH
	if (n > 0)
		restoremousecursor();
	#endif // OPENURLONCLICK_PATCH
}

void
kscrollup(const Arg* a)
{
	int n = a->i;

	if (!term.histf || IS_SET(MODE_ALTSCREEN))
		return;

	if (n < 0)
		n = MAX(term.row / -n, 1);

	if (term.scr + n <= term.histf) {
		term.scr += n;
	} else {
		n = term.histf - term.scr;
		term.scr = term.histf;
	}

	if (sel.ob.x != -1 && !sel.alt)
		selmove(n); /* negate change in term.scr */
	#if SYNC_PATCH
	tsync_end(); /* as tfulldirt() does: the user is not kept waiting */
	#endif // SYNC_PATCH
	tscrolldirt(0, term.row - 1, -n);

	#if SIXEL_PATCH
	scroll_images(n);
	#endif // SIXEL_PATCH

	#if OPENURLONCLICK_PATCH
	if (n > 0)
		restoremousecursor();
	#endif // OPENURLONCLICK_PATCH
}

/*
 * Rows top..bot of the view moved up by n (down if negative) and the rows
 * they left hold new lines.  The lines that only moved are already painted:
 * their dirty flags move with them and the move is recorded, so that the
 * window shifts their pixels instead of drawing every line of the region
 * again (scrollpixels() in wl.c).
 */
void
tscrolldirt(int top, int bot, int n)
{
	int h = bot - top + 1, m = abs(n), i;
	RowScroll *s;

	if (!n || h <= 0)
		return;
	#if OPENURLONCLICK_PATCH
	clearurl(); /* its underline is painted over the rows, it does not move */
	#endif // OPENURLONCLICK_PATCH
	if (m >= h) {
		n = n < 0 ? -h : h;
		m = h;
	} else if (n > 0) {
		memmove(&term.dirty[top], &term.dirty[top + m], (h - m) * sizeof(*term.dirty));
		if (term.dirtycols)
			memmove(&term.dirtycols[top], &term.dirtycols[top + m],
			        (h - m) * sizeof(*term.dirtycols));
	} else {
		memmove(&term.dirty[top + m], &term.dirty[top], (h - m) * sizeof(*term.dirty));
		if (term.dirtycols)
			memmove(&term.dirtycols[top + m], &term.dirtycols[top],
			        (h - m) * sizeof(*term.dirtycols));
	}
	for (i = n > 0 ? bot - m + 1 : top; m > 0; m--)
		term.dirty[i++] = 1;

	/* scrolls of other rows in between do not change what this one moves */
	for (i = term.nscrolls - 1; i >= 0; i--) {
		s = &term.scrolls[i];
		if (s->top == top && s->bot == bot) {
			s->n += n;
			LIMIT(s->n, -h, h);
			return;
		}
		if (s->top <= bot && top <= s->bot)
			break;
	}
	if (term.nscrolls < LEN(term.scrolls))
		term.scrolls[term.nscrolls++] = (RowScroll){top, bot, n};
	else
		tfulldirt(); /* too many to follow */
}

void
tscrollup(int top, int bot, int n, int mode)
{
	#if OPENURLONCLICK_PATCH
	restoremousecursor();
	#endif //OPENURLONCLICK_PATCH

	int i, j, s;
	int alt = IS_SET(MODE_ALTSCREEN);
	int savehist = !alt && top == 0 && mode != SCROLL_NOSAVEHIST;
	int scr = alt ? 0 : term.scr;
	#if SIXEL_PATCH
	int itop = top + scr, ibot = bot + scr;
	ImageList *im, *next;
	#endif // SIXEL_PATCH

	if (n <= 0)
		return;
	n = MIN(n, bot-top+1);

	if (savehist) {
		/* one template glyph, whole-struct stores: vectorizable, unlike tclearglyph() per cell */
		Glyph blank = { .u = ' ', .mode = ATTR_NULL,
		                .fg = term.c.attr.fg, .bg = term.c.attr.bg };
		for (i = 0; i < n; i++) {
			term.histi = (term.histi + 1) % HISTSIZE;
			thiststore(term.histi, term.line[i], term.col);
			for (j = 0; j < term.col; j++)
				term.line[i][j] = blank;
		}
		term.histf = MIN(term.histf + n, HISTSIZE);
		s = n;
		if (term.scr) {
			j = term.scr;
			term.scr = MIN(j + n, HISTSIZE);
			s = j + n - term.scr;
		}
		if (mode != SCROLL_RESIZE) { /* else the caller repaints everything */
			#if SYNC_PATCH
			tsync_end(); /* as the tfulldirt() this once was */
			#endif // SYNC_PATCH
			if (!scr)
				tscrolldirt(0, bot, n);
			else if (s || bot < term.row - 1)
				tfulldirt();
			/* else the view is scrolled back and stays on the same lines */
		}
	} else {
		tclearregion(0, top, term.col-1, top+n-1, 1);
		if (scr)
			tsetdirt(top + scr, bot + scr);
		else
			tscrolldirt(top, bot, n);
	}

	/*
	 * Rotate line[top..bot] left by n: the n lines at the top (now blank)
	 * end up at the bottom. One memmove instead of a swap per row.
	 */
	if (n < bot - top + 1) {
		Line saved[n];
		memcpy(saved, &term.line[top], n * sizeof(Line));
		memmove(&term.line[top], &term.line[top + n],
		        (bot - top + 1 - n) * sizeof(Line));
		memcpy(&term.line[bot - n + 1], saved, n * sizeof(Line));
	}

	#if SIXEL_PATCH
	if (alt || !savehist) {
		/* move images, if they are inside the scrolling region */
		for (im = term.images; im; im = next) {
			next = im->next;
			if (im->y >= itop && im->y <= ibot) {
				im->y -= n;
				if (im->y < itop)
					delete_image(im);
			}
		}
	} else {
		/* move images, if they are inside the scrolling region or scrollback */
		for (im = term.images; im; im = next) {
			next = im->next;
			im->y -= scr;
			if (im->y < 0) {
				im->y -= n;
			} else if (im->y >= top && im->y <= bot) {
				im->y -= n;
				if (im->y < top)
					im->y -= top; // move to scrollback
			}
			if (im->y < -HISTSIZE)
				delete_image(im);
			else
				im->y += term.scr;
		}
	}
	#endif // SIXEL_PATCH

	if (sel.ob.x != -1 && sel.alt == alt) {
		if (!savehist) {
			selscroll(top, bot, -n);
		} else if (s > 0) {
			/* it moves as a whole, also below the region where the
			 * lines stay, and is dropped once it leaves the history */
			if (mode != SCROLL_RESIZE && (sel.ne.y > bot + scr ||
			    sel.nb.y - s - term.scr < -term.histf))
				tfulldirt();
			selmove(-s);
			if (-term.scr + sel.nb.y < -term.histf)
				selremove();
		}
	}
}

void
tscrolldown(int top, int n)
{
	#if OPENURLONCLICK_PATCH
	restoremousecursor();
	#endif //OPENURLONCLICK_PATCH

	int i, bot = term.bot;
	int scr = IS_SET(MODE_ALTSCREEN) ? 0 : term.scr;
	int itop = top + scr, ibot = bot + scr;
	Line temp;
	#if SIXEL_PATCH
	ImageList *im, *next;
	#endif // SIXEL_PATCH

	if (n <= 0)
		return;
	n = MIN(n, bot-top+1);

	tclearregion(0, bot-n+1, term.col-1, bot, 1);
	if (scr)
		tsetdirt(top + scr, bot + scr);
	else
		tscrolldirt(top, bot, -n);

	for (i = bot; i >= top+n; i--) {
		temp = term.line[i];
		term.line[i] = term.line[i-n];
		term.line[i-n] = temp;
	}

	#if SIXEL_PATCH
	/* move images, if they are inside the scrolling region */
	for (im = term.images; im; im = next) {
		next = im->next;
		if (im->y >= itop && im->y <= ibot) {
			im->y += n;
			if (im->y > ibot)
				delete_image(im);
		}
	}
	#endif // SIXEL_PATCH

	if (sel.ob.x != -1 && sel.alt == IS_SET(MODE_ALTSCREEN))
		selscroll(top, bot, n);
}

void
tresize(int col, int row)
{
	int *bp;

	#if KEYBOARDSELECT_PATCH
	if (row != term.row || col != term.col)
		win.mode ^= kbds_keyboardhandler(XKB_KEY_Escape, NULL, 0, 1);
	#endif // KEYBOARDSELECT_PATCH

	term.dirty = xrealloc(term.dirty, row * sizeof(*term.dirty));
	term.dirtycols = xrealloc(term.dirtycols, row * sizeof(*term.dirtycols));
	term.tabs = xrealloc(term.tabs, col * sizeof(*term.tabs));
	if (col > term.col) {
		bp = term.tabs + term.col;
		memset(bp, 0, sizeof(*term.tabs) * (col - term.col));
		while (--bp > term.tabs && !*bp)
			/* nothing */ ;
		for (bp += tabspaces; bp < term.tabs + col; bp += tabspaces)
			*bp = 1;
	}

	if (IS_SET(MODE_ALTSCREEN))
		tresizealt(col, row);
	else
		tresizedef(col, row);
}

void
tclearregion(int x1, int y1, int x2, int y2, int usecurattr)
{
	int x, y;

	/* regionselected() takes relative coordinates */
	if (regionselected(x1, y1+term.scr, x2, y2+term.scr))
		selclear();

	for (y = y1; y <= y2; y++) {
		tdirtcols(y, x1, x2 + 1);
		for (x = x1; x <= x2; x++)
			tclearglyph(&term.line[y][x], usecurattr);
	}
}

void
tnew(int col, int row)
{
	int i, j;
	for (i = 0; i < 2; i++) {
		term.line = xmalloc(row * sizeof(Line));
		for (j = 0; j < row; j++)
			term.line[j] = xmalloc(col * sizeof(Glyph));
		term.col = col, term.row = row;
		tswapscreen();
	}
	term.dirty = xmalloc(row * sizeof(*term.dirty));
	term.dirtycols = xmalloc(row * sizeof(*term.dirtycols));
	for (i = 0; i < row; i++)
		term.dirty[i] = DIRTY_FULL;
	term.tabs = xmalloc(col * sizeof(*term.tabs));
	/* scrollback lines are allocated lazily when content scrolls into them */
	for (i = 0; i < HISTSIZE; i++) {
		term.hist[i] = NULL;
		term.histlen[i] = term.histcols[i] = 0;
	}
	treset();
}

void
tdeletechar(int n)
{
	int src, dst, size;
	Line line;

	if (n <= 0)
		return;
	dst = term.c.x;
	src = MIN(term.c.x + n, term.col);
	size = term.col - src;
	if (size > 0) { /* otherwise src would point beyond the array
	                   https://stackoverflow.com/questions/29844298 */
		line = term.line[term.c.y];
		memmove(&line[dst], &line[src], size * sizeof(Glyph));
	}
	tdirtcols(term.c.y, dst, term.col);
	tclearregion(dst + size, term.c.y, term.col - 1, term.c.y, 1);
}

void
tinsertblank(int n)
{
	int src, dst, size;
	Line line;

	if (n <= 0)
		return;
	dst = MIN(term.c.x + n, term.col);
	src = term.c.x;
	size = term.col - dst;

	if (size > 0) { /* otherwise dst would point beyond the array */
		line = term.line[term.c.y];
		memmove(&line[dst], &line[src], size * sizeof(Glyph));
	}
	tdirtcols(term.c.y, src, term.col);
	tclearregion(src, term.c.y, dst - 1, term.c.y, 1);
}

int
tlinelen(Line line)
{
	int i = term.col - 1;

	/* We are using a different algorithm on the alt screen because an
	 * application might use spaces to clear the screen and in that case it is
	 * impossible to find the end of the line when every cell has the ATTR_SET
	 * attribute. The second algorithm is more accurate on the main screen and
	 * and we can use it there. */
	if (IS_SET(MODE_ALTSCREEN))
		for (; i >= 0 && !(line[i].mode & ATTR_WRAP) && line[i].u == ' '; i--);
	else
		for (; i >= 0 && !(line[i].mode & (ATTR_SET | ATTR_WRAP)); i--);

	return i + 1;
}

int
tiswrapped(Line line)
{
	int len = tlinelen(line);

	return len > 0 && (line[len - 1].mode & ATTR_WRAP);
}

char *
tgetglyphs(char *buf, const Glyph *gp, const Glyph *lgp)
{
	while (gp <= lgp)
		if (gp->mode & ATTR_WDUMMY) {
			gp++;
		} else {
			buf += utf8encode((gp++)->u, buf);
		}
	return buf;
}

size_t
tgetline(char *buf, const Glyph *fgp)
{
	char *ptr;
	const Glyph *lgp = &fgp[term.col - 1];

	while (lgp > fgp && !(lgp->mode & (ATTR_SET | ATTR_WRAP)))
		lgp--;
	ptr = tgetglyphs(buf, fgp, lgp);
	if (!(lgp->mode & ATTR_WRAP))
		*(ptr++) = '\n';
	return ptr - buf;
}

int
regionselected(int x1, int y1, int x2, int y2)
{
	if (sel.ob.x == -1 || sel.mode == SEL_EMPTY ||
	    sel.alt != IS_SET(MODE_ALTSCREEN) || sel.nb.y > y2 || sel.ne.y < y1)
		return 0;

	return (sel.type == SEL_RECTANGULAR) ? sel.nb.x <= x2 && sel.ne.x >= x1
		: (sel.nb.y != y2 || sel.nb.x <= x2) &&
		  (sel.ne.y != y1 || sel.ne.x >= x1);
}

int
selected(int x, int y)
{
	return regionselected(x, y, x, y);
}

void
selsnap(int *x, int *y, int direction)
{
	int newx, newy;
	int rtop = 0, rbot = term.row - 1;
	int delim, prevdelim, maxlen;
	const Glyph *gp, *prevgp;

	if (!IS_SET(MODE_ALTSCREEN))
		rtop += -term.histf + term.scr, rbot += term.scr;

	switch (sel.snap) {
	case SNAP_WORD:
		/*
		 * Snap around if the word wraps around at the end or
		 * beginning of a line.
		 */
		maxlen = (TLINE(*y)[term.col-2].mode & ATTR_WRAP) ? term.col-1 : term.col;
		LIMIT(*x, 0, maxlen - 1);
		prevgp = &TLINE(*y)[*x];
		prevdelim = ISDELIM(prevgp->u);
		for (;;) {
			newx = *x + direction;
			newy = *y;
			if (!BETWEEN(newx, 0, maxlen - 1)) {
				newy += direction;
				if (!BETWEEN(newy, rtop, rbot))
					break;

				if (!tiswrapped(TLINE(direction > 0 ? *y : newy)))
					break;

				maxlen = (TLINE(newy)[term.col-2].mode & ATTR_WRAP) ? term.col-1 : term.col;
				newx = direction > 0 ? 0 : maxlen - 1;
			}

			gp = &TLINE(newy)[newx];
			delim = ISDELIM(gp->u);
			if (!(gp->mode & ATTR_WDUMMY) && (delim != prevdelim
					|| (delim && gp->u != prevgp->u)))
				break;

			*x = newx;
			*y = newy;
			if (!(gp->mode & ATTR_WDUMMY)) {
				prevgp = gp;
				prevdelim = delim;
			}
		}
		break;
	case SNAP_LINE:
		/*
		 * Snap around if the the previous line or the current one
		 * has set ATTR_WRAP at its end. Then the whole next or
		 * previous line will be selected.
		 */
		*x = (direction < 0) ? 0 : term.col - 1;
		if (direction < 0) {
			for (; *y > rtop; *y -= 1) {
				if (!tiswrapped(TLINE(*y-1)))
					break;
			}
		} else if (direction > 0) {
			for (; *y < rbot; *y += 1) {
				if (!tiswrapped(TLINE(*y)))
					break;
			}
		}
		break;
	}
}

void
selscroll(int top, int bot, int n)
{
	/* turn absolute coordinates into relative */
	top += term.scr, bot += term.scr;

	if (BETWEEN(sel.nb.y, top, bot) != BETWEEN(sel.ne.y, top, bot)) {
		selclear();
		tsetdirt(top, bot); /* its rows inside the region have moved */
	} else if (BETWEEN(sel.nb.y, top, bot)) {
		selmove(n);
		if (sel.nb.y < top || sel.ne.y > bot)
			selclear();
	}
}

void
tswapscreen(void)
{
	static Line *altline;
	static int altcol, altrow;
	Line *tmpline = term.line;
	int tmpcol = term.col, tmprow = term.row;
	#if SIXEL_PATCH
	ImageList *im = term.images;
	#endif // SIXEL_PATCH

	term.line = altline;
	term.col = altcol, term.row = altrow;
	altline = tmpline;
	altcol = tmpcol, altrow = tmprow;
	term.mode ^= MODE_ALTSCREEN;

	#if SIXEL_PATCH
	term.images = term.images_alt;
	term.images_alt = im;
	#endif // SIXEL_PATCH
}

char *
getsel(void)
{
	char *str, *ptr;
	int x, y, firstx, lastx, linelen;
	const Glyph *gp, *lgp;

	if (sel.ob.x == -1 || sel.alt != IS_SET(MODE_ALTSCREEN))
		return NULL;

	str = xmalloc((term.col + 1) * (sel.ne.y - sel.nb.y + 1) * UTF_SIZ);
	ptr = str;

	/* append every set & selected glyph to the selection */
	for (y = sel.nb.y; y <= sel.ne.y; y++) {
		LineView view = tlineview(y);

		if ((linelen = tviewlen(view)) == 0) {
			*ptr++ = '\n';
			continue;
		}

		if (sel.type == SEL_RECTANGULAR) {
			firstx = sel.nb.x;
			lastx = sel.ne.x;
		} else {
			firstx = sel.nb.y == y ? sel.nb.x : 0;
			lastx = (sel.ne.y == y) ? sel.ne.x : term.col-1;
		}
		lgp = tviewcell(view, MIN(lastx, linelen-1));

		for (x = firstx; x <= MIN(lastx, linelen-1); x++) {
			gp = tviewcell(view, x);
			if (!(gp->mode & ATTR_WDUMMY))
				ptr += utf8encode(gp->u, ptr);
		}
		/*
		 * Copy and pasting of line endings is inconsistent
		 * in the inconsistent terminal and GUI world.
		 * The best solution seems like to produce '\n' when
		 * something is copied from st and convert '\n' to
		 * '\r', when something to be pasted is received by
		 * st.
		 * FIXME: Fix the computer world.
		 */
		if ((y < sel.ne.y || lastx >= linelen) &&
		    (!(lgp->mode & ATTR_WRAP) || sel.type == SEL_RECTANGULAR))
			*ptr++ = '\n';
	}
	*ptr = '\0';
	return str;
}

void
tdumpline(int n)
{
	char str[(term.col + 1) * UTF_SIZ];

	tprinter(str, tgetline(str, &term.line[n][0]));
}
