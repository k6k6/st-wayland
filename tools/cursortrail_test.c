/* Headless regression of the real Wayland renderer and trail state machine.
 * Buffers have sentinel protocol handles; no Wayland requests are issued. */
#define main st_program_main
#include "../wl.c"
#undef main
#include <assert.h>

#if CURSOR_TRAIL_PATCH
#define COLS 24
#define ROWS 10
static int dirty[ROWS];
static Glyph cells[ROWS][COLS];
static Line lines[ROWS], otherlines[ROWS];
static const Glyph blank = {.u = ' ', .fg = 7, .bg = 0};
static Color red = {.color = {0xffff, 0, 0, 0xffff}, .pixel = 0xffff0000};

static struct timespec
stamp(long ms)
{
	return (struct timespec){.tv_sec = 100 + ms / 1000,
		.tv_nsec = (ms % 1000) * 1000000};
}

static void
update(int x, int y, long ms, Glyph g)
{
	struct timespec now = stamp(ms);
	trailupdate(x, y, g, red, &now);
}

static void
advance(long ms)
{
	struct timespec now = stamp(ms);
	trailadvance(&now);
}

static void
paint(long ms)
{
	struct timespec now = stamp(ms);
	trailpaint(&now);
	assert(cairo_status(xw.draw) == CAIRO_STATUS_SUCCESS);
}

static void
fresh(void)
{
	memset(&trail, 0, sizeof trail);
	memset(dirty, 0, sizeof dirty);
	win.mode = MODE_VISIBLE | MODE_FOCUSED;
	win.cursor = 2;
	cursortrail = 40;
	cursortrailthreshold = 2;
	cursortraildecay[0] = 100;
	cursortraildecay[1] = 400;
	term.line = lines;
	#if REFLOW_PATCH || SCROLLBACK_PATCH
	term.scr = 0;
	#endif
	ime.preedit = NULL;
}

static void
jump(void)
{
	fresh();
	update(2, 2, 0, blank);
	update(14, 6, 100, blank);
	assert(trail.valid && trail.active && trailneedsframe());
}

static void
settled(void)
{
	assert(!trail.active && !trailneedsframe());
	for (int i = 0; i < 4; i++) {
		assert(fabs(trail.corner[i].x - trail.target[i].x) < 0.5);
		assert(fabs(trail.corner[i].y - trail.target[i].y) < 0.5);
	}
}

static void
timing(void)
{
	TrailPoint expected[4];
	fresh();
	update(2, 2, 0, blank);
	assert(trail.valid && !trailneedsframe());
	update(14, 2, 20, blank); /* Insufficient dwell. */
	assert(!trail.active);
	update(15, 2, 120, blank); /* Ordinary single-cell typing. */
	assert(!trail.active);
	update(17, 2, 220, blank);
	assert(trail.active);
	advance(320);
	/* Leading corners converge faster, without a fixed arrival time. */
	assert(fabs(trail.corner[1].x - trail.target[1].x) < 0.5);
	assert(trail.target[0].x - trail.corner[0].x > 0.5);
	advance(620); settled();
	fresh(); update(2, 2, 0, blank); update(2, 4, 100, blank);
	assert(trail.active); /* Per-axis threshold, also vertical. */
	fresh(); cursortrail = 0;
	update(2, 2, 0, blank); update(20, 8, 100, blank);
	assert(!trail.valid && !trailneedsframe());

	jump(); advance(150);
	memcpy(expected, trail.corner, sizeof expected);
	/* Chase a new target from the last rendered positions, retaining the
	 * frame clock rather than restarting a fixed-duration interpolation. */
	update(1, 8, 160, blank);
	assert(trail.active && !memcmp(trail.corner, expected, sizeof expected));
	assert(trail.updated.tv_nsec == stamp(150).tv_nsec);
	advance(176);
	assert(memcmp(trail.corner, expected, sizeof expected));
	advance(800); settled();

	/* With equal decays the exponential is independent of frame partition. */
	jump(); cursortraildecay[0] = cursortraildecay[1] = 400;
	advance(250); memcpy(expected, trail.corner, sizeof expected);
	jump(); cursortraildecay[0] = cursortraildecay[1] = 400;
	for (int i = 110; i <= 250; i += 10) advance(i);
	for (int i = 0; i < 4; i++) {
		assert(fabs(trail.corner[i].x - expected[i].x) < 0.0001);
		assert(fabs(trail.corner[i].y - expected[i].y) < 0.0001);
	}
	/* A long dwell does not consume an animation before its first frame. */
	fresh(); update(2, 2, 0, blank);
	memcpy(expected, trail.corner, sizeof expected);
	update(14, 2, 10000, blank); advance(10000);
	assert(trail.active && !memcmp(trail.corner, expected, sizeof expected));
	advance(10016); assert(memcmp(trail.corner, expected, sizeof expected));

	fresh(); cursortraildecay[0] = NAN; cursortraildecay[1] = -10;
	update(2, 2, 0, blank); update(14, 2, 100, blank);
	advance(200); settled();
}

