/* Exercise real frame accumulation, sparse copying, cursor restoration, frame
 * elision and idle buffer release.
 * Wayland requests are captured; parser workloads use a local PTY. */
#define _GNU_SOURCE
#include <wayland-client.h>
static void test_attach(struct wl_surface *, struct wl_buffer *, int32_t, int32_t);
static void test_damage(struct wl_surface *, int32_t, int32_t, int32_t, int32_t);
static struct wl_callback *test_frame(struct wl_surface *);
static int test_listener(struct wl_callback *, const struct wl_callback_listener *, void *);
static void test_destroy_callback(struct wl_callback *);
static void test_commit(struct wl_surface *);
static int test_flush(struct wl_display *);
static struct wl_shm_pool *test_create_pool(struct wl_shm *, int32_t, int32_t);
static struct wl_buffer *test_create_buffer(struct wl_shm_pool *, int32_t, int32_t,
		int32_t, int32_t, uint32_t);
static void test_destroy_pool(struct wl_shm_pool *);
static int test_buffer_listener(struct wl_buffer *, const struct wl_buffer_listener *, void *);
static void test_destroy_buffer(struct wl_buffer *);
#define wl_surface_attach test_attach
#define wl_surface_damage_buffer test_damage
#define wl_surface_frame test_frame
#define wl_callback_add_listener test_listener
#define wl_callback_destroy test_destroy_callback
#define wl_surface_commit test_commit
#define wl_display_flush test_flush
#define wl_shm_create_pool test_create_pool
#define wl_shm_pool_create_buffer test_create_buffer
#define wl_shm_pool_destroy test_destroy_pool
#define wl_buffer_add_listener test_buffer_listener
#define wl_buffer_destroy test_destroy_buffer
#define main st_program_main
#include "../wl.c"
#undef main
#include <assert.h>

#define COLS 24
#define ROWS 10
static Glyph cells[ROWS][COLS], alternate[COLS];
static Line lines[ROWS];
static int dirty[ROWS];
static DamageRect reported[DAMAGE_MAX_RECTS];
static unsigned int nreported, commits, callback_id, created, destroyed;
static struct wl_buffer *attached;
static int count_lines, line_calls;

/* Function instrumentation measures calls without production test hooks. */
void __attribute__((no_instrument_function))
__cyg_profile_func_enter(void *function, void *caller)
{
	(void)caller;
	if (count_lines && function == (void *)xdrawline)
		line_calls++;
}
void __attribute__((no_instrument_function))
__cyg_profile_func_exit(void *function, void *caller)
{
	(void)function; (void)caller;
}

static void test_attach(struct wl_surface *s, struct wl_buffer *b, int32_t x, int32_t y)
{
	assert(!s && x == 0 && y == 0); attached = b;
}
static void test_damage(struct wl_surface *s, int32_t x, int32_t y, int32_t w, int32_t h)
{
	assert(!s && w > 0 && h > 0 && x >= 0 && y >= 0);
	assert(x + w <= win.w && y + h <= win.h);
	assert(nreported < LEN(reported));
	reported[nreported++] = (DamageRect){x, y, x + w, y + h};
}
static struct wl_callback *test_frame(struct wl_surface *s)
{
	assert(!s); return (struct wl_callback *)(uintptr_t)++callback_id;
}
static int test_listener(struct wl_callback *cb, const struct wl_callback_listener *l, void *data)
{
	assert(cb && l && !data); return 0;
}
static void test_destroy_callback(struct wl_callback *cb) { assert(cb); }
static void test_commit(struct wl_surface *s) { assert(!s); commits++; }
static int test_flush(struct wl_display *d) { assert(!d); return 0; }
static struct wl_shm_pool *test_create_pool(struct wl_shm *shm, int32_t fd, int32_t size)
{
	assert(!shm && fd >= 0 && size > 0); return (struct wl_shm_pool *)(uintptr_t)1;
}
static struct wl_buffer *test_create_buffer(struct wl_shm_pool *pool, int32_t offset,
		int32_t w, int32_t h, int32_t stride, uint32_t format)
{
	assert(pool && !offset && w == win.w && h == win.h && stride == w * 4);
	assert(format == WL_SHM_FORMAT_ARGB8888);
	return (struct wl_buffer *)(uintptr_t)(0x100 + ++created);
}
static void test_destroy_pool(struct wl_shm_pool *pool) { assert(pool); }
static int test_buffer_listener(struct wl_buffer *b, const struct wl_buffer_listener *l, void *data)
{
	assert(b && l && data); return 0;
}
static void test_destroy_buffer(struct wl_buffer *b) { assert(b); destroyed++; }

