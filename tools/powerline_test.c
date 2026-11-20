/* Exercises the actual terminal tagging, Wayland spec generation and renderer.
 * No FcInit, font loading, hbcreatebuffer, Wayland connection or display needed. */
#define main st_program_main
#include "../wl.c"
#undef main
#include <assert.h>
#define SPEC_FONT_UNUSED(s) ((s).imageglyph == NULL)

extern void powerline_test_setchar(Rune, const Glyph *);
static const Rune supported[] = {
	0xe0b0, 0xe0b1, 0xe0b2, 0xe0b3, 0xe0b4, 0xe0b5,
	0xe0b6, 0xe0b7, 0xe0b8, 0xe0b9, 0xe0ba, 0xe0bb,
	0xe0bc, 0xe0bd, 0xe0be, 0xe0bf, 0xe0d2, 0xe0d4
};

static int
expected(Rune u)
{
	for (size_t i = 0; i < LEN(supported); i++)
		if (supported[i] == u) return 1;
	return 0;
}

static void
tagging(int enabled)
{
	static Glyph cell;
	static Line line = &cell;
	static int dirty;
	Glyph attr = {.mode = ATTR_BOLD | ATTR_ITALIC};
	term.line = &line;
	term.dirty = &dirty;
	term.col = term.row = 1;
	/* Exhaustive BMP PUA coverage includes Nerd icons E6AE and F07B. */
	for (Rune u = 0xe000; u <= 0xf8ff; u++) {
		cell.mode = 0;
		powerline_test_setchar(u, &attr);
		assert(cell.u == u && dirty);
		assert((cell.mode & (ATTR_BOLD | ATTR_ITALIC)) == attr.mode);
		#if BOXDRAW_PATCH
		/* Branch drawing has its own, disjoint PUA whitelist. */
		int branch = boxdraw_branch && u >= 0xf5d0 && u <= 0xf60d;
		assert(!!(cell.mode & ATTR_BOXDRAW) == (enabled && (expected(u) || branch)));
		assert(!!isboxdraw(u) == (enabled && (expected(u) || branch)));
		assert(!!ispowerline(u) == expected(u));
		#else
		(void)enabled;
		#if REFLOW_PATCH
		assert(cell.mode == (attr.mode | ATTR_SET));
		#else
		assert(cell.mode == attr.mode);
		#endif
		#endif
	}
}

#if BOXDRAW_PATCH
#define SW 128
#define SH 64
#define PX 5
#define PY 7
static Color white = {.color = {.red=65535, .green=65535, .blue=65535, .alpha=65535}};
static Color black = {.color = {.alpha=65535}};
static VecContext *testcr;

static uint32_t
pixel(int x, int y)
{
	return ((uint32_t *)((unsigned char *)pixman_image_get_data(xw.pix) +
		y * pixman_image_get_stride(xw.pix)))[x];
}

static void
clear(uint32_t value)
{
	for (int y = 0; y < SH; y++)
		for (int x = 0; x < SW; x++)
			((uint32_t *)((unsigned char *)pixman_image_get_data(xw.pix) +
				y * pixman_image_get_stride(xw.pix)))[x] = value;
}

static void
shape(Rune u, int w, int h, ushort mode, Color *fg)
{
	Glyph g = {.u=u, .mode=ATTR_BOXDRAW | mode};
	GlyphFontSpec spec = {.glyph=boxdrawindex(&g)};
	drawboxes(PX, PY, w, h, fg, &black, &spec, 1);
	assert(vec_status(testcr) == 0);
}