static void
contexts(void)
{
	/* Hiding the cursor is a per-frame decision, not a snapping context: TUIs
	 * hide it across redraws, so the recorded position (and an animation in
	 * flight) must survive a frame that lands inside the hidden phase. */
	jump(); win.mode |= MODE_HIDE; trailprepare();
	assert(trail.valid && trail.active);
	paint(180);
	assert(trail.valid && !trail.active && !trail.painted);
	assert(!trailneedsframe());
	win.mode &= ~MODE_HIDE;
	jump(); focusout(); assert(!trail.active && !trail.valid);
	jump(); ime.preedit = "preedit"; trailprepare();
	assert(!trail.active); ime.preedit = NULL;
	#if REFLOW_PATCH || SCROLLBACK_PATCH
	jump(); term.scr = 1; trailprepare(); assert(!trail.active);
	#endif
	/* A screen switch keeps the window geometry, so it is not a snapping
	 * context: the jump onto the other screen is animated. */
	jump(); term.line = otherlines; update(18, 8, 200, blank);
	assert(trail.active && trail.screen == otherlines);
	term.line = lines;
	jump(); win.cw++; update(18, 8, 200, blank);
	assert(!trail.active); win.cw--;
	jump(); xsetmode(1, MODE_HIDE); trailprepare();
	assert(trail.valid && trail.active); /* Hidden, not forgotten. */
	jump(); xsetmode(1, MODE_HIDE); xsetmode(0, MODE_HIDE);
	assert(trail.active); /* Batched hide/move/show must retain the trail. */
	/* DECSCUSR alone is not a snapping context either; restyles() covers it. */
	jump(); assert(!xsetcursor(4)); assert(trail.active && trail.valid);
	jump(); assert(!xsetcursor(2)); assert(trail.active);
	xsetmode(0, MODE_HIDE); assert(trail.active); /* Redundant TUI updates. */
	jump(); trailprepare(); /* Caller skipped xdrawcursor, e.g. scrollback. */
	paint(200); assert(!trail.active);
	#if KEYBOARDSELECT_PATCH
	/* Keyboard selection only changes the cursor's colour context, so the
	 * accompanying move is animated like any other restyle. */
	jump(); win.mode |= MODE_KBDSELECT; update(18, 8, 200, blank);
	assert(trail.active && trail.keyboard);
	#endif
	#if BLINKING_CURSOR_PATCH
	jump(); win.cursor = 1; win.mode |= MODE_BLINK;
	assert(!trailvisible()); trailprepare(); assert(!trail.active);
	win.cursor = 2; assert(trailvisible());
	#endif
	jump(); win.mode &= ~MODE_VISIBLE; assert(!trailneedsframe());
}

