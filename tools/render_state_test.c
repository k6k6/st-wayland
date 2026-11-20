/* Exercise real frame accumulation, sparse copying, cursor restoration, frame
 * pacing, elision and idle buffer release.
 * Wayland requests are captured; parser workloads use a local PTY. */
#define _GNU_SOURCE
#include <time.h>
#include <malloc.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
static int test_clock_gettime(clockid_t, struct timespec *);
static int test_malloc_trim(size_t);
static void test_ack_configure(struct xdg_surface *, uint32_t);
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
#define clock_gettime test_clock_gettime
#define malloc_trim test_malloc_trim
#define xdg_surface_ack_configure test_ack_configure
#define main st_program_main
#include "../wl.c"
#undef main
#undef clock_gettime
#undef malloc_trim
#include <assert.h>

#define COLS 24
#define ROWS 10
static Glyph cells[ROWS][COLS], alternate[COLS];
static Line lines[ROWS];
static int dirty[ROWS];
static DamageRect reported[DAMAGE_MAX_RECTS];
static unsigned int nreported, commits, callback_id, created, destroyed;
static unsigned int callbacks_destroyed, configures_acked;
static unsigned int allocator_trims;
static int fake_clock;
static struct timespec fake_now;
static struct wl_buffer *attached;
static int count_lines, line_calls;
#if LIGATURES_PATCH
static int run_calls, checked_run_calls;
#endif
#if BOXDRAW_PATCH
static int box_calls, checked_box_calls;
#endif