static int contains(const DamageRegion *d, int x, int y)
{
	for (unsigned int i = 0; i < d->count; i++) {
		DamageRect r = d->rects[i];
		if (x >= r.x1 && x < r.x2 && y >= r.y1 && y < r.y2)
			return 1;
	}
	return 0;
}

static void clearregions(void)
{
	for (int i = 0; i < NBUFFERS; i++) damageclear(&buffers[i].damage);
	nreported = 0;
}

static void sparse(void)
{
	clearregions();
	adddamage(3, 4, 20, 2);
	adddamage(3, win.h - 6, 20, 2);
	for (int i = 0; i < NBUFFERS; i++) {
		assert(buffers[i].damage.count == 2);
		assert(contains(&buffers[i].damage, 3, 4));
		assert(contains(&buffers[i].damage, 22, win.h - 5));
		assert(!contains(&buffers[i].damage, 3, win.h / 2));
	}
	curbuf = &buffers[0]; xw.buf = curbuf->surface; xw.draw = curbuf->cr;
	present();
	assert(commits == 1 && attached == buffers[0].buffer && nreported == 2);
	assert(!curbuf && !xw.buf && !xw.draw && !buffers[0].damage.count);
	/* Committing A must not forget changes still missing from B and C. */
	assert(buffers[1].damage.count == 2 && buffers[2].damage.count == 2);
	size_t area = 0;
	for (unsigned int i = 0; i < nreported; i++)
		area += (size_t)(reported[i].x2 - reported[i].x1) *
		        (reported[i].y2 - reported[i].y1);
	assert(area == 80);
}

static void regions(void)
{
	DamageRegion d = {0};
	unsigned char expected[31][47] = {{0}};
	uint32_t rng = 1;
	/* A bridge must join earlier intervals, independent of insertion order. */
	damageadd(&d, (DamageRect){0, 0, 5, 2});
	damageadd(&d, (DamageRect){10, 0, 15, 2});
	damageadd(&d, (DamageRect){5, 0, 10, 2});
	assert(d.count == 1 && d.rects[0].x2 == 15);
	damageclear(&d);
	for (int i = 0; i < 1000; i++) {
		rng = rng * 1664525u + 1013904223u; int x = rng % 47;
		rng = rng * 1664525u + 1013904223u; int y = rng % 31;
		rng = rng * 1664525u + 1013904223u; int x2 = MIN(x + 1 + rng % 7, 47);
		rng = rng * 1664525u + 1013904223u; int y2 = MIN(y + 1 + rng % 5, 31);
		damageadd(&d, (DamageRect){x, y, x2, y2});
		for (int py = y; py < y2; py++)
			for (int px = x; px < x2; px++) expected[py][px] = 1;
		assert(d.count <= DAMAGE_MAX_RECTS);
		for (int py = 0; py < 31; py++)
			for (int px = 0; px < 47; px++)
				assert(!expected[py][px] || contains(&d, px, py));
	}
	assert(d.coarse);
	clearregions();
	int oldw = buffers[2].width, oldh = buffers[2].height;
	buffers[2].width = 17; buffers[2].height = 19;
	adddamage(-5, -7, 10, 12);
	adddamage(15, 17, INT_MAX, INT_MAX);
	adddamage(INT_MAX - 2, INT_MAX - 2, 10, 10);
	adddamage(0, 0, 0, 10);
	for (unsigned int i = 0; i < buffers[2].damage.count; i++) {
		DamageRect r = buffers[2].damage.rects[i];
		assert(r.x1 >= 0 && r.y1 >= 0 && r.x2 <= 17 && r.y2 <= 19);
	}
	assert(contains(&buffers[2].damage, 0, 0));
	assert(contains(&buffers[2].damage, 16, 18));
	buffers[2].width = oldw; buffers[2].height = oldh;
	clearregions();
}

static uint32_t *at(ShmBuffer *b, int x, int y)
{
	return (uint32_t *)((unsigned char *)b->data + (size_t)y * b->stride) + x;
}