static void
screenentry(void)
{
	TrailPoint old[4];
	int i;

	/* Entering a full screen TUI is the moment the trail is meant to be seen:
	 * "\033[?1049h\033[H\033[2J" swaps to the alternate screen, positions the
	 * cursor somewhere else and the trail has to fly from the shell cursor to
	 * the new cell. It used to snap, because term.line changed. */
	fresh(); update(5, 9, 0, blank);
	memcpy(old, trail.target, sizeof old);
	term.line = otherlines;
	update(6, 1, 10, blank); /* dwell 10 ms < cursortrail 40: skip guard */
	assert(trail.active && trailneedsframe() && trail.screen == otherlines);
	for (i = 0; i < 4; i++) {
		assert(fabs(trail.corner[i].x - old[i].x) < 1e-9);
		assert(fabs(trail.corner[i].y - old[i].y) < 1e-9);
	}
	advance(600);
	assert(!trail.active && !trailneedsframe());

	/* The threshold guard is bypassed too: a settlement long enough to pass
	 * the dwell rule, but only one cell apart across the screen swap. */
	fresh(); update(5, 4, 0, blank);
	memcpy(old, trail.target, sizeof old);
	term.line = otherlines;
	update(6, 4, 200, blank);
	assert(trail.active);
	for (i = 0; i < 4; i++)
		assert(fabs(trail.corner[i].x - old[i].x) < 1e-9);
	advance(600);
	assert(!trail.active);

	/* Leaving the TUI animates back to the shell prompt just the same. */
	fresh(); update(20, 3, 0, blank);
	memcpy(old, trail.target, sizeof old);
	term.line = otherlines;
	update(3, 9, 20, blank);
	assert(trail.active && trail.screen == otherlines);
	for (i = 0; i < 4; i++)
		assert(fabs(trail.corner[i].x - old[i].x) < 1e-9);
	advance(600);
	assert(!trail.active);

	/* Neovim brackets the redraw with "\033[?25l" / "\033[?25h" and moves the
	 * cursor in between. A frame that lands in the hidden phase must neither
	 * paint nor forget the shell position, so the frame that shows the cursor
	 * again still animates the whole distance. */
	fresh(); update(5, 9, 0, blank);
	memcpy(old, trail.target, sizeof old);
	win.mode |= MODE_HIDE;
	term.line = otherlines;
	trailprepare();
	paint(80);
	assert(trail.valid && !trail.painted && !trail.active);
	assert(!trailneedsframe());
	win.mode &= ~MODE_HIDE;
	update(20, 3, 100, blank);
	assert(trail.active && trailneedsframe());
	for (i = 0; i < 4; i++) {
		assert(fabs(trail.corner[i].x - old[i].x) < 1e-9);
		assert(fabs(trail.corner[i].y - old[i].y) < 1e-9);
	}
	advance(600);
	assert(!trail.active);
	term.line = lines;
}

static void
restyles(void)
{
	TrailPoint bar[4];
	int i, ox = 14, oy = 6;

	/* Neovim emits DECSCUSR in the same write as the cursor move of the new
	 * mode: "\033[2 q\033[1;15H" leaving insert mode, "\033[6 q" when
	 * entering it. Both moves must animate, starting from the old shape. */
	fresh(); win.cursor = 6;
	update(ox, oy, 0, blank);
	assert(trail.valid && !trail.active);
	assert(trail.target[1].x - trail.target[0].x == cursorthickness);
	memcpy(bar, trail.corner, sizeof bar);
	assert(!xsetcursor(2)); /* \033[2 q */
	update(ox - 1, oy, 200, blank);
	assert(trail.active && trailneedsframe());
	for (i = 0; i < 4; i++) {
		assert(fabs(trail.corner[i].x - bar[i].x) < 1e-9);
		assert(fabs(trail.corner[i].y - bar[i].y) < 1e-9);
	}
	/* Bar -> block: the new target is one full cell wide, one cell left. */
	assert(trail.target[1].x - trail.target[0].x == win.cw);
	assert(trail.target[2].y - trail.target[1].y == win.ch);
	advance(300);
	/* Moving left: leading corners converge sooner than the trailing edge. */
	assert(fabs(trail.corner[0].x - trail.target[0].x) < 0.5);
	assert(fabs(trail.corner[0].x - (bar[0].x - win.cw)) < 0.5);
	assert(trail.corner[1].x > trail.target[1].x);
	assert(trail.corner[1].x <= bar[1].x);
	advance(600);
	assert(!trail.active && !trailneedsframe());
	for (i = 0; i < 4; i++) {
		assert(fabs(trail.corner[i].x - trail.target[i].x) < 0.5);
		assert(fabs(trail.corner[i].y - trail.target[i].y) < 0.5);
	}

	/* The reverse direction: block -> bar, moving right. */
	assert(!xsetcursor(6));
	update(ox, oy, 900, blank);
	assert(trail.active);
	assert(fabs(trail.corner[2].y - trail.corner[1].y - win.ch) < 0.0001);
	assert(trail.target[1].x - trail.target[0].x == cursorthickness);
	advance(1300);
	assert(!trail.active);
	for (i = 0; i < 4; i++) {
		assert(fabs(trail.corner[i].x - trail.target[i].x) < 0.5);
		assert(fabs(trail.corner[i].y - trail.target[i].y) < 0.5);
	}

	/* A shape-only change draws no trail, but must not stall a later move. */
	fresh(); update(ox, oy, 0, blank);
	win.cursor = 6; /* e.g. a TUI editor applying a configured shape */
	update(ox, oy, 100, blank);
	assert(!trail.active && trail.valid);
	assert(trail.corner[1].x - trail.corner[0].x == cursorthickness);
	update(ox + 4, oy, 300, blank);
	assert(trail.active);
}

