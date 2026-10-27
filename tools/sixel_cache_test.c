/* Validate native-size borrowing and scaled rebuilds using real SIXEL input. */
#define main st_program_main
#include "../wl.c"
#undef main
#include <assert.h>

#if SIXEL_PATCH
static cairo_surface_t *reference(const ImageList *im, int w, int h)
{
	cairo_surface_t *src = cairo_image_surface_create_for_data(im->pixels,
		CAIRO_FORMAT_ARGB32, im->width, im->height, im->width * 4);
	cairo_surface_t *dst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_t *cr = cairo_create(dst);
	cairo_scale(cr, (double)w / im->width, (double)h / im->height);
	cairo_set_source_surface(cr, src, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), w == im->width && h == im->height ?
		CAIRO_FILTER_NEAREST : CAIRO_FILTER_GOOD);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE); cairo_paint(cr);
	assert(cairo_status(cr) == CAIRO_STATUS_SUCCESS);
	cairo_destroy(cr); cairo_surface_destroy(src);
	return dst;
}

static void samepixels(cairo_surface_t *a, cairo_surface_t *b)
{
	assert(cairo_image_surface_get_width(a) == cairo_image_surface_get_width(b));
	assert(cairo_image_surface_get_height(a) == cairo_image_surface_get_height(b));
	cairo_surface_flush(a); cairo_surface_flush(b);
	for (int y = 0; y < cairo_image_surface_get_height(a); y++)
		assert(!memcmp(cairo_image_surface_get_data(a) + y * cairo_image_surface_get_stride(a),
		               cairo_image_surface_get_data(b) + y * cairo_image_surface_get_stride(b),
		               cairo_image_surface_get_width(a) * 4));
}

static ImageList *parse(int transparent)
{
	sixel_state_t state = {0};
	ImageList *list;
	/* 12x12 with both painted and unpainted pixels, split into cell-height strips. */
	const unsigned char input[] = "\"1;1;12;12#1;2;100;0;0!6~-#2;2;0;100;0!12C\033\\";
	assert(sixel_parser_init(&state, transparent, 0xffffffff,
	                        transparent ? 0 : 0xff224466, 1, 4, 4) == 0);
	assert(sixel_parser_parse(&state, input, sizeof input - 1) > 0);
	assert(sixel_parser_finalize(&state, &list, 0, 0, 4, 4) == 3);
	sixel_parser_deinit(&state);
	return list;
}

static void surfaces(int transparent)
{
	term.images = parse(transparent);
	for (ImageList *im = term.images; im; im = im->next) {
		unsigned char *original = im->pixels;
		size_t bytes = (size_t)im->width * im->height * 4;
		unsigned char *saved = xmalloc(bytes); memcpy(saved, original, bytes);
		for (int cycle = 0; cycle < 18; cycle++) {
			int w = cycle % 3 == 1 ? im->width * 3 / 2 : im->width;
			int h = cycle % 3 == 1 ? im->height * 3 / 2 : im->height;
			im->pixmap = sixelsurface(im, w, h); assert(im->pixmap);
			cairo_surface_t *ref = reference(im, w, h);
			samepixels(im->pixmap, ref);
			if (w == im->width && h == im->height)
				assert(cairo_image_surface_get_data(im->pixmap) == original);
			else assert(cairo_image_surface_get_data(im->pixmap) != original);
			assert(!memcmp(original, saved, bytes));
			cairo_surface_destroy(ref);
			cairo_surface_destroy(im->pixmap); im->pixmap = NULL;
		}
		free(saved);
		im->pixmap = sixelsurface(im, im->width, im->height);
	}
	/* Deleting first/middle/last must respect list links and wrapper lifetime. */
	delete_image(term.images->next);
	while (term.images) delete_image(term.images);
	assert(!term.images);
}

static void erasure(void)
{
	Glyph cells[3][4] = {0}; Line lines[3] = {cells[0], cells[1], cells[2]};
	term.row = 3; term.col = 4; term.line = lines;
	win.cw = win.ch = 4; win.w = win.tw = 16; win.h = win.th = 12;
	#if ANYSIZE_PATCH
	win.hborderpx = win.vborderpx = 0;
	#else
	borderpx = 0;
	#endif
	for (int transparent = 0; transparent <= 1; transparent++) {
		for (int y = 0; y < 3; y++) for (int x = 0; x < 4; x++)
			cells[y][x].mode = ATTR_SIXEL;
		term.images = parse(transparent);
		xw.buf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 16, 12);
		xw.draw = cairo_create(xw.buf); resetdrawstate();
		/* No curbuf: present() is a no-op, but image clipping/deletion is real. */
		xfinishdraw();
		for (ImageList *im = term.images; im; im = im->next) {
			assert(im->pixmap);
			assert(cairo_image_surface_get_data(im->pixmap) == im->pixels);
		}
		/* History hides a strip without deleting its original or cache. */
		term.images->y = -1;
		void *cache = term.images->pixmap;
		xfinishdraw(); assert(term.images->pixmap == cache);
		term.images->y = 0;
		for (int y = 0; y < 3; y++) for (int x = 0; x < 4; x++) cells[y][x].mode = 0;
		xfinishdraw(); assert(!term.images);
		cairo_destroy(xw.draw); cairo_surface_destroy(xw.buf);
		xw.draw = NULL; xw.buf = NULL;
	}
}
#endif

int main(void)
{
	#if SIXEL_PATCH
	surfaces(0); surfaces(1); erasure();
	puts("PASS: real SIXEL parsing, native pixel sharing, exact scaled/reloaded pixels, "
	     "transparent/opaque, hidden strips, erasure and deletion");
	#else
	puts("PASS: SIXEL disabled build");
	#endif
	return 0;
}