static void copying(void)
{
	ShmBuffer *src = &buffers[0], *dst = &buffers[1];
	clearregions(); memset(dirty, 0, sizeof dirty);
	cairo_surface_flush(src->surface); cairo_surface_flush(dst->surface);
	for (int y = 0; y < win.h; y++) for (int x = 0; x < win.w; x++) {
		*at(src, x, y) = 0xff123456; *at(dst, x, y) = 0xffabcdef;
	}
	adddamage(2, 3, 5, 2);
	adddamage(11, win.h - 7, 7, 3);
	/* This region spans borders and dirty rows, which must be left for repaint. */
	adddamage(22, 0, 3, win.h);
	dirty[0] = dirty[ROWS - 1] = 1;
	copybufferdamage(dst, src);
	for (int y = 0; y < win.h; y++) for (int x = 0; x < win.w; x++) {
		int copied = contains(&dst->damage, x, y) && !pixelrowdirty(y);
		assert(*at(dst, x, y) == (copied ? 0xff123456u : 0xffabcdefu));
	}
	assert(dst->damage.count); /* Copying does not acknowledge compositor damage. */
	memset(dirty, 0, sizeof dirty);
	/* getbuffer() must rotate while A is held; B's sparse data is restored. */
	for (int i = 0; i < NBUFFERS; i++) buffers[i].busy = i != 1;
	lastbuf = src; curbuf = NULL; forcedraw = 1;
	assert(xstartdraw() && curbuf == dst);
	for (int y = 0; y < win.h; y++) for (int x = 0; x < win.w; x++)
		if (contains(&dst->damage, x, y)) assert(*at(dst, x, y) == 0xff123456u);
	nreported = 0; present();
	assert(!dst->damage.count && buffers[2].damage.count);
	for (int i = 0; i < NBUFFERS; i++) buffers[i].busy = 1;
	dirty[2] = 1;
	assert(!xstartdraw() && presentpending && dirty[2]);
	for (int i = 0; i < NBUFFERS; i++) buffers[i].busy = 0;
	memset(dirty, 0, sizeof dirty);
	clearregions();
}

static void drawcase(int force_restore, int focus, int hidden, int style,
                     int partial, unsigned char *output)
{
	ShmBuffer *b = &buffers[0];
	curbuf = NULL; lastbuf = b; b->busy = 0; forcedraw = 1;
	memset(dirty, 0, sizeof dirty);
	if (!partial) for (int y = 0; y < ROWS; y++) dirty[y] = 1;
	win.mode = MODE_VISIBLE | (focus ? MODE_FOCUSED : 0) | (hidden ? MODE_HIDE : 0);
	win.cursor = style;
	term.c.x = 13; term.c.y = 6; term.ocx = 4; term.ocy = 2;
	nreported = 0;
	assert(xstartdraw());
	drawregion(0, 0, COLS, ROWS);
	#if LIGATURES_PATCH
	if (force_restore) restoredrows[2] = NULL;
	#else
	(void)force_restore;
	#endif
	line_calls = 0; count_lines = 1;
	xdrawcursor(13, 6, cells[6][13], 4, 2, cells[2][4]
		#if LIGATURES_PATCH
		, lines[2], COLS
		#endif
	);
	count_lines = 0;
	#if LIGATURES_PATCH
	assert(line_calls == (force_restore || partial ? 1 : 0));
	#else
	assert(line_calls == 1);
	#endif
	cairo_surface_flush(b->surface);
	memcpy(output, b->data, b->size);
	present();
}

static void cursorrestoration(void)
{
	size_t size = buffers[0].size;
	unsigned char *reference = xmalloc(size), *optimized = xmalloc(size);
	for (int focus = 0; focus <= 1; focus++) for (int hidden = 0; hidden <= 1; hidden++)
		for (int style = 2; style <= 6; style += 2) {
			drawcase(1, focus, hidden, style, 0, reference);
			drawcase(0, focus, hidden, style, 0, optimized);
			assert(!memcmp(reference, optimized, size));
		}
	/* Selection and IME overlays must survive the same reuse decision. */
	selstart(2, 2, 0); selextend(18, 6, SEL_REGULAR, 0);
	ime.preedit = xstrdup("ab\xCE\xBB"); ime.pre_begin = 1; ime.pre_end = 2;
	drawcase(1, 1, 0, 2, 0, reference);
	drawcase(0, 1, 0, 2, 0, optimized);
	assert(!memcmp(reference, optimized, size));
	ime_clearpreedit(); selclear();
	/* A clean old row needs restoration in the next frame too. */
	drawcase(0, 1, 0, 2, 1, optimized);
	#if LIGATURES_PATCH
	/* Equal row numbers with different source lines must never skip restoration. */
	curbuf = NULL; lastbuf = &buffers[0]; lastbuf->busy = 0; forcedraw = 1;
	assert(xstartdraw());
	xdrawline(lines[2], 0, 2, COLS);
	memcpy(alternate, cells[2], sizeof alternate); alternate[0].u = 'Z';
	line_calls = 0; count_lines = 1;
	xdrawcursor(13, 6, cells[6][13], 4, 2, alternate[4], alternate, COLS);
	count_lines = 0; assert(line_calls == 1);
	nreported = 0; present();
	#endif
	free(reference); free(optimized);
}