static void
shapes(void)
{
	Glyph wide = blank;
	wide.mode = ATTR_WIDE;
	fresh(); update(2, 2, 0, wide);
	assert(trail.target[1].x - trail.target[0].x == 2 * win.cw);
	assert(trail.target[2].y - trail.target[1].y == win.ch);
	fresh(); win.cursor = 6; update(2, 2, 0, wide);
	assert(trail.target[1].x - trail.target[0].x == cursorthickness);
	fresh(); win.cursor = 4; update(2, 2, 0, blank);
	assert(trail.target[1].x - trail.target[0].x == win.cw);
	assert(trail.target[2].y - trail.target[1].y == cursorthickness);
	fresh(); win.cursor = 7; update(2, 2, 0, blank);
	assert(!trail.valid); /* Special st glyph cursor is left alone. */
}

static uint32_t
pixel(int x, int y)
{
	cairo_surface_flush(xw.buf);
	return ((uint32_t *)(cairo_image_surface_get_data(xw.buf) +
		y * cairo_image_surface_get_stride(xw.buf)))[x];
}

static void
clearbuffer(void)
{
	Color c = {.color = {0x1111, 0x2222, 0x3333, 0xffff}, .pixel = 0xff112233};
	wlfillrect(&c, 0, 0, win.w, win.h);
}

static void
pixelsandcleanup(void)
{
	int bw = borderpx, bh = borderpx, changed = 0, first, last;
	#if ANYSIZE_PATCH
	bw = win.hborderpx; bh = win.vborderpx;
	#endif
	clearbuffer(); jump(); paint(130);
	assert(trail.painted && trailneedsframe());
	int damaged = 0;
	for (unsigned int i = 0; i < buffers[0].damage.count; i++) {
		DamageRect r = buffers[0].damage.rects[i];
		if (r.x1 <= trail.x1 && r.y1 <= trail.y1 &&
		    r.x2 >= trail.x2 && r.y2 >= trail.y2)
			damaged = 1;
	}
	assert(damaged);
	for (int y = 0; y < win.h; y++) {
		for (int x = 0; x < win.w; x++) {
			int target = x >= bw + 14 * win.cw && x < bw + 15 * win.cw &&
				y >= bh + 6 * win.ch && y < bh + 7 * win.ch;
			uint32_t p = pixel(x, y);
			if (target || x < bw || x >= bw + win.tw ||
			    y < bh || y >= bh + win.th) assert(p == 0xff112233);
			if (p != 0xff112233) changed++;
		}
	}
	assert(changed > win.cw * win.ch);
	first = (trail.y1 - bh) / win.ch;
	last = (trail.y2 - 1 - bh) / win.ch;
	trailreset(); assert(trailneedsframe());
	trailprepare();
	assert(!trail.painted && !trailneedsframe());
	for (int y = 0; y < ROWS; y++) assert(dirty[y] == (y >= first && y <= last));
	/* Settle after a painted frame: exactly one restoration, then idle. */
	jump(); paint(130); trailprepare(); update(14, 6, 500, blank); paint(500);
	assert(!trail.active && !trail.painted && !trailneedsframe());
}

#ifdef CURSORTRAIL_KITTY_REFERENCE
#include "cursortrail-reference.h"

