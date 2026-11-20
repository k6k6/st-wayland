/* See LICENSE for license details.
 * Software cursor trail following kitty's per-frame exponential corner chase
 * (kitty/cursor_trail.c), in normalized viewport coordinates. Only the four
 * moving corners are painted; the live cursor rectangle is masked out.
 * Included only by wl.c, not the retained X11 backend.
 */
typedef struct { double x, y; } TrailPoint;
static struct {
	TrailPoint corner[4], target[4];
	float ndc[4][2];
	struct timespec moved, updated;
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

/* Reasons to forget the recorded cursor outright: it is not drawn at all, or
 * its cell no longer marks the same place. MODE_HIDE is deliberately absent:
 * it is a per-frame decision, not a state change. TUIs such as Neovim bracket
 * almost every redraw with "\033[?25l ... \033[?25h", so a frame that lands
 * inside such a bracket must keep the position the next visible frame has to
 * animate from - forgetting it turns the jump into a snap. */
static int
trailgone(void)
{
	if (!cursortrail || !IS_SET(MODE_VISIBLE) || !IS_SET(MODE_FOCUSED) ||
	    ime.preedit || win.cursor > 6)
		return 1;
	#if SCROLLBACK_PATCH || REFLOW_PATCH
	if (term.scr)
		return 1;
	#endif
	#if BLINKING_CURSOR_PATCH
	if (IS_SET(MODE_BLINK) && (win.cursor == 0 || win.cursor == 1 ||
	    win.cursor == 3 || win.cursor == 5))
		return 1;
	#endif
	return 0;
}

/* May the trail be painted in this frame? */
static int
trailvisible(void)
{
	return !trailgone() && !IS_SET(MODE_HIDE);
}

static int
trailneedsframe(void)
{
	return IS_SET(MODE_VISIBLE) && (trail.active || trail.painted);
}

static void
trailprepare(void)
{
	int top, y1, y2, y, row;

	/* Called only AFTER startframe succeeds. Thus animation-only dirty bits
	 * can never leak into tty parsing (notably sixel pruning) while waiting
	 * for a free buffer. The ordinary row redraw restores ligatures too. */
	trail.seen = 0;
	if (trailgone())
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
	for (y = y1; y <= y2; y++) {
		/* scrollpixels() is about to move the rows it was painted on */
		if ((row = scrolledrow(y)) >= 0)
			term.dirty[row] = 1;
	}
	trail.painted = 0;
}

static void
trailcapture(void)
{
	for (int i = 0; i < 4; i++) {
		trail.ndc[i][0] = -1.0f + (float)trail.corner[i].x * (2.0f / win.w);
		trail.ndc[i][1] = 1.0f - (float)trail.corner[i].y * (2.0f / win.h);
	}
}

static void
trailadvance(const struct timespec *nowp)
{
	struct timespec now = *nowp;
	float target[4][2], dx[4], dy[4], dot[4], min = INFINITY, max = -INFINITY;
	float sx, sy, x, y, radius, fast, slow, dt, decay, step;
	int i;

	if (!trail.active || TIMEDIFF(now, trail.updated) <= 0)
		return;
	/* Keep kitty's NDC float state as well as the pixel drawing coordinates.
	 * Round-tripping pixels each frame perturbs normalized dot products near
	 * convergence, which can change the fast/slow corner assignment. */
	sx = 2.0f / win.w;
	sy = 2.0f / win.h;
	for (i = 0; i < 4; i++) {
		target[i][0] = -1.0f + (float)trail.target[i].x * sx;
		target[i][1] = 1.0f - (float)trail.target[i].y * sy;
	}
	x = (target[0][0] + target[2][0]) * 0.5f;
	y = (target[0][1] + target[2][1]) * 0.5f;
	dx[0] = target[2][0] - target[0][0];
	dy[0] = target[2][1] - target[0][1];
	radius = sqrtf(dx[0] * dx[0] + dy[0] * dy[0]) * 0.5f;
	fast = (isfinite(cursortraildecay[0]) ?
		MAX(1.0, cursortraildecay[0]) : 100.0) / 1000.0;
	slow = (isfinite(cursortraildecay[1]) ?
		MAX(fast * 1000.0, cursortraildecay[1]) : 400.0) / 1000.0;
	slow = MAX(fast, slow);
	dt = MAX(0.0, TIMEDIFF(now, trail.updated)) / 1000.0;
	for (i = 0; i < 4; i++) {
		dx[i] = target[i][0] - trail.ndc[i][0];
		dy[i] = target[i][1] - trail.ndc[i][1];
		if ((fabsf(dx[i]) < 1e-6f && fabsf(dy[i]) < 1e-6f) ||
		    radius == 0.0f) {
			dx[i] = dy[i] = dot[i] = 0.0f;
		} else {
			dot[i] = (dx[i] * (target[i][0] - x) +
				dy[i] * (target[i][1] - y)) / radius /
				sqrtf(dx[i] * dx[i] + dy[i] * dy[i]);
		}
		min = fminf(min, dot[i]);
		max = fmaxf(max, dot[i]);
	}
	trail.active = 0;
	for (i = 0; i < 4; i++) {
		decay = min == max ? slow : slow +
			(fast - slow) * (dot[i] - min) / (max - min);
		step = 1.0f - exp2f(-10.0f * dt / decay);
		trail.ndc[i][0] += dx[i] * step;
		trail.ndc[i][1] += dy[i] * step;
		trail.corner[i].x = (trail.ndc[i][0] + 1.0f) * (win.w / 2.0);
		trail.corner[i].y = (1.0f - trail.ndc[i][1]) * (win.h / 2.0);
		/* kitty stops when both axes of every corner are below 0.5 px. */
		if (fabsf(target[i][0] - trail.ndc[i][0]) >= sx * 0.5f ||
		    fabsf(target[i][1] - trail.ndc[i][1]) >= sy * 0.5f)
			trail.active = 1;
	}
	trail.updated = now;
}

static void
trailupdate(int cx, int cy, Glyph g, Color color, const struct timespec *nowp)
{
	struct timespec now = *nowp;
	TrailPoint target[4];
	double x, y, w, h, dwell;
	int i, moved, context, restyle, screen,
		wide = !!(g.mode & ATTR_WIDE), keyboard = 0;

	trail.seen = 1;
	if (!trailvisible()) {
		/* drawcursor() returns before reaching us while the cursor is
		 * hidden, so this only catches the "gone" cases; a hidden frame
		 * keeps the recorded position for the frame that shows it again. */
		if (trailgone())
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
	/* Snapping contexts: a cell metric change (resize, zoom, font reload)
	 * makes the recorded pixel coordinates meaningless. A screen switch is
	 * NOT one of them: window geometry is untouched, so the previous cursor
	 * still marks a real pixel and the jump onto the other screen (a TUI
	 * taking over or handing the screen back) is exactly the kind of
	 * deliberate move a trail should show. */
	context = !trail.valid || trail.cw != win.cw || trail.ch != win.ch;
	screen = trail.valid && trail.screen != term.line;
	/* A cursor shape change (DECSCUSR) is not a snapping context: TUIs such
	 * as Neovim emit it in the same write as the cursor move that belongs to
	 * the new mode, so the move must still animate. Only a shape change that
	 * comes alone rectifies the resting corner, without a trail. The same
	 * write also bypasses the dwell/threshold skip below: a mode switch is a
	 * deliberate one-cell jump, not the machine-driven churn those guard
	 * against. */
	restyle = trail.valid && (trail.style != win.cursor ||
		trail.wide != wide || trail.keyboard != keyboard);
	moved = trail.cx != cx || trail.cy != cy;
	dwell = TIMEDIFF(now, trail.moved);
	/* A screen switch belongs with restyle: a deliberate one-shot jump, not
	 * the machine-driven churn the dwell/threshold skip guards against. */
	if (context || (moved && !restyle && !screen && !trail.active &&
	    (dwell < cursortrail ||
	     (abs(cx - trail.cx) < cursortrailthreshold &&
	      abs(cy - trail.cy) < cursortrailthreshold)))) {
		memcpy(trail.corner, target, sizeof target);
		trailcapture();
		trail.active = 0;
	} else if (moved) {
		/* Preserve the existing start timing: idle time is dwell, not time
		 * spent chasing. In flight, retain the last rendered timestamp and
		 * chase the new target without restarting a fixed-duration ease. */
		if (!trail.active) {
			trailcapture();
			trail.updated = now;
		}
		trail.active = 1;
	} else if (restyle) {
		memcpy(trail.corner, target, sizeof target);
		trailcapture();
		trail.active = 0;
	}
	if (!trail.active)
		trail.updated = now;
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

/* Match kitty's triangle fan, including folded corners on reversals.
 * Both triangles accumulate into one coverage mask before blending. */
static pixman_triangle_t
trailtriangle(int a, int b, int c)
{
	TrailPoint p = trail.corner[a], q = trail.corner[b], r = trail.corner[c];
	return (pixman_triangle_t){
		{pixman_double_to_fixed(p.x), pixman_double_to_fixed(p.y)},
		{pixman_double_to_fixed(q.x), pixman_double_to_fixed(q.y)},
		{pixman_double_to_fixed(r.x), pixman_double_to_fixed(r.y)}};
}

static void
trailpaint(const struct timespec *nowp)
{
	double left, top, right, bottom;
	int i, bw, bh;

	if (trailgone() || (!trail.seen && !IS_SET(MODE_HIDE))) {
		/* Not drawn at all any more, or visible yet skipped by this frame
		 * (scrollback, keyboard selection): nothing can be trusted about
		 * where the cursor is. */
		trail.painted = 0;
		trailreset();
		return;
	}
	if (!trailvisible()) {
		/* Cursor hidden for this frame: trailprepare() already dirtied the
		 * old trail rows, so erase it here but keep the recorded position.
		 * The animation is dropped (it could not be seen) and the resting
		 * corner aligned to the recorded target, so nothing asks for the
		 * frames that would never paint anything. */
		memcpy(trail.corner, trail.target, sizeof trail.corner);
		trail.active = 0;
		trail.painted = 0;
		return;
	}
	trailadvance(nowp);
	if (!trail.active || !xw.pix)
		return;
	#if ANYSIZE_PATCH
	bw = win.hborderpx; bh = win.vborderpx;
	#else
	bw = bh = borderpx;
	#endif
	left = right = trail.corner[0].x;
	top = bottom = trail.corner[0].y;
	for (i = 1; i < 4; i++) {
		left = MIN(left, trail.corner[i].x); right = MAX(right, trail.corner[i].x);
		top = MIN(top, trail.corner[i].y); bottom = MAX(bottom, trail.corner[i].y);
	}
	trail.x1 = MAX(bw, (int)floor(left));
	trail.y1 = MAX(bh, (int)floor(top));
	trail.x2 = MIN(MIN(win.w, bw + win.tw), (int)ceil(right));
	trail.y2 = MIN(MIN(win.h, bh + win.th), (int)ceil(bottom));
	if (trail.x1 >= trail.x2 || trail.y1 >= trail.y2)
		return;
	pixman_region32_t clip, cursor;
	pixman_triangle_t triangles[] = {trailtriangle(1, 2, 3), trailtriangle(1, 3, 0)};
	pixman_color_t color = {trail.color.color.red, trail.color.color.green,
		trail.color.color.blue, 0xffff};
	pixman_image_t *solid = pixman_image_create_solid_fill(&color);
	if (!solid)
		die("could not allocate cursor trail color\n");
	pixman_region32_init_rect(&clip, trail.x1, trail.y1,
		trail.x2 - trail.x1, trail.y2 - trail.y1);
	/* The live cursor edges are integer cell coordinates. Exclude only that
	 * rectangle, including thin underline/bar styles, as kitty does. */
	pixman_region32_init_rect(&cursor, (int)trail.target[0].x, (int)trail.target[0].y,
		(int)(trail.target[2].x - trail.target[0].x),
		(int)(trail.target[2].y - trail.target[0].y));
	if (!pixman_region32_subtract(&clip, &clip, &cursor) ||
	    !pixman_image_set_clip_region32(xw.pix, &clip))
		die("could not allocate cursor trail clip\n");
	pixman_composite_triangles(PIXMAN_OP_OVER, solid, xw.pix, PIXMAN_a8,
		0, 0, 0, 0, LEN(triangles), triangles);
	pixman_image_set_clip_region32(xw.pix, NULL);
	pixman_region32_fini(&cursor);
	pixman_region32_fini(&clip);
	pixman_image_unref(solid);
	adddamage(trail.x1, trail.y1, trail.x2 - trail.x1, trail.y2 - trail.y1);
	trail.painted = 1;
}