/* One frame the way run() produces it, released at once by the compositor. */
static int frame(void)
{
	unsigned int before = commits;
	nreported = 0;
	draw();
	assert(!curbuf);
	if (lastbuf) lastbuf->busy = 0;
	return commits != before;
}

static void snapshot(unsigned char *output)
{
	cairo_surface_flush(lastbuf->surface);
	memcpy(output, lastbuf->data, lastbuf->size);
}

static void elision(void)
{
	size_t size = buffers[0].size;
	unsigned char *reference = xmalloc(size), *again = xmalloc(size);
	Glyph saved;
	for (int i = 0; i < NBUFFERS; i++) buffers[i].busy = 0;
	curbuf = NULL; lastbuf = &buffers[0]; presentpending = 0;
	term.c.x = term.ocx = 13; term.c.y = term.ocy = 6;
	for (int focus = 0; focus <= 1; focus++) for (int hidden = 0; hidden <= 1; hidden++)
		for (int style = 2; style <= 6; style += 2) {
			win.mode = MODE_VISIBLE | (focus ? MODE_FOCUSED : 0) | (hidden ? MODE_HIDE : 0);
			win.cursor = style;
			for (int y = 0; y < ROWS; y++) dirty[y] = 1;
			assert(frame() && nreported);
			snapshot(reference);
			/* Pointer motion, key releases, modifiers: nothing to paint. */
			assert(!frame() && !presentpending && !nreported);
			assert(!frame());
			/* The skipped frame would have painted the very same picture. */
			forcedraw = 1; assert(frame() && !forcedraw);
			snapshot(again); assert(!memcmp(reference, again, size));
			assert(!frame());
		}
	/* Every input of the cursor picture brings the frame back, exactly once. */
	dirty[3] = 1; assert(frame() && !frame());
	term.c.x = 14; assert(frame() && term.ocx == 14 && !frame());
	term.c.y = 5; assert(frame() && term.ocy == 5 && !frame());
	win.mode ^= MODE_FOCUSED; assert(frame() && !frame());
	win.mode ^= MODE_HIDE; assert(frame() && !frame());
	win.mode ^= MODE_REVERSE; assert(frame() && !frame());
	win.mode ^= MODE_REVERSE; assert(frame() && !frame());
	win.cursor = 4; assert(frame() && !frame());
	selstart(2, 2, 0); selextend(18, 6, SEL_REGULAR, 0);
	memset(dirty, 0, sizeof dirty); assert(frame() && !frame());
	selclear(); memset(dirty, 0, sizeof dirty); assert(frame() && !frame());
	saved = cells[5][14]; cells[5][14].u = 'Q'; assert(frame() && !frame());
	cells[5][14] = saved; assert(frame() && !frame());
	presentpending = 1; assert(frame() && !presentpending && !frame());
	forcedraw = 1; assert(frame() && !frame());
	/* A frame that could not start is still owed. */
	for (int i = 0; i < NBUFFERS; i++) buffers[i].busy = 1;
	term.c.x = 13; assert(!frame() && presentpending);
	for (int i = 0; i < NBUFFERS; i++) buffers[i].busy = 0;
	assert(frame() && term.ocx == 13 && !frame());
	free(reference); free(again);
}