static void
kittyreference(void)
{
	const int map[] = {1, 2, 3, 0}; /* kitty TR BR BL TL -> st TL TR BR BL */
	const long periods[] = {16666667, 6944444, 11000000};
	const int targets[][2] = {{20, 2}, {2, 8}, {20, 8}};
	const int widths[] = {win.w, 1920, 1080}, heights[] = {win.h, 1080, 1920};
	int savedw = win.w, savedh = win.h, frames = 0;
	double maxerror = 0;
	KittyReferenceScreen screen = {
		.cell_size = {win.cw, win.ch}, .modes = {true}
	};
	KittyReferenceWindow window = {.render_data = {&screen}};
	KittyReferenceOSWindow os = {0};

	for (int aspect = 0; aspect < 3; aspect++) {
		win.w = widths[aspect]; win.h = heights[aspect];
		for (int shape = 2; shape <= 6; shape += 2)
		for (int rate = 0; rate < 3; rate++)
		for (int move = 0; move < 3; move++) {
			fresh(); win.cursor = shape;
			update(2, 2, 0, blank);
			update(targets[move][0], targets[move][1], 100, blank);
			KittyReferenceTrail ref = {.opacity = 1, .needs_render = true,
				.updated_at = 100100000000LL};
			for (int i = 0; i < 4; i++) {
				ref.corner_x[i] = -1.f + (float)trail.corner[map[i]].x * (2.f / win.w);
				ref.corner_y[i] = 1.f - (float)trail.corner[map[i]].y * (2.f / win.h);
			}
			kitty_reference_options.cursor_trail_decay_fast = cursortraildecay[0] / 1000;
			kitty_reference_options.cursor_trail_decay_slow = cursortraildecay[1] / 1000;
			int64_t ns = 100100000000LL;
			for (int frame = 0; frame < 200; frame++) {
				/* The third schedule deliberately varies frame intervals. */
				ns += periods[rate] + (rate == 2 ? (frame % 3) * 4000000 : 0);
				struct timespec now = {.tv_sec = ns / 1000000000,
					.tv_nsec = ns % 1000000000};
				if (frame == 3 || frame == 7) {
					/* Mid-flight reversal and another turn; keep both clocks. */
					trailupdate(frame == 3 ? 3 : 18, frame == 3 ? 7 : 3,
						blank, red, &now);
				}
				ref.cursor_edge_x[0] = -1.f + (float)trail.target[0].x * (2.f / win.w);
				ref.cursor_edge_x[1] = -1.f + (float)trail.target[2].x * (2.f / win.w);
				ref.cursor_edge_y[0] = 1.f - (float)trail.target[0].y * (2.f / win.h);
				ref.cursor_edge_y[1] = 1.f - (float)trail.target[2].y * (2.f / win.h);
				ndc_coords g = {.dx = 2.f * win.cw / win.w,
					.dy = 2.f * win.ch / win.h};
				update_cursor_trail_corners(&ref, g, ns, &os, &window);
				ref.updated_at = ns;
				update_cursor_trail_needs_render(&ref, &window, g);
				trailadvance(&now);
				for (int i = 0; i < 4; i++) {
					double x = (ref.corner_x[i] + 1.f) * (win.w / 2.0);
					double y = (1.f - ref.corner_y[i]) * (win.h / 2.0);
					double error = MAX(fabs(trail.corner[map[i]].x - x),
						fabs(trail.corner[map[i]].y - y));
					maxerror = MAX(error, maxerror);
					if (error >= 0.001)
						fprintf(stderr, "reference mismatch aspect=%d shape=%d rate=%d move=%d frame=%d corner=%d error=%.9f st=(%.9f,%.9f) kitty=(%.9f,%.9f)\n",
							aspect, shape, rate, move, frame, i, error,
							trail.corner[map[i]].x, trail.corner[map[i]].y, x, y);
					assert(error < 0.001);
				}
				assert(trail.active == ref.needs_render);
				frames++;
				if (!trail.active) break;
			}
			assert(!trail.active);
		}
	}
	win.w = savedw; win.h = savedh;
	printf("kitty source %s: %d frames, max corner error %.6f px; ",
		KITTY_REFERENCE_SHA, frames, maxerror);
}
#endif

