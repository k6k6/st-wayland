/* See LICENSE for license details.
 * Original software cursor trail: time-based corner easing, with the target
 * rectangle included in a convex hull so the trail never detaches from it.
 * Included only by wl.c, not the retained X11 backend. No heap allocations.
 */
typedef struct { double x, y; } TrailPoint;
static struct {
	TrailPoint from[4], corner[4], target[4];
	double duration[4];
	struct timespec moved;
	Color color;
	Line *screen;
	int valid, active, seen, painted;
	int cx, cy, cw, ch, style, wide, keyboard;
	int x1, y1, x2, y2;
} trail;

static void
trailreset(void)
{
	/* Keep the painted bounds until a successful frame can erase them. */
	trail.valid = trail.active = 0;
}

static int
trailvisible(void)
{
	if (!cursortrail || !IS_SET(MODE_VISIBLE) || !IS_SET(MODE_FOCUSED) ||
	    IS_SET(MODE_HIDE) || ime.preedit || win.cursor > 6)
		return 0;
	#if SCROLLBACK_PATCH || REFLOW_PATCH
	if (term.scr)
		return 0;
	#endif
	#if BLINKING_CURSOR_PATCH
	if (IS_SET(MODE_BLINK) && (win.cursor == 0 || win.cursor == 1 ||
	    win.cursor == 3 || win.cursor == 5))
		return 0;
	#endif
	return 1;
}

static int
trailneedsframe(void)
{
	return IS_SET(MODE_VISIBLE) && (trail.active || trail.painted);
}

static void
trailprepare(void)
{
	int top, y1, y2, y;

	/* Called only AFTER startframe succeeds. Thus animation-only dirty bits
	 * can never leak into tty parsing (notably sixel pruning) while waiting
	 * for a free buffer. The ordinary row redraw restores ligatures too. */
	trail.seen = 0;
	if (!trailvisible())
		trailreset();
	if (!trail.painted || !term.dirty || term.row <= 0 || win.ch <= 0)
		return;
	#if ANYSIZE_PATCH
	top = win.vborderpx;
	#else
	top = borderpx;
	#endif
	y1 = MAX(0, (trail.y1 - top) / win.ch);
	y2 = MIN(term.row - 1, (trail.y2 - 1 - top) / win.ch);
	for (y = y1; y <= y2; y++)
		term.dirty[y] = 1;
	trail.painted = 0;
}

static void
trailadvance(const struct timespec *nowp)
{
	struct timespec now = *nowp;
	double elapsed = MAX(0.0, TIMEDIFF(now, trail.moved)), t, ease;
	int i;

	if (!trail.active)
		return;
	trail.active = 0;
	for (i = 0; i < 4; i++) {
		t = MIN(1.0, elapsed / trail.duration[i]);
		ease = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
		trail.corner[i].x = trail.from[i].x +
			(trail.target[i].x - trail.from[i].x) * ease;
		trail.corner[i].y = trail.from[i].y +
			(trail.target[i].y - trail.from[i].y) * ease;
		if (t < 1.0)
			trail.active = 1;
	}
}

static void
trailupdate(int cx, int cy, Glyph g, Color color, const struct timespec *nowp)
{
	struct timespec now = *nowp;
	TrailPoint target[4];
	double x, y, w, h, dx, dy, radius, projection, fast, slow, dwell;
	int i, moved, context, wide = !!(g.mode & ATTR_WIDE), keyboard = 0;

	trail.seen = 1;
	if (!trailvisible()) {
		trailreset();
		return;
	}
	#if KEYBOARDSELECT_PATCH
	keyboard = IS_SET(MODE_KBDSELECT);
	#endif
	#if ANYSIZE_PATCH
	x = win.hborderpx + cx * win.cw;
	y = win.vborderpx + cy * win.ch;
	#else
	x = borderpx + cx * win.cw;
	y = borderpx + cy * win.ch;
	#endif
	w = (wide ? 2 : 1) * win.cw;
	h = win.ch;
	if (win.cursor == 3 || win.cursor == 4) {
		h = MIN(win.ch, cursorthickness);
		y += win.ch - h;
		w = win.cw;
	} else if (win.cursor == 5 || win.cursor == 6) {
		w = MIN(win.cw, cursorthickness);
	}
	target[0] = (TrailPoint){x, y};
	target[1] = (TrailPoint){x + w, y};
	target[2] = (TrailPoint){x + w, y + h};
	target[3] = (TrailPoint){x, y + h};
	context = !trail.valid || trail.screen != term.line ||
		trail.cw != win.cw || trail.ch != win.ch ||
		trail.style != win.cursor || trail.wide != wide ||
		trail.keyboard != keyboard;
	moved = trail.cx != cx || trail.cy != cy;
	dwell = TIMEDIFF(now, trail.moved);
	trailadvance(&now);
	if (context || (moved && !trail.active &&
	    (dwell < cursortrail ||
	     (abs(cx - trail.cx) < cursortrailthreshold &&
	      abs(cy - trail.cy) < cursortrailthreshold)))) {
		memcpy(trail.corner, target, sizeof target);
		trail.active = 0;
	} else if (moved) {
		dx = x - trail.target[0].x;
		dy = y - trail.target[0].y;
		radius = (fabs(dx) * w + fabs(dy) * h) / 2.0;
		fast = isfinite(cursortraildecay[0]) ? MAX(1.0, cursortraildecay[0]) : 100.0;
		slow = isfinite(cursortraildecay[1]) ? MAX(fast, cursortraildecay[1]) : fast;
		for (i = 0; i < 4; i++) {
			projection = radius > 0 ?
				((target[i].x - x - w / 2) * dx +
				 (target[i].y - y - h / 2) * dy) / radius : 0;
			LIMIT(projection, -1.0, 1.0);
			trail.duration[i] = fast + (slow - fast) * (1 - projection) / 2;
		}
		memcpy(trail.from, trail.corner, sizeof trail.from);
		trail.active = 1;
	}
	if (context || moved)
		trail.moved = now;
	memcpy(trail.target, target, sizeof target);
	trail.valid = 1;
	trail.screen = term.line;
	trail.cw = win.cw;
	trail.ch = win.ch;
	trail.style = win.cursor;
	trail.wide = wide;
	trail.keyboard = keyboard;
	trail.cx = cx;
	trail.cy = cy;
	trail.color = color;
}