static void trimming(void)
{
	size_t size = buffers[0].size;
	unsigned char *reference = xmalloc(size), *rebuilt = xmalloc(size);
	struct timespec now = {1000, 0};
	unsigned int before;
	double next;
	win.mode = MODE_VISIBLE | MODE_FOCUSED; win.cursor = 2;
	for (int i = 0; i < NBUFFERS; i++) buffers[i].busy = 0;
	curbuf = NULL; lastbuf = &buffers[0];
	for (int y = 0; y < ROWS; y++) dirty[y] = 1;
	assert(frame() && lastbuf == &buffers[0]);
	snapshot(reference);
	/* Held, recent and in-progress buffers stay; the idle released one goes. */
	buffers[0].used = (struct timespec){1000 - 6, 0};
	buffers[1].used = (struct timespec){1000 - 9, 0}; buffers[1].busy = 1;
	buffers[2].used = (struct timespec){1000 - 2, 0};
	before = destroyed;
	next = trimbuffers(&now);
	assert(destroyed == before + 1 && !buffers[0].buffer && !buffers[0].data && !lastbuf);
	assert(buffers[1].buffer && buffers[2].buffer);
	assert(next > BUFFER_IDLE - 2001 && next < BUFFER_IDLE - 1999);
	buffers[2].used = (struct timespec){1000 - 60, 0}; curbuf = &buffers[2];
	assert(trimbuffers(&now) < 0 && buffers[2].buffer && destroyed == before + 1);
	curbuf = NULL; buffers[2].busy = 1;
	assert(trimbuffers(&now) < 0 && destroyed == before + 1);
	/* Nothing holds the picture now: an otherwise empty frame rebuilds all of it. */
	memset(dirty, 0, sizeof dirty); before = created;
	assert(frame() && created == before + 1 && lastbuf == &buffers[0]);
	assert(attached == buffers[0].buffer && nreported == 1);
	assert(reported[0].x1 == 0 && reported[0].y1 == 0 &&
	       reported[0].x2 == win.w && reported[0].y2 == win.h);
	snapshot(rebuilt); assert(!memcmp(reference, rebuilt, size));
	assert(!frame());
	/* Released again, everything idle is returned and no timer remains. */
	for (int i = 0; i < NBUFFERS; i++) {
		buffers[i].busy = 0; buffers[i].used = (struct timespec){1000 - 6, 0};
	}
	before = destroyed;
	assert(trimbuffers(&now) < 0 && destroyed == before + NBUFFERS && !lastbuf);
	for (int i = 0; i < NBUFFERS; i++) assert(!buffers[i].buffer);
	assert(frame() && lastbuf);
	snapshot(rebuilt); assert(!memcmp(reference, rebuilt, size));
	free(reference); free(rebuilt);
}

#if REFLOW_PATCH
#include <pty.h>
/* The real parser and scrolling code of st.o, fed through a pty used as -l line. */
static int ptym, step;
static uint32_t seed = 20261119;
static unsigned char *shots[3];

static unsigned int rnd(unsigned int n)
{
	seed = seed * 1664525u + 1013904223u;
	return (seed >> 10) % n;
}

static void feed(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	int n;
	va_start(ap, fmt); n = vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
	assert(n > 0 && n < (int)sizeof buf && write(ptym, buf, n) == n);
	ttyread();
}

static int inreported(int x, int y)
{
	for (unsigned int i = 0; i < nreported; i++)
		if (x >= reported[i].x1 && x < reported[i].x2 &&
		    y >= reported[i].y1 && y < reported[i].y2)
			return 1;
	return 0;
}

/* One frame, into another buffer if the last one is held.  Whatever it moved
 * instead of drawing, the picture must be the one a complete repaint gives and
 * every pixel that changed must be reported.  Returns the lines it drew. */
static int scrollframe(int hold, int compare)
{
	unsigned char *prev = shots[0], *got = shots[1], *full = shots[2];
	ShmBuffer saved[NBUFFERS], *last;
	size_t size = (size_t)win.w * win.h * 4;
	unsigned int before = commits;
	int calls;

	snapshot(prev);
	for (int i = 0; i < NBUFFERS; i++) buffers[i].busy = hold && &buffers[i] == lastbuf;
	nreported = 0; line_calls = 0; count_lines = 1;
	draw();
	count_lines = 0; calls = line_calls;
	assert(!curbuf && lastbuf && !term.nscrolls);
	for (int i = 0; i < NBUFFERS; i++) buffers[i].busy = 0;
	snapshot(got);
	if (commits == before)
		assert(!memcmp(prev, got, size));
	for (int y = 0; y < win.h; y++) for (int x = 0; x < win.w; x++)
		assert(!memcmp(prev + ((size_t)y * win.w + x) * 4,
		               got + ((size_t)y * win.w + x) * 4, 4) || inreported(x, y));
	if (!compare)
		return calls;
	memcpy(saved, buffers, sizeof saved); last = lastbuf;
	tfulldirt(); forcedraw = 1; draw();
	assert(!curbuf && lastbuf == last);
	snapshot(full);
	memcpy(buffers, saved, sizeof saved);
	for (int y = 0; y < win.h; y++)
		if (memcmp(got + (size_t)y * win.w * 4, full + (size_t)y * win.w * 4, win.w * 4)) {
			fprintf(stderr, "step %d: pixel row %d (text row %d) differs from a repaint\n",
				step, y, (y - 2) / win.ch);
			abort();
		}
	thistcompact();
	return calls;
}

