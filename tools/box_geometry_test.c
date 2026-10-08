/* Headless tests of production terminal tagging, encoding and Cairo drawing. */
#define main st_program_main
#include "../wl.c"
#undef main
#include <assert.h>
#include "box_geometry_inventory.h"

extern void box_geometry_test_setchar(Rune, const Glyph *);

static void
check_tagging(int enabled)
{
	static Glyph cell;
	static Line line = &cell;
	static int dirty;
	Glyph attr = {.mode = ATTR_BOLD | ATTR_ITALIC};
	term.line = &line;
	term.dirty = &dirty;
	term.col = term.row = 1;
	for (unsigned i = 0; i < LEN(geometry_inventory); i++) {
		cell.mode = 0;
		box_geometry_test_setchar(geometry_inventory[i], &attr);
		assert(cell.u == geometry_inventory[i] && dirty);
		#if BOXDRAW_PATCH
		int expected = enabled && (boxdraw_branch || !isboxbranch(cell.u));
		if (!!isboxdraw(cell.u) != expected) {
			fprintf(stderr, "Missing/incorrect geometry route U+%04X\n", cell.u);
			abort();
		}
		assert(!!(cell.mode & ATTR_BOXDRAW) == expected);
		#else
		assert((cell.mode & attr.mode) == attr.mode);
		(void)enabled;
		#endif
	}
	#if BOXDRAW_PATCH
	for (unsigned i = 0; i < LEN(geometry_font_only); i++)
		assert(!isboxdraw(geometry_font_only[i]));
	const Rune other[] = {'A', 0x4e2d, 0xe6ae, 0xf07b, 0x1f600, 0x1fbf0, 0x1ccff, 0x10ffff};
	for (unsigned i = 0; i < LEN(other); i++) assert(!isboxdraw(other[i]));
	#endif
}

#if BOXDRAW_PATCH
static void
check_specs(void)
{
	Glyph glyphs[LEN(geometry_inventory)];
	GlyphFontSpec specs[LEN(geometry_inventory)];
	ushort seen[LEN(geometry_inventory)];
	int before = frclen;
	win.cw = 9;
	win.ch = 19;
	assert(dc.font.match == NULL && dc.bfont.match == NULL);
	for (unsigned i = 0; i < LEN(glyphs); i++) {
		glyphs[i] = (Glyph){.u = geometry_inventory[i], .mode = ATTR_BOXDRAW};
		seen[i] = boxdrawindex(&glyphs[i]);
		assert(seen[i]);
		if (isboxextra(glyphs[i].u)) {
			assert(seen[i] & BDX);
			assert(boxextrarune(seen[i]) == glyphs[i].u);
			assert(boxextrarune(seen[i] | BDB) == glyphs[i].u);
		}
		for (unsigned j = 0; j < i; j++) assert(seen[i] != seen[j]);
	}
	assert(xmakeglyphfontspecs(specs, glyphs, LEN(glyphs), 0, 0) == LEN(glyphs));
	for (unsigned i = 0; i < LEN(glyphs); i++) {
		assert(specs[i].font == NULL && specs[i].glyph == seen[i]);
		assert(specs[i].x == specs[0].x + i * win.cw);
	}
	/* Supplementary characters, old boxes, and double-width geometry coexist. */
	Glyph wide[] = {{.u=0x1fb3c, .mode=ATTR_BOXDRAW | ATTR_WIDE},
	                {.mode=ATTR_WDUMMY},
	                {.u=0x1cc30, .mode=ATTR_BOXDRAW | ATTR_WIDE},
	                {.mode=ATTR_WDUMMY}};
	assert(xmakeglyphfontspecs(specs, wide, 4, 0, 0) == 2);
	assert(specs[1].x - specs[0].x == 2 * win.cw);
	assert(frclen == before && frc == NULL);
}

#define SW 96
#define SH 80
#define PX 7
#define PY 9
static Color foreground = {.color={.red=65535, .green=65535, .blue=65535, .alpha=65535}};
static Color background = {.color={.red=0x1212, .green=0x1212, .blue=0x1212, .alpha=65535}};

static void
clear_image(void)
{
	wlfillrect(&background, 0, 0, SW, SH);
}

static uint32_t
pixel(int x, int y)
{
	return ((uint32_t *)(cairo_image_surface_get_data(xw.buf) +
	         y * cairo_image_surface_get_stride(xw.buf)))[x];
}