/* Function instrumentation measures calls without production test hooks. */
void __attribute__((no_instrument_function))
__cyg_profile_func_enter(void *function, void *caller)
{
	(void)caller;
	if (count_lines && function == (void *)xdrawline)
		line_calls++;
	#if LIGATURES_PATCH
	if (count_lines && function == (void *)textrun)
		run_calls++;
	#endif
	#if BOXDRAW_PATCH
	if (count_lines && function == (void *)drawbox)
		box_calls++;
	#endif
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
static void test_destroy_callback(struct wl_callback *cb) { assert(cb); callbacks_destroyed++; }
static int test_clock_gettime(clockid_t id, struct timespec *now)
{
	if (fake_clock && id == CLOCK_MONOTONIC) {
		*now = fake_now;
		return 0;
	}
	return clock_gettime(id, now);
}
static int test_malloc_trim(size_t pad)
{
	allocator_trims++;
	return malloc_trim(pad);
}
static void test_ack_configure(struct xdg_surface *surface, uint32_t serial)
{
	assert(!surface && serial); configures_acked++;
}
static void test_commit(struct wl_surface *s)
{
	assert(!s); commits++;
	/* Ordinary pixel tests have a responsive compositor. Pacing scenarios
	 * explicitly retain callbacks and advance the monotonic clock. */
	if (!fake_clock && framecb) frame_done(NULL, framecb, 0);
}
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
	curbuf = &buffers[0]; xw.draw = curbuf->cr;
	xw.pix = curbuf->pix;
	present();
	assert(commits == 1 && attached == buffers[0].buffer && nreported == 2);
	assert(!curbuf && !xw.pix && !xw.draw && !buffers[0].damage.count);
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

/* A synthetic color glyph exercises hidden text without any emoji-font
 * dependency. Its RGB ignores the foreground color during composition. */
static void hiddencolorglyph(void)
{
	ShmBuffer *b = &buffers[0];
	struct fcft_glyph color = {.width = 3, .height = 3, .is_color_glyph = true};
	const uint32_t ink = 0xff10ef43u;
	const uint32_t modes[] = {0, ATTR_INVISIBLE, ATTR_BLINK, ATTR_BLINK, 0};
	const int blink[] = {0, 0, 0, 1, 0};
	int oldmode = win.mode;
	#if ANYSIZE_PATCH
	int px = win.hborderpx + 2 * win.cw, py = win.vborderpx + 2 * win.ch;
	#else
	int px = borderpx + 2 * win.cw, py = borderpx + 2 * win.ch;
	#endif
	GlyphFontSpec spec = {.imageglyph = &color, .x = px, .y = py};
	Glyph base = {.u = 'X', .fg = defaultfg, .bg = defaultbg};
	color.pix = pixman_image_create_bits(PIXMAN_a8r8g8b8, 3, 3, NULL, 0);
	assert(color.pix);
	for (int y = 0; y < 3; y++) for (int x = 0; x < 3; x++)
		*(uint32_t *)((char *)pixman_image_get_data(color.pix) +
			y * pixman_image_get_stride(color.pix) + x * 4) = ink;
	curbuf = NULL; lastbuf = b; b->busy = 0; forcedraw = 1;
	assert(xstartdraw());
	for (size_t i = 0; i < LEN(modes); i++) {
		win.mode = MODE_VISIBLE | (blink[i] ? MODE_BLINK : 0);
		base.mode = modes[i] | ATTR_UNDERLINE | ATTR_STRUCK;
		#if WIDE_GLYPHS_PATCH
		/* Exercise the real separate background/foreground passes. */
		for (int pass = DRAW_BG; pass <= DRAW_FG; pass <<= 1)
		#endif
			xdrawglyphfontspecs(&spec, base, 1, 2, 2
				#if WIDE_GLYPHS_PATCH
				, pass
				#endif
				#if LIGATURES_PATCH
				, 1
				#endif
			);
		if ((modes[i] & ATTR_INVISIBLE) || blink[i]) {
			/* Neither colored text nor decorations may make translucent
			 * background pixels opaque when the cell is hidden. */
			for (int y = 0; y < win.ch; y++) for (int x = 0; x < win.cw; x++)
				assert(*at(b, px + x, py + y) == premulpixel(&dc.col[defaultbg]));
		} else {
			for (int y = 0; y < 3; y++) for (int x = 0; x < 3; x++)
				assert(*at(b, px + x, py + y) == ink);
		}
	}
	win.mode = oldmode;
	nreported = 0; present(); b->busy = 0;
	pixman_image_unref(color.pix);
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

static void tick(long milliseconds)
{
	fake_now.tv_sec += milliseconds / 1000;
	fake_now.tv_nsec += milliseconds % 1000 * 1000000L;
	if (fake_now.tv_nsec >= 1000000000L) {
		fake_now.tv_sec++;
		fake_now.tv_nsec -= 1000000000L;
	}
}

/* Start a real outstanding callback, then miss its first deadline. */
static void misscallback(void)
{
	if (framecb) frame_done(NULL, framecb, 0);
	forcedraw = 1; assert(frame() && framecb);
	tick(100);
	forcedraw = 1; assert(frame());
	tick(1);
	assert(framewait(&fake_now) > 100);
}

/* Calls to draw() include the pacing check used by direct redraw paths.
 * Measure actual commits, preserving one callback across a blackout, instead
 * of merely checking the shape of the backoff formula. */
static void framepacing(void)
{
	const size_t size = (size_t)win.w * win.h * 4;
	unsigned char *got = xmalloc(size), *full = xmalloc(size);
	struct wl_callback *pending, *stale;
	struct timespec requested;
	unsigned int before, requested_callbacks, destroyed_callbacks;
	double wait;
	int last_commit_ms = 0;

	win.mode = MODE_VISIBLE | MODE_FOCUSED | MODE_HIDE;
	for (int i = 0; i < NBUFFERS; i++) buffers[i].busy = 0;
	if (framecb) frame_done(NULL, framecb, 0);
	fake_clock = 1; fake_now = (struct timespec){10000, 0};
	forcedraw = 1; assert(frame() && framecb);
	pending = framecb;
	requested_callbacks = callback_id; destroyed_callbacks = callbacks_destroyed;
	tick(1); wait = framewait(&fake_now); requested = framereq;
	assert(wait > 0 && wait < 100);
	for (int i = 0; i < 1000; i++) {
		assert(framewait(&fake_now) == wait);
		assert(!memcmp(&requested, &framereq, sizeof requested));
	}
	/* A deadline does not advance unless a buffer actually gets committed. */
	tick(99); before = commits;
	for (int i = 0; i < NBUFFERS; i++) buffers[i].busy = 1;
	cells[2][3].u = '0'; dirty[2] = 1;
	for (int i = 0; i < 5; i++) {
		draw();
		assert(commits == before && presentpending && dirty[2]);
		assert(framewait(&fake_now) < 0);
		assert(!memcmp(&requested, &framereq, sizeof requested));
	}
	bufferrelease(lastbuf, lastbuf->buffer);
	assert(frame() && !dirty[2] && framecb == pending);
	for (int i = 0; i < NBUFFERS; i++) buffers[i].busy = 0;

	before = commits;
	for (int ms = 10; ms <= 30000; ms += 10) {
		tick(10);
		cells[2][3].u = 'A' + ms / 10 % 26; dirty[2] = 1;
		/* Forced redraws caused by terminal output must obey pacing too. */
		if (ms % 970 == 0) forcedraw = 1;
		if (frame()) {
			assert(ms - last_commit_ms <= 2010);
			last_commit_ms = ms;
		} else {
			assert(dirty[2] && presentpending);
		}
		assert(framecb == pending);
	}
	assert(commits - before >= 10 && commits - before < 30);
	assert(30000 - last_commit_ms <= 2010);
	assert(callback_id == requested_callbacks && callbacks_destroyed == destroyed_callbacks);
	assert(framewait(&fake_now) > 0 && dirty[2]);
	/* A late callback wakes pending content immediately, including its last
	 * update; the restored pixels must match a complete repaint. */
	stale = framecb; frame_done(NULL, framecb, 0);
	assert(framewait(&fake_now) < 0 && frame());
	assert(!dirty[2] && !presentpending && framecb && framecb != stale);
	snapshot(got);
	frame_done(NULL, framecb, 0); forcedraw = 1; assert(frame());
	snapshot(full); assert(!memcmp(got, full, size));
	/* An obsolete callback must not release or reset a newer long wait. */
	tick(100); forcedraw = 1; assert(frame());
	tick(1); pending = framecb; wait = framewait(&fake_now);
	assert(wait > 100);
	frame_done(NULL, stale, 0);
	assert(framecb == pending && framewait(&fake_now) == wait);

	/* Real interaction and configure handlers recover from missed callbacks.
	 * On a normally paced surface, input retains compositor synchronization. */
	for (int event = 0; event < 4; event++) {
		misscallback(); before = commits;
		cells[2][3].u = 'a' + event; dirty[2] = 1;
		switch (event) {
		case 0: win.mode &= ~MODE_FOCUSED; focusin(); break;
		case 1: keyboard_key(NULL, NULL, 1, 0, 30, WL_KEYBOARD_KEY_STATE_PRESSED); break;
		case 2: pointer_motion(NULL, NULL, 0, wl_fixed_from_int(2), wl_fixed_from_int(2)); break;
		case 3: {
			unsigned int acknowledged = configures_acked;
			pendingw = win.w; pendingh = win.h;
			xdgsurface_configure(NULL, NULL, 1);
			assert(configures_acked == acknowledged + 1);
			break;
		}
		}
		assert(framewait(&fake_now) < 0 && frame() && commits == before + 1);
		assert(framewait(&fake_now) > 0 && framewait(&fake_now) <= 100);
		cells[2][3].u = 'z' - event; dirty[2] = 1;
		keyboard_key(NULL, NULL, 1, 0, 30, WL_KEYBOARD_KEY_STATE_PRESSED);
		assert(framewait(&fake_now) > 0 && !frame() && dirty[2]);
	}
	/* Visible but unfocused surfaces still follow every compositor callback. */
	win.mode &= ~MODE_FOCUSED;
	before = commits;
	for (int i = 0; i < 120; i++) {
		tick(16); frame_done(NULL, framecb, 0);
		cells[2][3].u = 'A' + i % 26; dirty[2] = 1;
		assert(frame() && !dirty[2] && !presentpending);
	}
	assert(commits == before + 120);
	frame_done(NULL, framecb, 0);
	fake_clock = 0;
	free(got); free(full);
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

#if LIGATURES_PATCH
/* Build a cold working set, then draw the real screen last. Idle reclamation
 * must shrink cache ownership without changing pixels, retain recent words,
 * and leave no timer until later text-cache activity rearms it. */
static void fillcoldcache(void)
{
	uint32_t text[256];
	for (size_t j = 0; j < LEN(text); j++) text[j] = 'x';
	for (unsigned int i = 0;
	     textrun_bytes + textrun_imagebytes <= TEXTRUN_IDLE_BYTES + 256 * 1024u;
	     i++) {
		assert(i < 2048);
		text[0] = 'A' + i % 26;
		text[1] = 'A' + i / 26 % 26;
		text[2] = 'A' + i / (26 * 26);
		assert(textrun(dc.font.match, text, LEN(text)));
		textrun_endframe();
	}
}

static void idlecache(void)
{
	const uint32_t hot[] = {'f', 'f', 'i', '_', 'h', 'o', 't'};
	struct timespec now = {1000, 0};
	size_t size = (size_t)win.w * win.h * 4, before;
	unsigned char *reference = xmalloc(size), *rebuilt = xmalloc(size);
	const TextRun *recent;
	struct fcft_font *font = dc.font.match;
	unsigned int committed;
	double remaining;

	for (int i = 0; i < NBUFFERS; i++) buffers[i].busy = 0;
	curbuf = NULL; textrun_clear();
	textrunidle.pending = 0;
	assert(trimtextruns(&now) < 0 && !textrunidle.pending);
	fillcoldcache();
	forcedraw = 1; assert(frame());
	snapshot(reference);
	recent = textrun(font, hot, LEN(hot));
	assert(recent && textrun_activity);
	textrun_endframe();
	before = textrun_bytes + textrun_imagebytes;
	assert(before > TEXTRUN_IDLE_BYTES);
	committed = commits;
	remaining = trimtextruns(&now);
	assert(remaining == TEXTRUN_IDLE && textrunidle.pending && !textrun_activity);
	now.tv_sec += 10;
	assert(trimtextruns(&now) == TEXTRUN_IDLE - 10000 && commits == committed);
	/* A cache hit extends the deadline; protocol-only wakeups above do not. */
	assert(textrun(font, hot, LEN(hot)) == recent);
	textrun_endframe();
	assert(trimtextruns(&now) == TEXTRUN_IDLE);
	now.tv_sec += 29;
	assert(trimtextruns(&now) == 1000);
	/* Reclamation never invalidates glyph specs in a frame in progress. */
	curbuf = lastbuf;
	now.tv_sec += 2;
	assert(trimtextruns(&now) < 0 && textrunidle.pending);
	assert(textrun_bytes + textrun_imagebytes == before);
	curbuf = NULL;
	assert(trimtextruns(&now) < 0 && !textrunidle.pending);
	assert(textrun_bytes + textrun_imagebytes <= TEXTRUN_IDLE_BYTES &&
	       textrun_bytes + textrun_imagebytes < before);
	assert(dc.font.match == font && commits == committed);
	snapshot(rebuilt); assert(!memcmp(reference, rebuilt, size));
	now.tv_sec += 300;
	assert(trimtextruns(&now) < 0 && !textrunidle.pending);
	assert(textrun(font, hot, LEN(hot)) == recent);
	textrun_endframe();
	assert(trimtextruns(&now) < 0); /* a small hot set needs no idle timer */

	/* Combined SHM and cache reclamation still allows the same frame to be
	 * reconstructed, including font-owned and copied ligature bitmaps. */
	for (int i = 0; i < NBUFFERS; i++) {
		buffers[i].busy = 0;
		buffers[i].used = (struct timespec){now.tv_sec - 6, 0};
	}
	assert(trimbuffers(&now) < 0 && !lastbuf);
	assert(frame());
	snapshot(rebuilt); assert(!memcmp(reference, rebuilt, size));
	assert(dc.font.match == font);
	fillcoldcache();
	assert(trimtextruns(&now) == TEXTRUN_IDLE && textrunidle.pending);
	textrun_clear();
	assert(trimtextruns(&now) < 0 && !textrunidle.pending);
	free(reference); free(rebuilt);
}
#endif

#if REFLOW_PATCH
#include <pty.h>
/* The real parser and scrolling code of st.o, fed through a pty used as -l line. */
static int ptym, step;
static uint32_t seed = 20261119;
static unsigned char *shots[3];
static int checked_row_damage[ROWS];

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
	#if OPENURLONCLICK_PATCH
	int pending_url = url_draw, consumed_url;
	#endif

	snapshot(prev);
	for (int i = 0; i < NBUFFERS; i++) buffers[i].busy = hold && &buffers[i] == lastbuf;
	nreported = 0; line_calls = 0; count_lines = 1;
	#if LIGATURES_PATCH
	run_calls = 0;
	#endif
	#if BOXDRAW_PATCH
	box_calls = 0;
	#endif
	draw();
	#if OPENURLONCLICK_PATCH
	consumed_url = url_draw;
	#endif
	count_lines = 0; calls = line_calls;
	#if LIGATURES_PATCH
	checked_run_calls = run_calls;
	#endif
	#if BOXDRAW_PATCH
	checked_box_calls = box_calls;
	#endif
	assert(!curbuf && lastbuf && !term.nscrolls);
	for (int i = 0; i < NBUFFERS; i++) buffers[i].busy = 0;
	snapshot(got);
	if (commits == before)
		assert(!memcmp(prev, got, size));
	/* Preserve the protocol coverage before the reference repaint replaces
	 * it. This also lets a focused case prove that partial updates ran. */
	for (int y = 0; y < ROWS; y++) {
		#if ANYSIZE_PATCH
		int py = win.vborderpx + y * win.ch + win.ch / 2;
		#else
		int py = borderpx + y * win.ch + win.ch / 2;
		#endif
		checked_row_damage[y] = 0;
		for (int x = 0; x < win.w; x++)
			checked_row_damage[y] += inreported(x, py);
	}
	for (int y = 0; y < win.h; y++) for (int x = 0; x < win.w; x++)
		if (memcmp(prev + ((size_t)y * win.w + x) * 4,
		           got + ((size_t)y * win.w + x) * 4, 4) && !inreported(x, y)) {
			fprintf(stderr, "step %d: changed pixel (%d, %d) outside damage\n", step, x, y);
			abort();
		}
	if (!compare)
		return calls;
	memcpy(saved, buffers, sizeof saved); last = lastbuf;
	#if OPENURLONCLICK_PATCH
	/* URL drawing is a one-shot request consumed by the renderer. Give the
	 * independent full repaint the same input, then restore actual state. */
	url_draw = pending_url;
	#endif
	tfulldirt(); forcedraw = 1; draw();
	#if OPENURLONCLICK_PATCH
	url_draw = consumed_url;
	#endif
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

static void pendingcovers(int row, int left, int right)
{
	assert(term.dirty[row]);
	if (term.dirty[row] == DIRTY_PARTIAL) {
		assert(term.dirtycols);
		assert(term.dirtycols[row].left <= left);
		assert(term.dirtycols[row].right >= right);
	}
}

static void deferredupdates(void)
{
	struct wl_callback *pending;
	struct timespec requested;
	unsigned int before;
	double interval;

	if (ime.preedit) ime_clearpreedit();
	selclear();
	feed("\033[?1049h\033[r\033[?7l\033[?25l\033[m\033[2J");
	feed("\033[3;1Haa 123 bb 456 cc 789 dd\033[%d;%dH", ROWS, COLS);
	scrollframe(0, 1);
	fake_clock = 1; fake_now = (struct timespec){20000, 0};
	misscallback(); before = commits; pending = framecb;
	/* Real parser writes continue while every attempted draw waits. */
	for (int i = 0; i < 8; i++) {
		tick(10);
		feed("\033[3;%dH%c\033[%d;%dH", 4 + 2 * i, 'A' + i, ROWS, COLS);
		assert(!frame() && commits == before && framecb == pending);
		pendingcovers(2, 3, 4 + 2 * i);
	}
	/* OSC palette changes call redraw directly. They must accumulate with
	 * ordinary PTY updates instead of accidentally unthrottling them. */
	feed("\033]4;1;#123456\007");
	assert(commits == before && framecb == pending && presentpending);
	feed("\033[4;7H\xe4\xb8\xad\033[5;6H\xe2\x94\x82\033[%d;%dH", ROWS, COLS);
	assert(!frame());
	/* Missing free buffers must not push the pending callback deadline out. */
	tick(300); requested = framereq; interval = frameinterval;
	for (int i = 0; i < NBUFFERS; i++) buffers[i].busy = 1;
	for (int i = 0; i < 4; i++) {
		draw();
		assert(commits == before && presentpending && term.dirty[2]);
		assert(frameinterval == interval && framewait(&fake_now) < 0);
		assert(!memcmp(&requested, &framereq, sizeof requested));
	}
	bufferrelease(lastbuf, lastbuf->buffer);
	/* Recovery draws precisely the current parser state and reports all
	 * changed pixels; scrollframe compares against an independent repaint. */
	frame_done(NULL, framecb, 0); fake_clock = 0;
	step = -1700; scrollframe(1, 1);
	assert(!term.dirty[2] && !term.dirty[3] && !term.dirty[4] && !presentpending);
	feed("\033]104;1\007\033[m\033[r\033[?7h\033[?25h\033[?1049l");
	scrollframe(0, 1);
}

/* Dirty ranges belong to the not-yet-painted view. Accumulate several writes,
 * let buffers stay busy, and draw several partial frames before consulting the
 * independent full renderer so it cannot refresh the shadow between edits. */
static void dirtyupdates(void)
{
	unsigned int before;
	int turn = 0;

	if (ime.preedit) ime_clearpreedit();
	selclear();
	feed("\033[?1049h\033[r\033[?7l\033[?25l\033[m\033[2J");
	for (int y = 0; y < ROWS; y++)
		feed("\033[%d;1Haa 123 bb 456 cc 789 dd", y + 1);
	feed("\033[%d;%dH", ROWS, COLS);
	scrollframe(0, 1);

	/* Multiple independent parser writes to one row form one covering range. */
	feed("\033[3;5HX\033[3;17HY\033[%d;%dH", ROWS, COLS);
	assert(term.dirty[2] == DIRTY_PARTIAL);
	pendingcovers(2, 4, 17);
	step = -1200; scrollframe(1, 0);
	assert(!term.dirty[2]);
	feed("\033[3;11HZZ\033[%d;%dH", ROWS, COLS);
	assert(term.dirty[2] == DIRTY_PARTIAL);
	pendingcovers(2, 10, 12);
	step = -1201; scrollframe(0, 0);
	feed("\033[3;5H7\033[3;17H8\033[%d;%dH", ROWS, COLS);
	step = -1202; scrollframe(1, 1);

	/* An old-style full-row invalidation overrides an existing narrow hint.
 * A following narrow write must not hide an unrelated changed cell. */
	feed("\033[3;5HM\033[%d;%dH", ROWS, COLS);
	assert(term.dirty[2] == DIRTY_PARTIAL);
	term.line[2][20].u = 'Q'; term.dirty[2] = 1;
	feed("\033[3;7HN\033[%d;%dH", ROWS, COLS);
	assert(term.dirty[2] == 1);
	step = -1203; scrollframe(0, 1);
	feed("\033[3;11H1\033[%d;%dH", ROWS, COLS);
	assert(term.dirty[2] == DIRTY_PARTIAL);
	step = -1204; scrollframe(1, 1);

	/* A failed frame consumes neither the pending range nor the text shadow. */
	for (int i = 0; i < NBUFFERS; i++) buffers[i].busy = 1;
	before = commits;
	feed("\033[3;4HA\033[%d;%dH", ROWS, COLS);
	draw(); assert(commits == before && presentpending);
	pendingcovers(2, 3, 4);
	feed("\033[3;19HB\033[%d;%dH", ROWS, COLS);
	draw(); assert(commits == before && presentpending);
	pendingcovers(2, 3, 19);
	step = -1205; scrollframe(1, 1);

	/* Both halves of a repaired wide character are changes, including the
 * neighbour outside the bytes the application explicitly overwrote. */
	feed("\033[3;7H\xe4\xb8\xad\033[%d;%dH", ROWS, COLS);
	step = -1210; scrollframe(0, 0);
	feed("\033[3;7HX\033[%d;%dH", ROWS, COLS);
	pendingcovers(2, 6, 8);
	step = -1211; scrollframe(1, 0);
	feed("\033[3;7H\xe4\xb8\xad\033[%d;%dH", ROWS, COLS);
	step = -1212; scrollframe(0, 0);
	feed("\033[3;8HY\033[%d;%dH", ROWS, COLS);
	pendingcovers(2, 6, 8);
	step = -1213; scrollframe(1, 1);

	/* Structural changes and erasure after a narrow write must cover every
 * shifted or cleared cell; a later narrow write cannot shrink that work. */
	const char *structural[] = {
		"\033[3;4HX\033[3;10H\033[2@AB\033[3;6HY",
		"\033[3;17HX\033[3;5H\033[3P\033[3;8HY",
		"\033[3;4HX\033[3;12H\033[K\033[3;6HY",
		"\033[3;19HX\033[3;8H\033[1K\033[3;15HY",
		"\033[3;4HX\033[3;10H\033[4X\033[3;20HY",
		"\033[3;4HX\033[3;10H\033[4hAB\033[4l\033[3;20HY",
	};
	for (size_t i = 0; i < LEN(structural); i++) {
		feed("\033[3;1H\033[2Kaa 123 bb 456 cc 789 dd\033[%d;%dH", ROWS, COLS);
		scrollframe(turn++ % 2, 0);
		feed("%s\033[%d;%dH", structural[i], ROWS, COLS);
		step = -1220 - i; scrollframe(turn++ % 2, 1);
	}

	/* Dirty rows with pending ranges move through scroll regions before the
 * renderer sees them; subsequent ordinary frames use the new row identities. */
	feed("\033[4;6HX\033[6;18HY\033[2;7r\033[7;1H\n\033[3;10HZ\033[r\033[%d;%dH", ROWS, COLS);
	step = -1230; scrollframe(1, 0);
	feed("\033[3;15HQ\033[6;4HR\033[%d;%dH", ROWS, COLS);
	step = -1231; scrollframe(0, 0);
	feed("\033[3;5HS\033[%d;%dH", ROWS, COLS);
	step = -1232; scrollframe(1, 1);

	/* Repeated small edits should not need a full reference frame to keep
 * their saved text current, including when the same cell is written twice. */
	for (int i = 0; i < 48; i++) {
		int y = 1 + rnd(ROWS - 1), x = 2 + rnd(COLS - 3);
		feed("\033[%d;%dH%c\033[%d;%dH%c\033[%d;%dH",
			y, x, 'a' + rnd(26), y, x, 'a' + rnd(26), ROWS, COLS);
		step = -1240 - i;
		scrollframe(i % 2, i % 8 == 7);
	}
	feed("\033[m\033[r\033[?7h\033[?25h\033[?1049l");
	scrollframe(0, 1);
}

#if BOXDRAW_PATCH
/* A border in an otherwise ordinary row must not force every numeric update
 * through the full renderer. If the changed word or its ink reaches geometry,
 * the unmodified vector path must still produce the full reference picture. */
static void mixedupdates(void)
{
	if (ime.preedit) ime_clearpreedit();
	selclear();
	feed("\033[?1049h\033[r\033[?7l\033[?25l\033[m\033[2J");
	feed("\033[3;1H\xe2\x94\x82 aa 123 bb 456 cc \033[3;%dH\xe2\x94\x82\033[%d;%dH",
		COLS, ROWS, COLS);
	scrollframe(0, 1);
	assert(term.line[2][0].mode & ATTR_BOXDRAW);
	assert(term.line[2][COLS - 1].mode & ATTR_BOXDRAW);
	for (int i = 0; i < 4; i++) {
		feed("\033[3;7H%c\033[%d;%dH", '4' + i, ROWS, COLS);
		step = -1300 - i;
		scrollframe(i % 2, i == 3);
		#if LIGATURES_PATCH && WIDE_GLYPHS_PATCH
		/* A newly selected buffer can still carry full damage from setup.
		 * The first update uses the just-presented buffer and isolates this
		 * edit's damage; later updates exercise rotation and pixel equality. */
		if (i == 0)
			assert(checked_row_damage[2] > 0 && checked_row_damage[2] < win.w / 2);
		#endif
	}

	static const struct { const char *before, *edit; } cases[] = {
		{"\xe2\x94\x82" "123 aa bb cc", "\033[3;3H9"},
		{"aa bb cc 123\xe2\x94\x82", "\033[3;10H9"},
		{"\xe2\x94\x82 abc 123 def \xe2\x94\x82", "\033[3;1Hx"},
		{"x abc 123 def \xe2\x94\x82", "\033[3;1H\xe2\x94\x82"},
		{"\xe2\x94\x82 abc 123 def \xe2\x94\x82", "\033[3;1H\xe2\x94\x83"},
		{"\xe2\x94\x82 \xe2\x96\x88\xe2\x96\x80 123 \xe2\x94\x82", "\033[3;3H\xe2\x96\x84"},
		{"\xe2\x94\x82 aa \xe4\xb8\xad \xe2\x94\x82", "\033[3;7HX"},
		{"\xe2\x94\x82 aa 123 \xe2\x94\x82", "\033[3;9H\xe4\xb8\xad"},
		{"\xe2\x94\x82 aa \033[3mfff\033[m \xe2\x94\x82", "\033[3;7H\033[3mj\033[m"},
		{"\xe2\x94\x82 aa \033[4m123\033[m \xe2\x94\x82", "\033[3;7HX"},
	};
	for (size_t i = 0; i < LEN(cases); i++) {
		feed("\033[3;1H\033[m\033[2K%s\033[m\033[%d;%dH", cases[i].before, ROWS, COLS);
		scrollframe(i % 2, 0);
		feed("%s\033[%d;%dH", cases[i].edit, ROWS, COLS);
		step = -1310 - i;
		scrollframe(!(i % 2), 1);
	}
	#if LIGATURES_PATCH && WIDE_GLYPHS_PATCH
	/* New text can overhang a distant border. The old and new ink bounds
	 * must force the same background/vector ordering as a full repaint. */
	{
		const uint32_t cp = '@';
		const TextRun *run;
		TextrunGlyph *glyph;
		int dx;
		feed("\033[3;1H\033[2K\xe2\x94\x82 aa 123 bb\033[3;%dH\xe2\x94\x82\033[%d;%dH",
			COLS, ROWS, COLS);
		textrun_clear(); tfulldirt(); forcedraw = 1; scrollframe(0, 1);
		run = textrun(dc.font.match, &cp, 1);
		assert(run && run->count == 1);
		glyph = run->glyphs; dx = glyph->dx;
		glyph->dx -= 10 * win.cw;
		feed("\033[3;11H\033[31m@\033[m\033[%d;%dH", ROWS, COLS);
		step = -1330; scrollframe(1, 0);
		feed("\033[3;7H\033[44m9\033[m\033[%d;%dH", ROWS, COLS);
		step = -1331; scrollframe(0, 0);
		feed("\033[3;11H\033[31mx\033[m\033[%d;%dH", ROWS, COLS);
		step = -1332; scrollframe(1, 1);
		assert(textrun(dc.font.match, &cp, 1) == run && run->glyphs == glyph);
		glyph->dx = dx;
	}
	#endif
	feed("\033[m\033[r\033[?7h\033[?25h\033[?1049l");
	scrollframe(0, 1);
}

static void geometrycell(int row, int col, Rune rune, const char *sgr)
{
	char encoded[5];
	encoded[utf8encode(rune, encoded)] = '\0';
	feed("\033[%d;%dH\033[%sm%s\033[m\033[%d;%dH",
		row, col, sgr, encoded, ROWS, COLS);
	assert(term.line[row - 1][col - 1].mode & ATTR_BOXDRAW);
}

/* Adjacent geometric cells are independent shaping boundaries. A dense graph
 * must redraw only the changed neighbourhood, while every supported geometry
 * family produces the same pixels as a full row (including alpha backgrounds). */
static void geometryupdates(void)
{
	static const Rune shapes[] = {
		0x2502, 0x2550, 0x256c, 0x2504, 0x256d, 0x256e, 0x2570,
		0x2571, 0x2573, 0x2580, 0x2584, 0x2592, 0x259a, 0x28b5,
		0xe0b0, 0xe0b5, 0xe0b7, 0xe0d2, 0x1fb3c, 0x1fb9c,
		0x1fbbd, 0x1fbbe, 0x1cd7f, 0x1ce51, 0xf5ed, 0xf60d,
	};
	static const char *styles[] = {"", "1", "2", "3", "7", "8", "31", "44"};
	if (ime.preedit) ime_clearpreedit();
	selclear();
	feed("\033[?1049h\033[r\033[?7l\033[?25l\033[m\033[2J");
	for (int x = 1; x <= COLS; x++)
		geometrycell(3, x, 0x256d, "");
	scrollframe(0, 1);
	for (size_t i = 0; i < LEN(shapes); i++) {
		geometrycell(3, 12, shapes[i], styles[i % LEN(styles)]);
		step = -1400 - i;
		scrollframe(0, 1);
		#if LIGATURES_PATCH && WIDE_GLYPHS_PATCH
		assert(checked_row_damage[2] > 0 && checked_row_damage[2] < win.w / 2);
		/* Counting actual geometry draws rejects an implementation which
		 * clips damage but still traverses all twenty-four symbols. */
		if (strcmp(styles[i % LEN(styles)], "8"))
			assert(checked_box_calls > 0 && checked_box_calls < COLS / 2);
		#endif
	}
	/* Text joins/splits at the old AND new geometry boundary; a wide dummy
	 * must remain attached to its base when either half is overwritten. */
	static const char *edits[] = {
		"\033[3;9Hffi", "\033[3;10H\xe2\x95\xad", "\033[3;10Hf",
		"\033[3;10H\xe4\xb8\xad", "\033[3;11H\xe2\x95\xae",
		"\033[3;10H\xe4\xb8\xad", "\033[3;10H\xe2\x95\xad",
		"\033[3;8H!=", "\033[3;9H\xee\x82\xb0", "\033[3;9H=",
		"\033[3;9H\033[4m\xe2\x95\xaf\033[m",
	};
	for (size_t i = 0; i < LEN(edits); i++) {
		feed("%s\033[%d;%dH", edits[i], ROWS, COLS);
		step = -1440 - i;
		scrollframe(i % 2, i % 3 == 2 || i == LEN(edits) - 1);
	}
	/* Several consecutive frames run without a reference redraw refreshing
	 * the shadow or glyph spill bounds. Rotate buffers and change another
	 * row too, so a leaked destination clip cannot hide behind one row. */
	for (int i = 0; i < 64; i++) {
		geometrycell(3, 2 + rnd(COLS - 3), shapes[rnd(LEN(shapes))],
			styles[rnd(LEN(styles))]);
		geometrycell(5, 2 + rnd(COLS - 3), shapes[rnd(LEN(shapes))], "32");
		step = -1460 - i;
		scrollframe(i % 2, i % 8 == 7);
	}
	feed("\033[m\033[r\033[?7h\033[?25h\033[?1049l");
	scrollframe(0, 1);
}

#if LIGATURES_PATCH && WIDE_GLYPHS_PATCH
/* New text ink reaches only half of an unchanged geometric cell. Its vector
 * path resets its own clip, and may use an alpha group or cached coverage;
 * drawing the other half twice would change antialiased pixels outside damage. */
static void geometryclip(void)
{
	static const Rune shapes[] = {0x256d, 0x2573, 0xe0b5, 0xe0b7,
		0x1fb3c, 0x1fbbd, 0x1fbbe, 0xf5ed, 0xf60d};
	const uint32_t cp = '@';
	const int target = 8, source = 11;
	#if ANYSIZE_PATCH
	const int border = win.hborderpx, top = win.vborderpx;
	#else
	const int border = borderpx, top = borderpx;
	#endif
	const int edge = border + (target - 1) * win.cw + win.cw / 2;
	const int py = top + 2 * win.ch + win.ch / 2;
	feed("\033[?1049h\033[r\033[?7l\033[?25l\033[m\033[2J");
	for (size_t i = 0; i < LEN(shapes); i++) {
		const TextRun *run;
		TextrunGlyph *glyph;
		int dx;
		feed("\033[3;1H\033[2K\033[%d;%dH", ROWS, COLS);
		geometrycell(3, target, shapes[i], "32");
		textrun_clear(); tfulldirt(); forcedraw = 1; scrollframe(0, 1);
		run = textrun(dc.font.match, &cp, 1);
		assert(run && run->count == 1 && textrun_glyph(run->glyphs[0].ref)->pix);
		glyph = run->glyphs; dx = glyph->dx;
		glyph->dx = (target - source) * win.cw + win.cw / 2 -
			textrun_glyph(glyph->ref)->x;
		for (int j = 0; j < 6; j++) {
			feed("\033[3;%dH\033[%dm%c\033[m\033[%d;%dH",
				source, 31 + j % 2, j % 3 == 2 ? ' ' : '@', ROWS, COLS);
			step = -1540 - i * 6 - j;
			scrollframe(j % 2, j == 5);
			if (j == 0) {
				assert(checked_box_calls > 0 && checked_box_calls < COLS / 2);
				assert(inreported(edge, py) && !inreported(edge - 1, py));
			}
		}
		assert(textrun(dc.font.match, &cp, 1) == run && run->glyphs == glyph);
		glyph->dx = dx;
	}
	feed("\033[m\033[r\033[?7h\033[?25h\033[?1049l");
	scrollframe(0, 1);
}
#endif
#endif

#if LIGATURES_PATCH && WIDE_GLYPHS_PATCH
/* A font can position a glyph far beyond its source word. Move a cached
 * one-character run so this case is deterministic for every installed font.
 * The full renderer and partial renderer consume the same synthetic run. */
static void distantink(void)
{
	const uint32_t cp = '@';
	size_t size = (size_t)win.w * win.h * 4;
	for (int direction = -1; direction <= 1; direction += 2) {
		const int source = direction < 0 ? 20 : 2;
		const int target = direction < 0 ? 4 : 18;
		const TextRun *run;
		TextrunGlyph *glyph;
		int dx;

		feed("\033[?1049h\033[r\033[?7l\033[?25l\033[m\033[2J");
		feed("\033[3;%dH\033[31m@\033[m\033[3;%dH1\033[%d;%dH",
			source, target, ROWS, COLS);
		textrun_clear();
		tfulldirt(); forcedraw = 1; scrollframe(0, 1);
		run = textrun(dc.font.match, &cp, 1);
		assert(run && run->count == 1 && textrun_glyph(run->glyphs[0].ref)->pix);
		glyph = run->glyphs;
		dx = glyph->dx;
		glyph->dx += (target - source) * win.cw;
		tfulldirt(); forcedraw = 1; scrollframe(0, 1);
		assert(memcmp(shots[0], shots[1], size)); /* The synthetic ink is visible. */

		/* The unchanged, remote '@' must survive backgrounds redrawn here.
		 * Consecutive partial frames do not get help from a full reference
		 * repaint rebuilding the renderer's saved bounds. */
		for (int i = 0; i < 3; i++) {
			step = -1100 - (direction + 1) * 3 - i;
			feed("\033[3;%dH\033[44m%c\033[m\033[%d;%dH",
				target, '2' + i, ROWS, COLS);
			scrollframe(i % 2, i == 2);
		}
		assert(textrun(dc.font.match, &cp, 1) == run && run->glyphs == glyph);

		/* A global overlay disables partial rendering. Its removal must
		 * leave enough context for subsequent ordinary partial frames. */
		ime.preedit = xstrdup("x"); ime.pre_begin = 0; ime.pre_end = 1;
		forcedraw = 1; scrollframe(0, 0);
		ime_clearpreedit(); forcedraw = 1; scrollframe(1, 0);
		for (int i = 0; i < 3; i++) {
			step = -1120 - (direction + 1) * 3 - i;
			feed("\033[3;%dH\033[42m%c\033[m\033[%d;%dH",
				target, '5' + i, ROWS, COLS);
			scrollframe(i % 2, i == 2);
		}
		assert(textrun(dc.font.match, &cp, 1) == run && run->glyphs == glyph);
		glyph->dx = dx;
		tfulldirt(); forcedraw = 1; scrollframe(0, 1);
		feed("\033[m\033[r\033[?7h\033[?25h\033[?1049l");
		scrollframe(0, 1);
	}
}
#endif

/* Editing a few cells must leave exactly the picture a full repaint produces.
 * Exercise both the old and new shaping contexts, and effects that change the
 * picture without changing the underlying characters. A released buffer and a
 * held previous buffer alternate so untouched pixels must survive rotation. */
static void partialupdates(void)
{
	static const struct {
		const char *before, *edit;
	} cases[] = {
		{"left 01234567 right", "\033[3;9H9"},
		{"left 01234567 right", "\033[3;6H12345678"},
		{"abc def ghi jkl", "\033[3;4Hx"},
		{"abcdefghijkl", "\033[3;5H "},
		{"ab != cd === ef -> gh", "\033[3;5H="},
		{"ab != cd === ef -> gh", "\033[3;11H "},
		{"ab != cd === ef -> gh", "\033[3;17H="},
		{"a ffi b ffl c fi d", "\033[3;4Hx"},
		{"a ffi b ffl c fi d", "\033[3;5H "},
		{"a f fi b f fl c", "\033[3;4Hf"},
		{"ab <-> cd !== ef", "\033[3;6H\033[1m>\033[m"},
		{"ab 123 cd 456 ef", "\033[3;5H\033[3m2\033[m"},
		{"ab 123 cd 456 ef", "\033[3;5H\033[7m2\033[m"},
		{"ab 123 cd 456 ef", "\033[3;5H\033[8m2\033[m"},
		{"ab 123 cd 456 ef", "\033[3;5H\033[38;2;120;60;230m2\033[m"},
		{"\033[3mfff jjj -> ==\033[m", "\033[3;4H\033[3mf\033[m"},
		{"\033[4mabcdef 123456\033[m", "\033[3;4Hx"},
		{"\033[9mabcdef 123456\033[m", "\033[3;7Hx"},
		{"\033[4:3mabcdef 123456\033[m", "\033[3;4Hx"},
		{"\033[4:3;58:2::200:40:90mabcdef 123456\033[m", "\033[3;8H\033[4:3m9\033[m"},
		{"abc \xe4\xb8\xad\xe6\x96\x87 def", "\033[3;5HX"},
		{"abc \xe4\xb8\xad\xe6\x96\x87 def", "\033[3;6HX"},
		{"abc ABCD def", "\033[3;6H\xe4\xb8\xad"},
		{"abc \xf0\x9f\x98\x80 def", "\033[3;6HX"},
		{"abc AB def", "\033[3;5H\xf0\x9f\x98\x80"},
		{"abc e\xcc\x81 f\xcc\x88 def", "\033[3;5Ha"},
		{"abcdefghijklmnop", "\033[3;5H\033[3P"},
		{"abcdefghijklmnop", "\033[3;5H\033[2@XY"},
		{"abcdefghijklmnop", "\033[3;8H\033[K"},
		{"abcdefghijklmnop", "\033[3;8H\033[1K"},
		{"abcdefghijklmnop", "\033[3;8H\033[3X"},
		{"\033[44mabcdefghijklmnop\033[m", "\033[3;8H\033[41m\033[3X\033[m"},
		{"aa \033[3mf\033[m 123 \033[3mj\033[m zz", "\033[3;7H9"},
		{"aa \033[3mf\033[m 123 \033[3mj\033[m zz", "\033[3;5H\033[44m \033[m"},
		{"aa \033[31mf\033[32mf\033[34mi\033[m zz", "\033[3;5H\033[35mf\033[m"},
		{"aa \033[41m!\033[42m=\033[44m=\033[m zz", "\033[3;5H\033[43m=\033[m"},
		{"aa != \033[44m \033[m -> zz", "\033[3;7H="},
		{"aa != \033[44m \033[m -> zz", "\033[3;6H\033[41m \033[m"},
		{"aa \033[3mffi\033[m zz", "\033[3;5H\033[3m \033[m"},
		{"aa \033[3mf fi\033[m zz", "\033[3;5H\033[3mf\033[m"},
		{"aa \xe4\xb8\xad != zz", "\033[3;6H\033[44m \033[m"},
		{"aa != \xe4\xb8\xad zz", "\033[3;6H\033[44m \033[m"},
		{"aa \xe4\xb8\xad\xe6\x96\x87 zz", "\033[3;5H "},
		{"aa \xf0\x9f\x98\x80 != zz", "\033[3;5H "},
		{"f 123 right", "\033[3;1H\033[3mj\033[m"},
		{"left 123            ffi", "\033[3;23H\033[3mj\033[m"},
		{"aa != zz", "\033[3;4H\033[38;2;7;230;80m!\033[m"},
		{"aa != zz", "\033[3;4H\033[48;2;70;23;180m!\033[m"},
	};
	int turn = 0;

	if (ime.preedit) ime_clearpreedit();
	selclear();
	feed("\033[?1049h\033[r\033[?7l\033[?25l\033[m\033[2J");
	for (int y = 0; y < ROWS; y++)
		feed("\033[%d;1Hrow%02d abc 012 xyz != ->", y + 1, y);
	feed("\033[%d;%dH", ROWS, COLS);
	scrollframe(0, 1);
	/* This assertion distinguishes an exercised optimization from tests
	 * that happen to pass because every update falls back to full rows. */
	feed("\033[3;1H\033[2Kleft 123 right\033[%d;%dH", ROWS, COLS);
	scrollframe(0, 1);
	feed("\033[3;7H9\033[%d;%dH", ROWS, COLS);
	scrollframe(0, 1);
	assert(checked_row_damage[2] > 0 && checked_row_damage[2] < win.w / 2);
	#if LIGATURES_PATCH && WIDE_GLYPHS_PATCH
	/* A tiny edit must avoid querying all twelve words. Pixel clipping alone
	 * passes the damage assertion above but still shapes the complete row. */
	feed("\033[3;1H\033[2Ka b c d e f g h i j k l\033[%d;%dH", ROWS, COLS);
	scrollframe(0, 1);
	feed("\033[3;11Hx\033[%d;%dH", ROWS, COLS);
	scrollframe(1, 1);
	assert(checked_run_calls > 0 && checked_run_calls < 12);
	#endif
	for (size_t i = 0; i < LEN(cases); i++) {
		feed("\033[3;1H\033[m\033[2K%s\033[m\033[%d;%dH", cases[i].before, ROWS, COLS);
		scrollframe(turn++ % 2, 1);
		feed("%s\033[%d;%dH", cases[i].edit, ROWS, COLS);
		step = -100 - i;
		scrollframe(turn++ % 2, 1);
	}

	/* Small random edits interleaved with preserved pixels in other rows. */
	for (int i = 0; i < 300; i++) {
		int y = 1 + rnd(ROWS - 1), x = 1 + rnd(COLS);
		feed("\033[%d;%dH\033[%dm%c\033[m\033[%d;%dH",
			y, x, 30 + rnd(8), "a >!=0123"[rnd(9)], ROWS, COLS);
		if (i % 7 == 0)
			feed("\033[%d;%dH%c\033[%d;%dH", 1 + rnd(ROWS - 1),
				1 + rnd(COLS), 'a' + rnd(26), ROWS, COLS);
		step = -200 - i;
		scrollframe(turn++ % 2, 1);
	}

	/* The shadow of a glyph cannot capture a selection or a palette change. */
	step = -600;
	selstart(2, 2, 0); selextend(17, 4, SEL_REGULAR, 0);
	scrollframe(turn++ % 2, 1);
	feed("\033[3;6Hnew\033[%d;%dH", ROWS, COLS); scrollframe(turn++ % 2, 1);
	selclear(); scrollframe(turn++ % 2, 1);
	feed("\033[3;1H\033[31mred text\033[44m blue bg\033[m\033[%d;%dH", ROWS, COLS);
	scrollframe(turn++ % 2, 1);
	feed("\033]4;1;#1234ab\033\\"); scrollframe(turn++ % 2, 1);
	feed("\033]4;4;#fe9345\033\\"); scrollframe(turn++ % 2, 1);
	feed("\033]10;#7f5c3a\033\\\033]11;#152535\033\\"); scrollframe(turn++ % 2, 1);
	xloadcols(); tfulldirt(); forcedraw = 1; scrollframe(turn++ % 2, 1);
	feed("\033[3;1H\033[5mblink test\033[m\033[%d;%dH", ROWS, COLS);
	scrollframe(turn++ % 2, 1);
	win.mode ^= MODE_BLINK; tsetdirtattr(ATTR_BLINK); scrollframe(turn++ % 2, 1);
	win.mode ^= MODE_BLINK; tsetdirtattr(ATTR_BLINK); scrollframe(turn++ % 2, 1);
	win.mode ^= MODE_REVERSE; tfulldirt(); scrollframe(turn++ % 2, 1);
	win.mode ^= MODE_REVERSE; tfulldirt(); scrollframe(turn++ % 2, 1);

	/* Several edits before a buffer release must not advance the shadow of
	 * the displayed frame or forget damage that has not yet been painted. */
	step = -700;
	for (int i = 0; i < NBUFFERS; i++) buffers[i].busy = 1;
	unsigned int before = commits;
	feed("\033[3;5Hbusy"); draw();
	assert(commits == before && presentpending);
	feed("\033[4;7Hmore\033[%d;%dH", ROWS, COLS);
	scrollframe(1, 1);

	/* Line identities change on scrolling, screen swaps and insert/delete. */
	step = -800;
	feed("\033[2;7r\033[7;1H\n\033[3;7H12\033[6;11H34\033[%d;%dH", ROWS, COLS);
	scrollframe(turn++ % 2, 1);
	feed("\033[3;1H\033[L\033[3;8H56\033[5;1H\033[M\033[r\033[%d;%dH", ROWS, COLS);
	scrollframe(turn++ % 2, 1);
	feed("\033[?1049l"); scrollframe(turn++ % 2, 1);
	feed("\033[?1049h"); scrollframe(turn++ % 2, 1);
	feed("\033[3;8H78\033[%d;%dH", ROWS, COLS); scrollframe(turn++ % 2, 1);
	#if OPENURLONCLICK_PATCH
	step = -900;
	feed("\033[3;1H\033[2Khttp://a.bc/link text\033[%d;%dH", ROWS, COLS);
	scrollframe(0, 1);
	step = -901; assert(detecturl(5, 2, 1)); scrollframe(1, 1);
	step = -902; clearurl(); scrollframe(0, 1);
	step = -903;
	feed("\033[3;13Hx\033[%d;%dH", ROWS, COLS); scrollframe(1, 1);
	#endif

	/* Reload a visually different font while keeping identical cell metrics:
	 * unchanged Glyph values must still repaint the new font's bitmaps. */
	{
		const char *configured = getenv("RENDER_STATE_FONT");
		const char *original = configured && *configured ? configured : "DejaVu Sans Mono:pixelsize=16";
		char boldfont[512];
		int cw = win.cw, ch = win.ch;
		assert(snprintf(boldfont, sizeof boldfont, "%s:weight=bold", original) < (int)sizeof boldfont);
		step = -910;
		feed("\033[3;1H\033[2Kabc def 123 != ->\033[%d;%dH", ROWS, COLS);
		scrollframe(0, 1);
		step = -911; xunloadfonts(); xloadfonts(boldfont, 0);
		assert(win.cw == cw && win.ch == ch);
		tfulldirt(); scrollframe(1, 1);
		step = -912; xunloadfonts(); xloadfonts(original, 0);
		assert(win.cw == cw && win.ch == ch);
		tfulldirt(); scrollframe(0, 1);
	}

	/* Cursor/IME restoration may need more than the edited cells. */
	step = -1000;
	feed("\033[3;1Habc != def -> ghi\033[3;5H\033[?25h"); scrollframe(turn++ % 2, 1);
	for (int style = 2; style <= 6; style += 2) {
		win.cursor = style;
		feed("\033[3;5H=\033[3;12H"); scrollframe(turn++ % 2, 1);
		feed("\033[3;6H!\033[3;4H"); scrollframe(turn++ % 2, 1);
	}
	ime.preedit = xstrdup("ab\xCE\xBB"); ime.pre_begin = 1; ime.pre_end = 2;
	forcedraw = 1; scrollframe(turn++ % 2, 1);
	feed("\033[3;10H42\033[3;4H"); scrollframe(turn++ % 2, 1);
	ime_clearpreedit(); forcedraw = 1; scrollframe(turn++ % 2, 1);
	win.mode ^= MODE_FOCUSED; scrollframe(turn++ % 2, 1);
	win.mode ^= MODE_FOCUSED; scrollframe(turn++ % 2, 1);
	feed("\033[m\033[r\033[?7h\033[?25h\033[?1049l");
	win.cursor = 2;
	scrollframe(0, 1);
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

/* A configure may resize the grid several times before a frame is possible.
 * Both the pty size and the eventual picture must use the latest dimensions,
 * including when every old-size buffer is still held by the compositor. */
static void resizewindow(int cols, int rows, int extra)
{
	struct winsize ws;
	xw.lw = cols * win.cw + 2 * borderpx + extra;
	xw.lh = rows * win.ch + 2 * borderpx + extra;
	updatesize();
	assert(win.w == xw.lw && win.h == xw.lh);
	assert(term.col == cols && term.row == rows);
	assert(!ioctl(ptym, TIOCGWINSZ, &ws));
	assert(ws.ws_col == cols && ws.ws_row == rows);
	assert(ws.ws_xpixel == cols * win.cw && ws.ws_ypixel == rows * win.ch);
}

static void resizeframe(int step)
{
	unsigned char *got;
	ShmBuffer *b;
	unsigned int before = commits;
	size_t size;
	nreported = 0;
	draw();
	if (commits != before + 1 || presentpending || curbuf)
		fprintf(stderr, "resize step %d: commits %u -> %u pending %d\n",
			step, before, commits, presentpending);
	assert(commits == before + 1 && !presentpending && !curbuf);
	b = lastbuf;
	assert(b && b->width == win.w && b->height == win.h);
	size = b->size;
	got = xmalloc(size);
	memcpy(got, b->data, size);
	/* Every resized frame is fully damaged, including the border pixels. */
	for (int y = 0; y < win.h; y++) for (int x = 0; x < win.w; x++)
		assert(inreported(x, y));
	/* Compare the incremental result with a repaint of the same terminal
	 * cells; this distinguishes a stale pixel buffer from old application
	 * content that is still present in the terminal grid. */
	bufferrelease(b, b->buffer);
	tfulldirt(); forcedraw = 1; nreported = 0;
	draw();
	assert(lastbuf == b && !curbuf);
	if (memcmp(got, b->data, size)) {
		fprintf(stderr, "resize step %d: %dx%d differs from a repaint\n",
			step, win.w, win.h);
		abort();
	}
	free(got);
}

#if BOXDRAW_PATCH
/* Geometry stays on complete rows at extreme zoom, including vector shapes
 * which themselves remain confined to their cells at these sizes. */
static void tinygeometry(void)
{
	static const int sizes[][2] = {{1, 2}, {2, 1}, {2, 2}, {3, 2}, {2, 3}};
	int cw = win.cw, ch = win.ch;
	feed("\033[?1049h\033[r\033[?7l\033[?25l\033[m\033[2J");
	ttystarted = 1;
	for (size_t i = 0; i < LEN(sizes); i++) {
		win.cw = sizes[i][0]; win.ch = sizes[i][1];
		resizewindow(COLS, ROWS, 0);
		for (int x = 1; x <= COLS; x++)
			geometrycell(3, x, 0x256d, "");
		for (int b = 0; b < NBUFFERS; b++) buffers[b].busy = 0;
		resizeframe(700 + i);
		geometrycell(3, 12, 0x2573, "31");
		step = -1610 - i; scrollframe(0, 1);
		assert(checked_row_damage[2] == win.w);
		assert(checked_box_calls == COLS);
	}
	win.cw = cw; win.ch = ch;
	resizewindow(COLS, ROWS, 0);
	for (int b = 0; b < NBUFFERS; b++) buffers[b].busy = 0;
	resizeframe(710);
	ttystarted = 0;
}
#endif

static void resizing(void)
{
	if (tisaltscr()) feed("\033[?1049l");
	kscrolldown(&(Arg){.i = HISTSIZE});
	feed("\033[?1049h\033[?25l\033[2J\033[r");
	win.mode = MODE_VISIBLE | MODE_FOCUSED | MODE_HIDE;
	ttystarted = 1;
	for (int i = 0; i < NBUFFERS; i++) buffers[i].busy = 0;
	for (int i = 0; i < 600; i++) {
		int cols = i % 2 ? COLS : 2 * COLS;
		int rows = i % 3 ? ROWS : 2 * ROWS;
		int extra = i % 4 ? 0 : MIN(win.cw, win.ch) / 2;
		unsigned int before;
		/* Leave a narrow hint near the old right edge before changing the
		 * allocation and width. It must not survive as an in-range hint for
		 * the new view, nor discard newly exposed cells on the way back. */
		feed("\033[2;%dHZ\033[%d;%dH", term.col - 1, term.row, term.col);
		/* Coalesced size events, with scrolling pending before the resize. */
		if (i % 5 == 0) {
			feed("\033[H\033[S");
			resizewindow(COLS + 3, ROWS + 2, 0);
			resizewindow(COLS + 1, ROWS + 1, 0);
		}
		resizewindow(cols, rows, 0);
		/* Changing only pixel dimensions must also repaint moved borders. */
		if (extra) resizewindow(cols, rows, extra);
		assert(!term.nscrolls);
		for (int y = 0; y < term.row; y++) assert(term.dirty[y]);
		/* Periodic application output; some frames deliberately contain only
		 * old application text so that a resize itself is also exercised. */
		if (i % 4) {
			feed("\033[2J");
			for (int y = 0; y < rows; y++)
				feed("\033[%d;1H\033[%dm%02d %dx%d -> != \xe4\xb8\xad\033[m\033[%d;%dHX",
					y + 1, 31 + y % 6, y, cols, rows, y + 1, cols);
		}
		/* Force buffer exhaustion with all three previous frames in flight.
		 * The dirty rows must survive until a release makes progress possible. */
		before = commits; nreported = 0;
		if (i >= NBUFFERS) {
			for (int j = 0; j < NBUFFERS; j++) buffers[j].busy = 1;
			draw();
			assert(commits == before && presentpending);
			for (int y = 0; y < term.row; y++) assert(term.dirty[y]);
			bufferrelease(&buffers[i % NBUFFERS], buffers[i % NBUFFERS].buffer);
		}
		resizeframe(i);
	}
	for (int i = 0; i < NBUFFERS; i++) buffers[i].busy = 0;
	resizewindow(COLS + 1, ROWS, 0);
	resizewindow(COLS, ROWS, 0);
	resizeframe(600);
	ttystarted = 0;
}

/* Several real configure handlers may run during callback blackout while old
 * buffers remain held. The last configured size wins once one buffer is free. */
static void deferredresize(void)
{
	unsigned int before;
	struct winsize ws;

	feed("\033[?1049h\033[?25l\033[2J\033[r");
	win.mode = MODE_VISIBLE | MODE_FOCUSED | MODE_HIDE;
	for (int i = 0; i < NBUFFERS; i++) buffers[i].busy = 0;
	fake_clock = 1; fake_now = (struct timespec){30000, 0};
	misscallback(); before = commits; ttystarted = 1;
	for (int i = 0; i < NBUFFERS; i++) buffers[i].busy = 1;
	for (int i = 0; i < 8; i++) {
		int cols = i % 2 ? COLS : 2 * COLS;
		int rows = i % 3 ? ROWS : 2 * ROWS;
		pendingw = cols * win.cw + 2 * borderpx;
		pendingh = rows * win.ch + 2 * borderpx;
		xdgsurface_configure(NULL, NULL, i + 1);
		assert(framewait(&fake_now) < 0);
		assert(term.col == cols && term.row == rows);
		assert(!ioctl(ptym, TIOCGWINSZ, &ws));
		assert(ws.ws_col == cols && ws.ws_row == rows);
		feed("\033[2;%dH%c", cols, 'A' + i);
		draw(); assert(commits == before && presentpending);
		for (int y = 0; y < term.row; y++) assert(term.dirty[y]);
	}
	bufferrelease(lastbuf, lastbuf->buffer);
	fake_clock = 0;
	resizeframe(800);
	assert(term.col == COLS && term.row == ROWS);
	ttystarted = 0;
}

/* Real grid changes update PTY dimensions immediately, but allocator scans
 * wait for a quiet resize interval. Same-size events cannot postpone it. */
static void resizereclamation(void)
{
	unsigned int before = allocator_trims;
	ShmBuffer *saved;

	assert(!curbuf);
	if (framecb) frame_done(NULL, framecb, 0);
	fake_clock = 1; fake_now = (struct timespec){40000, 0};
	resizetrim.pending = 0; ttystarted = 1;
	resizewindow(COLS, ROWS, 0);
	assert(trimresize(&fake_now) < 0 && !resizetrim.pending);
	resizewindow(COLS + 1, ROWS, 0);
	assert(allocator_trims == before && trimresize(&fake_now) == 500);
	tick(200); resizewindow(COLS + 2, ROWS, 0);
	assert(trimresize(&fake_now) == 500);
	tick(300); resizewindow(COLS + 2, ROWS, 0);
	assert(trimresize(&fake_now) == 200);
	resizewindow(COLS + 2, ROWS, 1);
	assert(trimresize(&fake_now) == 200);
	tick(199);
	assert(trimresize(&fake_now) == 1 && allocator_trims == before);
	/* An active frame retains its data even if a deadline is overdue. */
	saved = curbuf; curbuf = lastbuf;
	tick(1); assert(trimresize(&fake_now) < 0 && resizetrim.pending);
	assert(allocator_trims == before); curbuf = saved;
	assert(trimresize(&fake_now) < 0 && !resizetrim.pending);
	assert(allocator_trims == ++before);
	tick(5000);
	assert(trimresize(&fake_now) < 0 && allocator_trims == before);
	/* A continuous burst still gets bounded reclamation, then no periodic
	 * timer remains once changes stop. */
	resizewindow(COLS, ROWS, 0);
	for (int i = 0; i < 4; i++) {
		tick(400); resizewindow(COLS + 1 + i % 2, ROWS, 0);
		assert(trimresize(&fake_now) > 0 && allocator_trims == before);
	}
	tick(400); resizewindow(COLS, ROWS, 0);
	assert(trimresize(&fake_now) < 0 && allocator_trims == ++before);
	tick(10000);
	assert(trimresize(&fake_now) < 0 && allocator_trims == before);
	fake_clock = 0;
	for (int i = 0; i < NBUFFERS; i++) buffers[i].busy = 0;
	resizeframe(801);
	ttystarted = 0;
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
	deferredupdates();
	partialupdates();
	dirtyupdates();
	#if BOXDRAW_PATCH
	mixedupdates();
	geometryupdates();
	#if LIGATURES_PATCH && WIDE_GLYPHS_PATCH
	geometryclip();
	#endif
	#endif
	#if LIGATURES_PATCH && WIDE_GLYPHS_PATCH
	distantink();
	#endif

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
	#if BOXDRAW_PATCH
	tinygeometry();
	#endif
	for (int i = 0; i < 3; i++) free(shots[i]);
	resizing();
	deferredresize();
	resizereclamation();
	close(ptym); close(ptys);
}
#endif

int main(void)
{
	const char *testfont = getenv("RENDER_STATE_FONT");
	assert(FcInit()); xw.scale = 1;
	xloadfonts(testfont && *testfont ? testfont : "DejaVu Sans Mono:pixelsize=16", 0); xloadcols();
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
		b->pix = pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8,
			b->width, b->height, b->data, b->stride);
		assert(b->pix);
		b->cr = vec_create(b->pix);
		assert(b->cr);
		b->buffer = (struct wl_buffer *)(uintptr_t)(i + 1);
	}
	sparse(); regions(); copying();
	hiddencolorglyph();
	cursorrestoration(); elision(); framepacing(); trimming();
	#if LIGATURES_PATCH
	idlecache();
	#endif
	#if REFLOW_PATCH
	scrolling();
	#endif
	for (int i = 0; i < NBUFFERS; i++) { buffers[i].buffer = NULL; destroybuffer(&buffers[i]); }
	free(xw.specbuf);
	#if LIGATURES_PATCH
	free(xw.specseq); free(restoredrows);
	#endif
	xunloadfonts(); free(dc.col);
	fcft_fini();
	FcFini();
	puts("PASS: sparse protocol damage, bounded regions, clipping, stale-buffer rotation, "
	     "dirty-row copies, shortage, cursor pixels, restored-line reuse, frame elision, "
	     "idle buffer release, bounded missing-callback fallback and immediate recovery"
	     #if LIGATURES_PATCH
	     ", one-shot cold-cache reclamation and resume"
	     #endif
	     #if REFLOW_PATCH
	     ", partial text/geometry updates, deferred PTY changes, scrolled pixels, rapid alternate-screen resizing and bounded resize reclamation"
	     #endif
	);
	return 0;
}