static void
quadpixels(void)
{
	int bw = borderpx, bh = borderpx;
	#if ANYSIZE_PATCH
	bw = win.hborderpx; bh = win.vborderpx;
	#endif
	/* A detached moving rectangle must not gain a bridge to the target. */
	fresh(); update(20, 5, 0, blank);
	for (int i = 0; i < 4; i++) trail.corner[i].x -= 8 * win.cw;
	trail.active = 1; trail.updated = stamp(100);
	trail.color.color.alpha = 0x2000;
	clearbuffer(); paint(100);
	assert(pixel(bw + 12 * win.cw + win.cw / 2, bh + 5 * win.ch + win.ch / 2) == red.pixel);
	assert(pixel(bw + 17 * win.cw, bh + 5 * win.ch + win.ch / 2) == 0xff112233);
	assert(trail.x2 < trail.target[0].x);

	/* Even a two-pixel residual stays opaque; background/source alpha and
	 * distance no longer attenuate it. The live cursor itself stays masked. */
	fresh(); update(10, 5, 0, blank);
	for (int i = 0; i < 4; i++) trail.corner[i].x -= 2;
	trail.active = 1; trail.updated = stamp(100);
	trail.color.color.alpha = 0x2000;
	clearbuffer(); paint(100);
	assert(pixel(trail.target[0].x - 1, bh + 5 * win.ch + win.ch / 2) == red.pixel);
	assert(pixel(trail.target[0].x + 1, bh + 5 * win.ch + win.ch / 2) == 0xff112233);

	/* A thin cursor masks its rectangle, allowing a tail to cross other
	 * pixels of the same cell. Test both beam and underline. */
	fresh(); win.cursor = 6; update(10, 5, 0, blank);
	for (int i = 0; i < 4; i++)
		trail.corner[i].x = trail.target[0].x + ((i == 1 || i == 2) ? win.cw / 2 : -win.cw / 2);
	trail.active = 1; trail.updated = stamp(100);
	clearbuffer(); paint(100);
	assert(pixel(bw + 10 * win.cw + cursorthickness + 1, bh + 5 * win.ch + win.ch / 2) == red.pixel);
	assert(pixel(bw + 10 * win.cw, bh + 5 * win.ch + win.ch / 2) == 0xff112233);

	fresh(); win.cursor = 4; update(10, 5, 0, blank);
	trail.corner[0].y -= win.ch / 2; trail.corner[1].y -= win.ch / 2;
	trail.active = 1; trail.updated = stamp(100);
	clearbuffer(); paint(100);
	assert(pixel(bw + 10 * win.cw + win.cw / 2, trail.target[0].y - 2) == red.pixel);
	assert(pixel(bw + 10 * win.cw + win.cw / 2, trail.target[0].y) == 0xff112233);

	/* A folded quad is the union of kitty's two fan triangles, not an
	 * even-odd polygon that punches a hole in their overlap. */
	fresh(); update(20, 5, 0, blank);
	double x = bw + 4 * win.cw, y = bh + 3 * win.ch, size = 3 * win.cw;
	trail.corner[0] = (TrailPoint){x, y};
	trail.corner[1] = (TrailPoint){x + size, y + size};
	trail.corner[2] = (TrailPoint){x + size, y};
	trail.corner[3] = (TrailPoint){x, y + size};
	trail.active = 1; trail.updated = stamp(100);
	clearbuffer(); paint(100);
	assert(pixel(x + 2 * size / 3, y + 3 * size / 4) == red.pixel);
	assert(pixel(x + size / 5, y + size / 10) == 0xff112233);
}

static void
rendercursor(int x, int y, int ox, int oy)
{
	term.c.x = x; term.c.y = y;
	xdrawcursor(x, y, term.line[y][x], ox, oy, term.line[oy][ox]
		#if LIGATURES_PATCH
		, term.line[oy], COLS
		#endif
	);
}

static void
finishfake(void)
{
	cairo_surface_flush(xw.buf);
	lastbuf = curbuf;
	damageclear(&lastbuf->damage);
	curbuf = NULL; xw.buf = NULL; xw.draw = NULL;
}