static void
specs(void)
{
	Glyph g[32] = {0};
	GlyphFontSpec s[32] = {0};
	win.cw = 9; win.ch = 17;
	assert(dc.font.match == NULL && dc.bfont.match == NULL && dc.ifont.match == NULL);
	for (size_t i = 0; i < LEN(supported); i++)
		g[i] = (Glyph){.u=supported[i], .mode=ATTR_BOXDRAW};
	assert(xmakeglyphfontspecs(s, g, LEN(supported), 2, 3) == LEN(supported));
	for (size_t i = 0; i < LEN(supported); i++) {
		assert(SPEC_FONT_UNUSED(s[i]) && s[i].glyph == (BDP | (supported[i] & 255)));
		assert(s[i].x == s[0].x + i * win.cw && s[i].y == s[0].y);
	}
	for (int style = 0; style < 4; style++) {
		ushort mode = ATTR_BOXDRAW | (style & 1 ? ATTR_BOLD : 0) | (style & 2 ? ATTR_ITALIC : 0);
		g[0] = (Glyph){.u=0x2588, .mode=mode};
		g[1] = (Glyph){.u=0xe0b0, .mode=mode | ATTR_WIDE};
		g[2] = (Glyph){.u=0, .mode=ATTR_WDUMMY};
		g[3] = (Glyph){.u=0xe0b2, .mode=mode};
		g[4] = (Glyph){.u=0x2588, .mode=mode};
		assert(xmakeglyphfontspecs(s, g, 5, 0, 0) == 4);
		const int indices[] = {0, 1, 3, 4};
		for (int i = 0; i < 4; i++) {
			assert(SPEC_FONT_UNUSED(s[i]) && s[i].glyph == boxdrawindex(&g[indices[i]]));
			assert(s[i].x == s[0].x + indices[i] * win.cw);
		}
	}
}

static void
pixels(void)
{
	const int sizes[][2] = {{1,1}, {1,3}, {2,2}, {3,5}, {7,13}, {8,16}, {9,17}};
	uint32_t saved[SW * SH];
	for (size_t d = 0; d < LEN(sizes); d++) {
		int w = sizes[d][0], h = sizes[d][1];
		for (size_t i = 0; i <= LEN(supported); i++) {
			Rune u = i == LEN(supported) ? 0x2588 : supported[i];
			clear(0); shape(u, w, h, 0, &white);
			int nonempty = 0;
			for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++) {
				uint32_t p = saved[y * SW + x] = pixel(x, y);
				if (x < PX || x >= PX+w || y < PY || y >= PY+h) assert(p == 0);
				else { nonempty |= p != 0; if (u == 0x2588) assert(p == 0xffffffff); }
			}
			assert(nonempty);
			clear(0); shape(u, w, h, ATTR_BOLD | ATTR_ITALIC, &white);
			for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++)
				assert(pixel(x, y) == saved[y * SW + x]);
		}
	}
	/* Interior edge samples avoid antialiased slope/corner coverage. */
	const Rune right[] = {0xe0b0, 0xe0b4}, left[] = {0xe0b2, 0xe0b6};
	for (int i = 0; i < 2; i++) {
		clear(0); shape(right[i], 16, 24, 0, &white);
		for (int y = 3; y < 21; y++) assert(pixel(PX, PY+y) == 0xffffffff);
		assert(pixel(PX+15, PY+2) == 0);
		clear(0); shape(left[i], 16, 24, 0, &white);
		for (int y = 3; y < 21; y++) assert(pixel(PX+15, PY+y) == 0xffffffff);
		assert(pixel(PX, PY+2) == 0);
	}
	const Rune triangles[] = {0xe0b8, 0xe0ba, 0xe0bc, 0xe0be};
	const int solid[][2] = {{2,20}, {13,20}, {2,3}, {13,3}};
	for (int i = 0; i < 4; i++) {
		clear(0); shape(triangles[i], 16, 24, 0, &white);
		assert(pixel(PX+solid[i][0], PY+solid[i][1]) == 0xffffffff);
		assert(pixel(PX+15-solid[i][0], PY+23-solid[i][1]) == 0);
	}
	/* The real drawboxes loop must advance between mixed block/triangle cells. */
	Glyph a[] = {{.u=0x2588}, {.u=0xe0b0}, {.u=0xe0b2}, {.u=0x2588}};
	GlyphFontSpec s[4];
	for (int i = 0; i < 4; i++) s[i].glyph = boxdrawindex(&a[i]);
	clear(0);
	drawboxes(PX, PY, 16, 24, &white, &black, s, 4);
	assert(pixel(PX+15, PY+12) == 0xffffffff && pixel(PX+16, PY+12) == 0xffffffff);
	assert(pixel(PX+47, PY+12) == 0xffffffff && pixel(PX+48, PY+12) == 0xffffffff);
}

