/* Exercise real frame accumulation, sparse copying and cursor restoration.
 * Wayland requests are captured; no display server or PTY is needed. */
#define _GNU_SOURCE
#include <wayland-client.h>
static void test_attach(struct wl_surface *, struct wl_buffer *, int32_t, int32_t);
static void test_damage(struct wl_surface *, int32_t, int32_t, int32_t, int32_t);
static struct wl_callback *test_frame(struct wl_surface *);
static int test_listener(struct wl_callback *, const struct wl_callback_listener *, void *);
static void test_destroy_callback(struct wl_callback *);
static void test_commit(struct wl_surface *);
static int test_flush(struct wl_display *);
#define wl_surface_attach test_attach
#define wl_surface_damage_buffer test_damage
#define wl_surface_frame test_frame
#define wl_callback_add_listener test_listener
#define wl_callback_destroy test_destroy_callback
#define wl_surface_commit test_commit
#define wl_display_flush test_flush
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
static unsigned int nreported, commits, callback_id;
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
	lastbuf = src; curbuf = NULL;
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
	curbuf = NULL; lastbuf = b; b->busy = 0;
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
	assert(!line_calls);
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
	curbuf = NULL; lastbuf = &buffers[0]; lastbuf->busy = 0;
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
	sparse(); regions(); copying(); cursorrestoration();
	for (int i = 0; i < NBUFFERS; i++) { buffers[i].buffer = NULL; destroybuffer(&buffers[i]); }
	free(xw.specbuf);
	#if LIGATURES_PATCH
	free(xw.specseq); free(restoredrows); hbdestroybuffer();
	#endif
	xunloadfonts(); free(frc); free(dc.col);
	cairo_debug_reset_static_data(); FcFini();
	puts("PASS: sparse protocol damage, bounded regions, clipping, stale-buffer rotation, "
	     "dirty-row copies, shortage, cursor pixels and restored-line reuse");
	return 0;
}