static void
restoration(void)
{
	unsigned char *reference;
	size_t size = buffers[0].size;
	int ox = 14, oy = 6;

	fresh();
	for (int y = 0; y < ROWS; y++) {
		for (int x = 0; x < COLS; x++) {
			cells[y][x] = blank;
			cells[y][x].u = "->!=abcdef"[x % 10];
			cells[y][x].bg = (x / 4 + y) % 3;
		}
		dirty[y] = 1;
	}
	drawregion(0, 0, COLS, ROWS);
	rendercursor(ox, oy, ox, oy);
	reference = xmalloc(size);
	cairo_surface_flush(xw.buf);
	memcpy(reference, cairo_image_surface_get_data(xw.buf), size);
	jump(); paint(130); assert(trail.painted);
	finishfake();

	/* Cycle to another buffer. startframe() copies the previous composed
	 * picture; trailprepare() then dirties its rows before drawregion(). */
	buffers[0].busy = 1;
	trailreset();
	assert(xstartdraw());
	assert(curbuf == &buffers[1]);
	drawregion(0, 0, COLS, ROWS);
	rendercursor(ox, oy, ox, oy);
	paint(600);
	cairo_surface_flush(xw.buf);
	assert(!memcmp(reference, cairo_image_surface_get_data(xw.buf), size));
	assert(!trailneedsframe());
	finishfake();
	buffers[0].busy = 0;
	free(reference);

	/* A buffer shortage must not expose synthetic dirty rows to ttyread(). */
	jump();
	trail.painted = 1; trail.y1 = borderpx; trail.y2 = win.h - borderpx;
	memset(dirty, 0, sizeof dirty);
	for (int i = 0; i < NBUFFERS; i++) buffers[i].busy = 1;
	assert(!xstartdraw() && presentpending);
	for (int y = 0; y < ROWS; y++) assert(!dirty[y]);
	assert(trail.painted);
	for (int i = 0; i < NBUFFERS; i++) buffers[i].busy = 0;
	trailreset(); assert(xstartdraw());
	assert(!trail.painted && !presentpending);
	finishfake();
}

int
main(void)
{
	assert(FcInit());
	xw.scale = 1;
	#if LIGATURES_PATCH
	hbcreatebuffer();
	#endif
	xloadfonts("DejaVu Sans Mono:pixelsize=16", 0);
	xloadcols();
	term.row = ROWS; term.col = COLS; term.dirty = dirty; term.line = lines;
	borderpx = 2;
	#if ANYSIZE_PATCH
	win.hborderpx = win.vborderpx = borderpx;
	#endif
	win.tw = COLS * win.cw; win.th = ROWS * win.ch;
	win.w = win.tw + 2 * borderpx; win.h = win.th + 2 * borderpx;
	xw.configured = 1;
	xw.specbuf = xmalloc(COLS * 4 * sizeof(*xw.specbuf));
	#if LIGATURES_PATCH
	xw.specseq = xmalloc(COLS * 4 * sizeof(*xw.specseq));
	#endif
	for (int y = 0; y < ROWS; y++) {
		lines[y] = otherlines[y] = cells[y];
		for (int x = 0; x < COLS; x++) cells[y][x] = blank;
	}
	for (int i = 0; i < 2; i++) {
		ShmBuffer *b = &buffers[i];
		b->buffer = (struct wl_buffer *)(uintptr_t)1;
		b->width = win.w; b->height = win.h; b->stride = win.w * 4;
		b->size = (size_t)b->stride * b->height;
		b->data = calloc(1, b->size); assert(b->data);
		b->surface = cairo_image_surface_create_for_data(b->data,
			CAIRO_FORMAT_ARGB32, win.w, win.h, b->stride);
		b->cr = cairo_create(b->surface);
		damageadd(&b->damage, (DamageRect){0, 0, win.w, win.h});
	}
	/* The unused slot also has a sentinel, so shortage tests never allocate. */
	buffers[2].buffer = (struct wl_buffer *)(uintptr_t)1;
	buffers[2].width = win.w; buffers[2].height = win.h; buffers[2].busy = 1;
	fresh(); assert(xstartdraw());
	timing(); contexts(); screenentry(); restyles(); shapes();
	pixelsandcleanup(); quadpixels(); restoration();
	#ifdef CURSORTRAIL_KITTY_REFERENCE
	kittyreference();
	#endif
	for (int i = 0; i < 2; i++) {
		cairo_destroy(buffers[i].cr); cairo_surface_destroy(buffers[i].surface);
		free(buffers[i].data);
	}
	free(xw.specbuf);
	#if LIGATURES_PATCH
	free(xw.specseq); free(restoredrows); hbdestroybuffer();
	#endif
	xunloadfonts(); free(frc); free(dc.col);
	cairo_debug_reset_static_data(); FcFini();
	puts("PASS: exponential chase/retarget, quad/opacity, styles/contexts, TUI screen entry, "
	     "mode-switch retarget, pixels/clipping/damage, text/ligature restoration, "
	     "buffer rotation/shortage, idle");
	return 0;
}
#else
int main(void) { puts("PASS: patch disabled build"); return 0; }
#endif