static void
state(void)
{
	double dash[] = {2, 3}, out[2], offset, x1, y1, x2, x2b, y2;
	vec_save(testcr);
	vec_rectangle(testcr, 0, 0, 1, 1); vec_clip(testcr); /* stale disjoint text clip */
	vec_set_operator(testcr, VEC_OPERATOR_CLEAR);
	vec_set_antialias(testcr, VEC_ANTIALIAS_NONE);
	vec_set_line_width(testcr, 7);
	vec_set_line_cap(testcr, VEC_LINE_CAP_ROUND);
	vec_set_line_join(testcr, VEC_LINE_JOIN_BEVEL);
	vec_set_miter_limit(testcr, 4);
	vec_set_fill_rule(testcr, VEC_FILL_RULE_EVEN_ODD);
	vec_set_dash(testcr, dash, 2, 1);
	vec_set_source_rgb(testcr, .2, .3, .4);
	VecPattern *source = vec_pattern_reference(vec_get_source(testcr));
	vec_move_to(testcr, 2, 3); vec_line_to(testcr, 11, 12);
	VecPath *path = vec_copy_path(testcr);
	clear(0xff0000ff);
	Color red = {.color={.red=65535, .alpha=32768}};
	shape(0xe0b0, 16, 24, 0, &red);
	uint32_t p = pixel(PX, PY+12);
	assert((p >> 24) == 255 && ((p >> 16) & 255) >= 127 && ((p >> 16) & 255) <= 128);
	assert((p & 255) >= 127 && (p & 255) <= 128 && (p & 0xff00) == 0);
	assert(pixel(PX+15, PY) == 0xff0000ff);
	assert(vec_get_operator(testcr) == VEC_OPERATOR_CLEAR);
	assert(vec_get_antialias(testcr) == VEC_ANTIALIAS_NONE);
	assert(vec_get_line_width(testcr) == 7 && vec_get_miter_limit(testcr) == 4);
	assert(vec_get_line_cap(testcr) == VEC_LINE_CAP_ROUND);
	assert(vec_get_line_join(testcr) == VEC_LINE_JOIN_BEVEL);
	assert(vec_get_fill_rule(testcr) == VEC_FILL_RULE_EVEN_ODD);
	assert(vec_get_source(testcr) == source);
	assert(vec_get_dash_count(testcr) == 2);
	vec_get_dash(testcr, out, &offset);
	assert(out[0] == 2 && out[1] == 3 && offset == 1);
	vec_clip_extents(testcr, &x1, &y1, &x2, &y2);
	assert(x1 == 0 && y1 == 0 && x2 == 1 && y2 == 1);
	VecPath *after = vec_copy_path(testcr);
	assert(vec_path_equal(path, after));
	VecMatrix m; vec_get_matrix(testcr, &m);
	assert(m.xx == 1 && m.yy == 1); /* caller's identity CTM */
	assert(m.xy == 0 && m.yx == 0 && m.x0 == 0 && m.y0 == 0);
	clear(0);
	shape(0xe0b0, 0, 24, 0, &white); shape(0xe0b0, 16, -1, 0, 0);
	drawpowerline(testcr, PX, PY, 16, 24, NULL, 0xe6ae);
	for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++) assert(pixel(x,y) == 0);
	vec_clip_extents(testcr, &x1, &y1, &x2b, &y2); assert(x2b == x2);
	vec_path_destroy(path); vec_path_destroy(after); vec_pattern_destroy(source);
	vec_restore(testcr); vec_new_path(testcr);
}
#endif

int
main(void)
{
	#if BOXDRAW_PATCH
	boxdraw = 1; tagging(1);
	boxdraw = 0; tagging(0);
	boxdraw = 1;
	specs();
	xw.pix = pixman_image_create_bits(PIXMAN_a8r8g8b8, SW, SH, NULL, 0);
	assert(xw.pix);
	testcr = vec_create(xw.pix); boxdraw_xinit(testcr);
	pixels(); state();
	vec_destroy(testcr);
	pixman_image_unref(xw.pix); xw.pix = NULL;
	puts("PASS: exact PUA whitelist, runtime gate, tsetchar, font/HB-free specs, mixed/wide cells, pixels, styling, clip/state/alpha/no-op");
	#else
	tagging(0);
	puts("PASS: BOXDRAW_PATCH=0 builds and leaves all PUA characters untagged");
	#endif
	return 0;
}