static void scrollop(void)
{
	int a, b;
	switch (rnd(17)) {
	case 0: case 1: case 2: case 3: /* output: scrolls at the bottom of the region */
		feed("\033[%d;%dm%c%c \xe4\xb8\xad -> != http://a.bc/%d\033[m\r\n",
			30 + rnd(8), 40 + rnd(8), 'a' + rnd(26), 'A' + rnd(26), rnd(50)); break;
	case 4: feed("%c%c\n", 'a' + rnd(26), 'a' + rnd(26)); break;
	case 5: a = 1 + rnd(ROWS); b = 1 + rnd(ROWS);
		feed("\033[%d;%dr", MIN(a, b), MAX(a, b)); break;
	case 6: feed("\033[r"); break;
	case 7: feed("\033[%d%c", 1 + rnd(ROWS + 2), "STLM"[rnd(4)]); break;
	case 8: feed("\033[%d%c", 1 + rnd(2), "STLM"[rnd(4)]); break;
	case 9: feed(rnd(2) ? "\033M" : "\033D"); break;
	case 10: feed("\033[%d;%dH", 1 + rnd(ROWS), 1 + rnd(COLS)); break;
	case 11: kscrollup(&(Arg){.i = 1 + rnd(rnd(2) ? 2 : ROWS + 2)}); break;
	case 12: kscrolldown(&(Arg){.i = 1 + rnd(rnd(2) ? 2 : ROWS + 2)}); break;
	case 13:
		if (rnd(3)) {
			selstart(rnd(COLS), rnd(ROWS), 0);
			selextend(rnd(COLS), rnd(ROWS), SEL_REGULAR, 0);
		} else {
			selclear();
		}
		break;
	case 14:
		if (!rnd(3)) feed(tisaltscr() ? "\033[?1049l" : "\033[?1049h");
		else feed(rnd(2) ? "\033[K" : "\033[2J");
		break;
	case 15:
		if (ime.preedit) {
			ime_clearpreedit();
		} else {
			ime.preedit = xstrdup("ab\xCE\xBB"); ime.pre_begin = 1; ime.pre_end = 2;
		}
		forcedraw = 1; break;
	case 16:
		if (rnd(2)) win.mode ^= MODE_FOCUSED;
		else feed(rnd(2) ? "\033[?25l" : "\033[?25h");
		break;
	}
}