static double
trailcross(TrailPoint a, TrailPoint b, TrailPoint c)
{
	return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

static void
trailpaint(const struct timespec *nowp)
{
	TrailPoint points[8], hull[16], p;
	double left, top, right, bottom, error = 0, opacity;
	int i, j, n = 0, lower, bw, bh;

	if (!trail.seen || !trailvisible()) {
		trailreset();
		return;
	}
	trailadvance(nowp);
	if (!trail.active || !xw.draw)
		return;
	for (i = 0; i < 4; i++) {
		points[i] = trail.corner[i];
		points[i + 4] = trail.target[i];
		error = MAX(error, hypot(points[i].x - points[i + 4].x,
			points[i].y - points[i + 4].y));
	}
	/* Monotone convex hull of only eight points: bounded stack storage. */
	for (i = 1; i < 8; i++) {
		p = points[i];
		for (j = i; j > 0 && (points[j-1].x > p.x ||
		    (points[j-1].x == p.x && points[j-1].y > p.y)); j--)
			points[j] = points[j-1];
		points[j] = p;
	}
	for (i = 0; i < 8; i++) {
		while (n >= 2 && trailcross(hull[n-2], hull[n-1], points[i]) <= 0)
			n--;
		hull[n++] = points[i];
	}
	lower = n + 1;
	for (i = 6; i >= 0; i--) {
		while (n >= lower && trailcross(hull[n-2], hull[n-1], points[i]) <= 0)
			n--;
		hull[n++] = points[i];
	}
	if (--n < 3)
		return;
	#if ANYSIZE_PATCH
	bw = win.hborderpx; bh = win.vborderpx;
	#else
	bw = bh = borderpx;
	#endif
	left = right = hull[0].x;
	top = bottom = hull[0].y;
	for (i = 1; i < n; i++) {
		left = MIN(left, hull[i].x); right = MAX(right, hull[i].x);
		top = MIN(top, hull[i].y); bottom = MAX(bottom, hull[i].y);
	}
	trail.x1 = MAX(bw, (int)floor(left));
	trail.y1 = MAX(bh, (int)floor(top));
	trail.x2 = MIN(MIN(win.w, bw + win.tw), (int)ceil(right));
	trail.y2 = MIN(MIN(win.h, bh + win.th), (int)ceil(bottom));
	if (trail.x1 >= trail.x2 || trail.y1 >= trail.y2)
		return;
	opacity = 0.65 * MIN(1.0, error / MAX(win.cw, win.ch));
	resetclip();
	cairo_save(xw.draw);
	/* Exclude the entire live cursor cell, not just the thin bar. This
	 * preserves the immediate cursor, glyphs and IME overlay without seams. */
	cairo_rectangle(xw.draw, bw, bh, win.tw, win.th);
	cairo_rectangle(xw.draw, bw + trail.cx * win.cw,
		bh + trail.cy * win.ch, (trail.wide ? 2 : 1) * win.cw, win.ch);
	cairo_set_fill_rule(xw.draw, CAIRO_FILL_RULE_EVEN_ODD);
	cairo_clip(xw.draw);
	cairo_set_fill_rule(xw.draw, CAIRO_FILL_RULE_WINDING);
	cairo_set_antialias(xw.draw, CAIRO_ANTIALIAS_DEFAULT);
	cairo_set_operator(xw.draw, CAIRO_OPERATOR_OVER);
	cairo_set_source_rgba(xw.draw, trail.color.color.red / 65535.0,
		trail.color.color.green / 65535.0, trail.color.color.blue / 65535.0,
		opacity * trail.color.color.alpha / 65535.0);
	cairo_move_to(xw.draw, hull[0].x, hull[0].y);
	for (i = 1; i < n; i++)
		cairo_line_to(xw.draw, hull[i].x, hull[i].y);
	cairo_close_path(xw.draw);
	cairo_fill(xw.draw);
	cairo_restore(xw.draw);
	adddamage(trail.x1, trail.y1, trail.x2 - trail.x1, trail.y2 - trail.y1);
	trail.painted = 1;
}