static void
render(Rune u, int x, int y, int w, int h, ushort style)
{
	Glyph g = {.u=u, .mode=ATTR_BOXDRAW | style};
	GlyphFontSpec spec = {.glyph=boxdrawindex(&g)};
	drawboxes(x, y, w, h, &foreground, &background, &spec, 1);
	assert(cairo_status(xw.draw) == CAIRO_STATUS_SUCCESS);
	cairo_surface_flush(xw.buf);
}

static void
check_pixels(void)
{
	const int sizes[][2] = {{1,1}, {2,3}, {7,13}, {8,16}, {9,17}, {13,23}, {24,40}};
	unsigned renders = 0;
	for (unsigned i = 0; i < LEN(geometry_inventory); i++) {
		Rune u = geometry_inventory[i];
		/* Existing rect renderer intentionally uses minimum 3px double lines. */
		if (!isboxextra(u)) continue;
		for (unsigned d = 0; d < LEN(sizes); d++) {
			int w = sizes[d][0], h = sizes[d][1];
			for (int style = 0; style < 2; style++) {
				clear_image();
				render(u, PX, PY, w, h, style ? ATTR_BOLD | ATTR_ITALIC : 0);
				int nonempty = 0;
				for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++) {
					uint32_t p = pixel(x, y);
					if (x < PX || x >= PX+w || y < PY || y >= PY+h)
						assert(p == 0xff121212);
					else nonempty |= p != 0xff121212;
				}
				/* Individual fractional blocks may quantize away in tiny cells. */
				if (w >= 8 && h >= 16 && !nonempty) {
					fprintf(stderr, "Empty geometry U+%04X at %dx%d\n", u, w, h);
					abort();
				}
				renders++;
			}
		}
	}
	printf("%u clipped/styled raster cases; ", renders);
	/* Legacy halves/sextants share the same rounded boundaries as full blocks. */
	clear_image();
	render(0x1fb00, PX, PY, 9, 18, 0); /* top-left sextant */
	assert(pixel(PX, PY) == 0xffffffff);
	assert(pixel(PX+3, PY+4) == 0xffffffff);
	assert(pixel(PX+7, PY+4) == 0xff121212);
	assert(pixel(PX+3, PY+12) == 0xff121212);
	/* Real rounded corners still meet original straight rules at their centers. */
	clear_image();
	render(0x256d, PX, PY, 9, 17, 0);
	assert(pixel(PX+8, PY+8) == 0xffffffff);
	assert(pixel(PX+4, PY+16) == 0xffffffff);
	assert(pixel(PX+4, PY+8) != 0xffffffff);
	/* Heavy dashes use the same three-stem thickness as old heavy rules. */
	uint32_t heavy[40];
	clear_image();
	render(0x2501, PX, PY, 24, 40, 0);
	for (int y = 0; y < 40; y++) heavy[y] = pixel(PX+2, PY+y);
	clear_image();
	render(0x2505, PX, PY, 24, 40, 0);
	for (int y = 0; y < 40; y++) assert(pixel(PX+2, PY+y) == heavy[y]);
	/* Inverse WHITE circle is an unpainted ring, not an empty disk. */
	clear_image();
	render(0x25d9, PX, PY, 24, 40, 0);
	assert(pixel(PX+12, PY+20) == 0xffffffff);
	assert(pixel(PX+23, PY+20) == 0xffffffff);
	int ring = 0;
	for (int x = 13; x < 23; x++) ring |= pixel(PX+x, PY+20) == 0xff121212;
	assert(ring);
}

static void
check_tiling(void)
{
	uint32_t tiled[SW * SH];
	const Rune patterns[] = {0x1fb95, 0x1fb96, 0x1fb97, 0x1fb98, 0x1fb99,
	                         0x1cc40, 0x1cc41, 0x1cc42, 0x1cc43,
	                         0x1cc44, 0x1cc45, 0x1cc46, 0x1cc47};
	for (unsigned i = 0; i < LEN(patterns); i++) {
		clear_image();
		for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++)
			render(patterns[i], PX+x*9, PY+y*17, 9, 17, 0);
		for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++)
			tiled[y*SW+x] = pixel(x,y);
		clear_image();
		render(patterns[i], PX, PY, 18, 34, 0);
		for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++)
			assert(pixel(x,y) == tiled[y*SW+x]);
	}
	/* Sixteen tile characters must form two uninterrupted nested ellipses. */
	clear_image();
	for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
		render(0x1cc30+y*4+x, PX+x*9, PY+y*13, 9, 13, 0);
	for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++)
		tiled[y*SW+x] = pixel(x,y);
	clear_image();
	cairo_save(xw.draw);
	cairo_set_source_rgb(xw.draw, 1, 1, 1);
	cairo_set_antialias(xw.draw, CAIRO_ANTIALIAS_GRAY);
	cairo_set_line_width(xw.draw, 1);
	boxcircledisk(xw.draw, PX+18, PY+26, 17.5, 25.5, 0);
	boxcircledisk(xw.draw, PX+18, PY+26, 8.5, 12.5, 0);
	cairo_restore(xw.draw);
	cairo_surface_flush(xw.buf);
	for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++)
		assert(abs((int)(pixel(x,y)&255) - (int)(tiled[y*SW+x]&255)) <= 1);
	/* Overlapping subpaths form a union, not multiple alpha applications. */
	foreground.color.alpha = 32768;
	clear_image();
	render(0x1fb7d, PX, PY, 16, 32, 0);
	uint32_t p = pixel(PX, PY);
	assert((p >> 24) == 255 && (p & 255) >= 136 && (p & 255) <= 137);
	foreground.color.alpha = 65535;
}