static void scrolling(void)
{
	char name[64];
	int ptys, urls = 0, moved = 0;

	/* A wide glyph takes two cells where a locale knows it, else it overflows one. */
	if (!setlocale(LC_CTYPE, "") || wcwidth(0x4e2d) != 2)
		setlocale(LC_CTYPE, "en_US.UTF-8");
	assert(!openpty(&ptym, &ptys, name, NULL, NULL));
	tnew(COLS, ROWS);
	assert(ttynew(name, NULL, NULL, NULL) >= 0);
	for (int i = 0; i < 3; i++) shots[i] = xmalloc((size_t)win.w * win.h * 4);
	win.mode = MODE_VISIBLE | MODE_FOCUSED; win.cursor = 2;
	assert(frame() && lastbuf);
	scrollframe(0, 1);

	/* A scrolled line is not drawn again: only the new one and the cursor's. */
	for (int i = 0; i < ROWS + 3; i++) feed("line %d ->\r\n", i);
	scrollframe(0, 1);
	feed("next\r\n"); assert(scrollframe(0, 1) <= 3);
	feed("held\r\n"); assert(scrollframe(1, 1) <= 3);
	feed("\n\n\n"); assert(scrollframe(0, 1) <= 5);
	kscrollup(&(Arg){.i = 2}); assert(scrollframe(0, 1) <= 3);
	feed("behind the view\r\n"); assert(scrollframe(0, 1) <= 1);
	kscrolldown(&(Arg){.i = 1}); assert(scrollframe(1, 1) <= 2);
	kscrolldown(&(Arg){.i = 2}); assert(scrollframe(0, 1) <= 4 && !term.scr);
	feed("\033[3;7r\033[7;1H\n"); assert(scrollframe(0, 1) <= 3);
	feed("\033M\033[3;1H\033M"); assert(scrollframe(0, 1) <= 4);
	feed("\033[2S\033[1T\033[r"); assert(scrollframe(0, 1) <= 5);
	/* Scrolls of the same rows add up only if none in between moved them. */
	feed("\033[S\033[3;6r\033[S\033[r\033[S"); scrollframe(0, 1);
	feed("\033[2;4r\033[T\033[6;9r\033[2S\033[2;4r\033[T\033[r");
	assert(scrollframe(1, 1) <= 6);
	/* A selection follows the lines pushed into the history as a whole, also
	 * on rows below the region that stay, and ends where the history does. */
	feed("\033[1;6r\033[6;1H");
	selstart(2, 4, 0); selextend(5, 8, SEL_REGULAR, 0); scrollframe(0, 1);
	feed("\n"); scrollframe(0, 1);
	selclear(); feed("\033[r\033[%d;1H", ROWS);
	for (int i = 0; i < HISTSIZE; i++) feed("%d\n", i);
	kscrollup(&(Arg){.i = HISTSIZE}); selstart(0, 0, 0);
	kscrolldown(&(Arg){.i = HISTSIZE}); selextend(5, 4, SEL_REGULAR, 0);
	scrollframe(0, 1);
	feed("\n"); scrollframe(0, 1);
	selclear();
	/* More scrolls than are followed, and one that leaves nothing, repaint. */
	feed("\033[1;3r\033[S\033[4;6r\033[S\033[7;9r\033[S\033[2;8r\033[S\033[3;5r\033[S\033[r");
	scrollframe(0, 1);
	feed("\033[%dS", ROWS); scrollframe(0, 1);

	#if SIXEL_PATCH
	/* Real parser input: opaque and transparent strips, partial erasure,
	 * replacement before a frame, history, alt screen and buffer rotation. */
	for (int transparent = 0; transparent <= 1; transparent++) {
		char image[240];
		int len = snprintf(image, sizeof image,
			"\033P0;%dq\"1;1;%d;%d#1;2;100;0;0", transparent,
			5 * win.cw, 3 * win.ch);
		for (int y = 0; y < 3 * win.ch; y += 6)
			len += snprintf(image + len, sizeof image - len,
				"!%d%c-", 5 * win.cw, y % 12 ? 'C' : '~');
		assert(len + 3 < (int)sizeof image);
		strcpy(image + len, "\033\\");
		feed("\033[r\033[?25l\033[2J\033[2;3H%s", image);
		assert(term.images); scrollframe(0, 1);
		feed("\033[2S"); scrollframe(1, 1);
		feed("\033[T"); scrollframe(0, 1);
		kscrollup(&(Arg){.i = 2}); scrollframe(1, 1);
		kscrolldown(&(Arg){.i = 2}); scrollframe(0, 1);
		feed("\033[3;4HXX\033[3;3H%s", image); scrollframe(1, 1);
		feed("\033[2;7r\033[7;1H\n"); scrollframe(0, 1);
		feed("\033[?1049h\033[2;3H%s", image); scrollframe(1, 1);
		feed("\033[S"); scrollframe(0, 1);
		feed("\033[?1049l\033[r\033[2J\033[?25h"); scrollframe(1, 1);
		for (ImageList *im = term.images; im; im = im->next)
			assert(im->y < 0 || im->y >= term.row);
	}
	#endif

	for (step = 0; step < 6000; step++) {
		int calls;
		if (!rnd(12)) {
			/* An underlined url is painted over its rows and does not move. */
			if (!term.scr && detecturl(rnd(COLS), rnd(ROWS), 1)) {
				scrollframe(0, 0); urls++;
				if (!tisaltscr() && term.histf && rnd(2))
					kscrollup(&(Arg){.i = 1});
				else
					feed(rnd(2) ? "\033[S" : "\033[T");
				scrollframe(rnd(4) == 0, 1);
			}
			continue;
		}
		for (int n = 1 + rnd(4); n > 0; n--) scrollop();
		calls = scrollframe(rnd(4) == 0, 1);
		moved += calls > 0 && calls < ROWS / 2;
	}
	/* The cases above were really reached. */
	assert(urls > 20 && moved > 1000);
	if (ime.preedit) ime_clearpreedit();
	selclear();
	#if KEYBOARDSELECT_PATCH
	/* Keyboard selection paints a cursor and an indicator of its own, which
	 * do not move with the lines it scrolls through. */
	if (tisaltscr()) feed("\033[?1049l");
	kscrolldown(&(Arg){.i = HISTSIZE}); feed("\033[r\033[?25h");
	win.mode |= MODE_FOCUSED; scrollframe(0, 1);
	win.mode ^= kbds_keyboardhandler(-1, NULL, 0, 0);
	assert(IS_SET(MODE_KBDSELECT) && term.histf > 2 * ROWS);
	for (int i = 0; i < 4 * ROWS; i++) {
		kbds_keyboardhandler(i < 3 * ROWS ? XKB_KEY_k : XKB_KEY_j, NULL, 0, 0);
		scrollframe(i % 3 == 0, 1);
	}
	assert(term.scr > ROWS);
	win.mode ^= kbds_keyboardhandler(XKB_KEY_Escape, NULL, 0, 0);
	assert(!IS_SET(MODE_KBDSELECT) && !term.scr); scrollframe(0, 1);
	#endif
	for (int i = 0; i < 3; i++) free(shots[i]);
	close(ptym); close(ptys);
}
#endif

