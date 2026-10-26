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
	/* Moving right: leading (right) corners arrive sooner. */
	assert(trail.duration[1] == 100 && trail.duration[0] == 400);
	advance(320);
	assert(trail.corner[1].x == trail.target[1].x);
	assert(trail.corner[0].x < trail.target[0].x);
	advance(620);
	assert(!trail.active && !trailneedsframe());
	for (int i = 0; i < 4; i++) {
		assert(trail.corner[i].x == trail.target[i].x);
		assert(trail.corner[i].y == trail.target[i].y);
	}
	fresh(); update(2, 2, 0, blank); update(2, 4, 100, blank);
	assert(trail.active); /* Per-axis threshold, also vertical. */
	fresh(); cursortrail = 0;
	update(2, 2, 0, blank); update(20, 8, 100, blank);
	assert(!trail.valid && !trailneedsframe());

	jump(); advance(250);
	memcpy(expected, trail.corner, sizeof expected);
	jump();
	for (int i = 101; i <= 250; i++) advance(i);
	for (int i = 0; i < 4; i++) {
		assert(fabs(trail.corner[i].x - expected[i].x) < 1e-9);
		assert(fabs(trail.corner[i].y - expected[i].y) < 1e-9);
	}
	/* Retarget in flight starts at the interpolated location, with no jump. */
	update(1, 8, 250, blank);
	assert(trail.active);
	assert(!memcmp(trail.from, expected, sizeof expected));
	advance(650);
	assert(!trail.active);
	fresh(); cursortraildecay[0] = NAN; cursortraildecay[1] = -10;
	update(2, 2, 0, blank); update(14, 2, 100, blank);
	for (int i = 0; i < 4; i++) assert(trail.duration[i] == 100);
	advance(200); assert(!trail.active);
}

static void
contexts(void)
{
	jump(); win.mode |= MODE_HIDE; trailprepare();
	assert(!trail.valid && !trail.active);
	jump(); focusout(); assert(!trail.active && !trail.valid);
	jump(); ime.preedit = "preedit"; trailprepare();
	assert(!trail.active); ime.preedit = NULL;
	#if REFLOW_PATCH || SCROLLBACK_PATCH
	jump(); term.scr = 1; trailprepare(); assert(!trail.active);
	#endif
	jump(); term.line = otherlines; update(18, 8, 200, blank);
	assert(!trail.active && trail.screen == otherlines);
	jump(); win.cursor = 6; update(18, 8, 200, blank);
	assert(!trail.active);
	jump(); win.cw++; update(18, 8, 200, blank);
	assert(!trail.active); win.cw--;
	jump(); xsetmode(1, MODE_HIDE); trailprepare(); assert(!trail.active);
	jump(); xsetmode(1, MODE_HIDE); xsetmode(0, MODE_HIDE);
	assert(trail.active); /* Batched hide/move/show must retain the trail. */
	jump(); assert(!xsetcursor(4)); assert(!trail.active);
	jump(); assert(!xsetcursor(2)); assert(trail.active);
	xsetmode(0, MODE_HIDE); assert(trail.active); /* Redundant TUI updates. */
	jump(); trailprepare(); /* Caller skipped xdrawcursor, e.g. scrollback. */
	paint(200); assert(!trail.active);
	#if KEYBOARDSELECT_PATCH
	jump(); win.mode |= MODE_KBDSELECT; update(18, 8, 200, blank);
	assert(!trail.active);
	#endif
	#if BLINKING_CURSOR_PATCH
	jump(); win.cursor = 1; win.mode |= MODE_BLINK;
	assert(!trailvisible()); trailprepare(); assert(!trail.active);
	win.cursor = 2; assert(trailvisible());
	#endif
	jump(); win.mode &= ~MODE_VISIBLE; assert(!trailneedsframe());
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
	assert(buffers[0].dx1 <= trail.x1 && buffers[0].dy1 <= trail.y1);
	assert(buffers[0].dx2 >= trail.x2 && buffers[0].dy2 >= trail.y2);
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
	lastbuf->dx1 = lastbuf->dy1 = lastbuf->dx2 = lastbuf->dy2 = 0;
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
		b->dx1 = b->dy1 = 0; b->dx2 = win.w; b->dy2 = win.h;
	}
	/* The unused slot also has a sentinel, so shortage tests never allocate. */
	buffers[2].buffer = (struct wl_buffer *)(uintptr_t)1;
	buffers[2].width = win.w; buffers[2].height = win.h; buffers[2].busy = 1;
	fresh(); assert(xstartdraw());
	timing(); contexts(); shapes(); pixelsandcleanup(); restoration();
	for (int i = 0; i < 2; i++) {
		cairo_destroy(buffers[i].cr); cairo_surface_destroy(buffers[i].surface);
		free(buffers[i].data);
	}
	free(xw.specbuf);
	#if LIGATURES_PATCH
	free(xw.specseq); hbdestroybuffer();
	#endif
	xunloadfonts(); free(frc); free(dc.col);
	cairo_debug_reset_static_data(); FcFini();
	puts("PASS: trigger/easing/retarget, styles/contexts, pixels/clipping/damage, text/ligature restoration, buffer rotation/shortage, idle");
	return 0;
}
#else
int main(void) { puts("PASS: patch disabled build"); return 0; }
#endif