#include "box_geometry_extra_test.h"

static void
check_state(void)
{
	cairo_save(xw.draw);
	cairo_rectangle(xw.draw, 0, 0, 1, 1);
	cairo_clip(xw.draw);
	cairo_set_operator(xw.draw, CAIRO_OPERATOR_CLEAR);
	cairo_set_antialias(xw.draw, CAIRO_ANTIALIAS_NONE);
	cairo_set_source_rgb(xw.draw, .2, .3, .4);
	cairo_pattern_t *source = cairo_pattern_reference(cairo_get_source(xw.draw));
	cairo_set_line_width(xw.draw, 11);
	cairo_move_to(xw.draw, 3, 4);
	cairo_line_to(xw.draw, 13, 14);
	for (unsigned i = 0; i < LEN(geometry_inventory); i++) {
		Rune u = geometry_inventory[i];
		if (!isboxextra(u)) continue;
		clear_image();
		render(u, PX, PY, 16, 32, 0);
		int nonempty = 0;
		for (int y = PY; y < PY+32; y++) for (int x = PX; x < PX+16; x++)
			nonempty |= pixel(x, y) != 0xff121212;
		assert(nonempty);
		assert(cairo_get_operator(xw.draw) == CAIRO_OPERATOR_CLEAR);
		assert(cairo_get_antialias(xw.draw) == CAIRO_ANTIALIAS_NONE);
		assert(cairo_get_source(xw.draw) == source);
		assert(cairo_get_line_width(xw.draw) == 11);
		double x1, y1, x2, y2;
		cairo_clip_extents(xw.draw, &x1, &y1, &x2, &y2);
		assert(x1 == 0 && y1 == 0 && x2 == 1 && y2 == 1);
		cairo_path_extents(xw.draw, &x1, &y1, &x2, &y2);
		assert(x1 == 3 && y1 == 4 && x2 == 13 && y2 == 14);
	}
	cairo_pattern_destroy(source);
	cairo_restore(xw.draw);
	cairo_new_path(xw.draw);
	clear_image();
	drawboxextra(xw.draw, PX, PY, 0, 32, NULL, NULL, boxextraindex(0x1fb00));
	drawboxextra(xw.draw, PX, PY, 16, -1, NULL, NULL, boxextraindex(0x1fb00));
	drawboxextra(xw.draw, PX, PY, 16, 32, NULL, NULL, BDX | 0xf00);
	cairo_surface_flush(xw.buf);
	for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++)
		assert(pixel(x,y) == 0xff121212);
}
#endif

int
main(void)
{
	#if BOXDRAW_PATCH
	boxdraw = 1;
	check_tagging(1); /* Includes the configured branch-off build. */
	boxdraw_branch = 0;
	check_tagging(1);
	boxdraw_branch = 1;
	check_tagging(1);
	boxdraw = 0;
	check_tagging(0);
	boxdraw = 1;
	check_all_routes();
	check_specs();
	xw.buf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, SW, SH);
	xw.draw = cairo_create(xw.buf);
	boxdraw_xinit(xw.draw);
	check_pixels();
	check_tiling();
	check_mosaic_semantics();
	check_branch_semantics();
	check_scaled_alpha();
	check_state();
	cairo_destroy(xw.draw);
	cairo_surface_destroy(xw.buf);
	printf("PASS: %zu geometry scalars, supplementary encoding, font-free specs, state/clip, exclusions\n", LEN(geometry_inventory));
	#else
	check_tagging(0);
	puts("PASS: BOXDRAW_PATCH=0 builds and preserves font routing");
	#endif
	return 0;
}