int main(void)
{
	assert(FcInit()); xw.scale = 1;
	#if LIGATURES_PATCH
	hbcreatebuffer();
	#endif
	xloadfonts("DejaVu Sans Mono:pixelsize=16", 0); xloadcols();
	term.row = ROWS; term.col = COLS; term.line = lines; term.dirty = dirty;
	term.bot = ROWS - 1; selinit();
	borderpx = 2;
	#if ANYSIZE_PATCH
	win.hborderpx = win.vborderpx = borderpx;
	#endif
	win.tw = COLS * win.cw; win.th = ROWS * win.ch;
	win.w = win.tw + 4; win.h = win.th + 4;
	xw.configured = 1; win.mode = MODE_VISIBLE;
	#if CURSOR_TRAIL_PATCH
	cursortrail = 0;
	#endif
	xw.specbuf = xmalloc(COLS * 4 * sizeof(*xw.specbuf));
	#if LIGATURES_PATCH
	xw.specseq = xmalloc(COLS * 4 * sizeof(*xw.specseq));
	#endif
	for (int y = 0; y < ROWS; y++) {
		lines[y] = cells[y];
		for (int x = 0; x < COLS; x++) cells[y][x] = (Glyph){
			.u = "->!=ffiabcdef"[x % 13], .fg = defaultfg, .bg = (x / 4 + y) % 3};
		cells[y][8].u = 0x4e2d; cells[y][8].mode = ATTR_WIDE;
		cells[y][9].u = 0; cells[y][9].mode = ATTR_WDUMMY;
	}
	for (int i = 0; i < NBUFFERS; i++) {
		ShmBuffer *b = &buffers[i];
		b->width = win.w; b->height = win.h; b->stride = win.w * 4;
		b->size = (size_t)b->stride * b->height;
		b->data = mmap(NULL, b->size, PROT_READ | PROT_WRITE,
		              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		assert(b->data != MAP_FAILED);
		b->surface = cairo_image_surface_create_for_data(b->data,
			CAIRO_FORMAT_ARGB32, b->width, b->height, b->stride);
		b->cr = cairo_create(b->surface);
		b->buffer = (struct wl_buffer *)(uintptr_t)(i + 1);
	}
	sparse(); regions(); copying(); cursorrestoration(); elision(); trimming();
	#if REFLOW_PATCH
	scrolling();
	#endif
	for (int i = 0; i < NBUFFERS; i++) { buffers[i].buffer = NULL; destroybuffer(&buffers[i]); }
	free(xw.specbuf);
	#if LIGATURES_PATCH
	free(xw.specseq); free(restoredrows); hbdestroybuffer();
	#endif
	xunloadfonts(); free(frc); free(dc.col);
	cairo_debug_reset_static_data(); FcFini();
	puts("PASS: sparse protocol damage, bounded regions, clipping, stale-buffer rotation, "
	     "dirty-row copies, shortage, cursor pixels, restored-line reuse, frame elision, "
	     "idle buffer release and scrolled pixels");
	return 0;
}
