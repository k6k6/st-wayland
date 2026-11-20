/* Real SIXEL parsing, borrowed native pixels, filtered resize and composition. */
#define main st_program_main
#include "../wl.c"
#undef main
#include <assert.h>

#if SIXEL_PATCH
static uint32_t pixel(pixman_image_t *image, int x, int y)
{
	return *(uint32_t *)((unsigned char *)pixman_image_get_data(image) +
		y * pixman_image_get_stride(image) + x * 4);
}

static ImageList *parse(int transparent)
{
	sixel_state_t state = {0};
	ImageList *list;
	const unsigned char input[] = "\"1;1;12;12#1;2;100;0;0!6~-#2;2;0;100;0!12C\033\\";
	assert(sixel_parser_init(&state, transparent, 0xffffffff,
	                        transparent ? 0 : 0xff224466, 1, 4, 4) == 0);
	assert(sixel_parser_parse(&state, input, sizeof input - 1) > 0);
	assert(sixel_parser_finalize(&state, &list, 0, 0, 4, 4) == 3);
	sixel_parser_deinit(&state);
	return list;
}

static void images(int transparent)
{
	term.images = parse(transparent);
	for (ImageList *im = term.images; im; im = im->next) {
		unsigned char *original = im->pixels;
		size_t bytes = (size_t)im->width * im->height * 4;
		unsigned char *saved = xmalloc(bytes); memcpy(saved, original, bytes);
		for (int cycle = 0; cycle < 18; cycle++) {
			int w = cycle % 3 == 1 ? im->width * 3 / 2 : cycle % 3 == 2 ? im->width / 2 : im->width;
			int h = cycle % 3 == 1 ? im->height * 3 / 2 : cycle % 3 == 2 ? im->height / 2 : im->height;
			im->pixmap = sixelsurface(im, w, h); assert(im->pixmap);
			assert(pixman_image_get_width(im->pixmap) == w && pixman_image_get_height(im->pixmap) == h);
			if (w == im->width && h == im->height) {
				assert((unsigned char *)pixman_image_get_data(im->pixmap) == original);
				for (int y = 0; y < h; y++) for (int x = 0; x < w; x++)
					assert(pixel(im->pixmap, x, y) == ((uint32_t *)original)[y*w+x]);
			} else {
				assert((unsigned char *)pixman_image_get_data(im->pixmap) != original);
				/* Premultiplied colors remain valid after convolution. */
				for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
					uint32_t p = pixel(im->pixmap,x,y), a = p >> 24;
					assert(((p >> 16) & 255) <= a && ((p >> 8) & 255) <= a && (p & 255) <= a);
				}
			}
			assert(!memcmp(original, saved, bytes));
			pixman_image_unref(im->pixmap); im->pixmap = NULL;
		}
		free(saved);
		im->pixmap = sixelsurface(im, im->width, im->height);
	}
	delete_image(term.images->next);
	while (term.images) delete_image(term.images);
	assert(!term.images);
}

static void downsampling(void)
{
	uint32_t pixels[64*64];
	ImageList im = {.pixels = (unsigned char *)pixels, .width = 64, .height = 64};
	/* One white column in every eight must contribute 1/8 intensity. At this
	 * scale nearest/bilinear sampling hits only black columns and loses it. */
	for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++)
		pixels[y*64+x] = x % 8 == 0 ? 0xffffffff : 0xff000000;
	pixman_image_t *small = sixelsurface(&im, 8, 8); assert(small);
	for (int y = 1; y < 7; y++) for (int x = 1; x < 7; x++) {
		uint32_t p = pixel(small,x,y);
		assert((p>>24) == 255);
		assert((p&255) >= 31 && (p&255) <= 33);
		assert(((p>>8)&255) == (p&255) && ((p>>16)&255) == (p&255));
	}
	pixman_image_unref(small);
	assert(!sixelsurface(&im,0,8));
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
		const uint32_t background = 0xff8899aau;
		for (int y = 0; y < 3; y++) for (int x = 0; x < 4; x++) cells[y][x].mode = ATTR_SIXEL;
		term.images = parse(transparent);
		xw.pix = pixman_image_create_bits(PIXMAN_a8r8g8b8,16,12,NULL,0); assert(xw.pix);
		xw.draw = vec_create(xw.pix); assert(xw.draw);
		for (int i = 0; i < 16*12; i++) pixman_image_get_data(xw.pix)[i] = background;
		/* Erase middle cell in every strip: its pixels must be left alone. */
		for (int y = 0; y < 3; y++) cells[y][1].mode = 0;
		xfinishdraw();
		for (ImageList *im = term.images; im; im = im->next) {
			assert(im->pixmap && (unsigned char *)pixman_image_get_data(im->pixmap) == im->pixels);
			for (int y = 0; y < im->height; y++) for (int x = 0; x < im->width; x++) {
				uint32_t source = ((uint32_t *)im->pixels)[y*im->width+x];
				uint32_t want = x/4 == 1 || (transparent && !source) ? background : source;
				assert(pixel(xw.pix,x,im->y*4+y) == want);
			}
		}
		for (int y = 0; y < 12; y++) for (int x = 12; x < 16; x++) assert(pixel(xw.pix,x,y) == background);
		term.images->y = -1;
		pixman_image_t *cache = term.images->pixmap;
		xfinishdraw(); assert(term.images->pixmap == cache);
		term.images->y = 0;
		for (int y = 0; y < 3; y++) for (int x = 0; x < 4; x++) cells[y][x].mode = 0;
		xfinishdraw(); assert(!term.images);
		vec_destroy(xw.draw); pixman_image_unref(xw.pix);
		xw.draw = NULL; xw.pix = NULL;
	}
}
#endif

int main(void)
{
	#if SIXEL_PATCH
	images(0); images(1); downsampling(); erasure();
	puts("PASS: real SIXEL parsing, exact native pixels, borrowed storage, filtered shrink/enlarge, "
	     "premultiplied alpha, transparent/opaque cell clipping, hidden strips and deletion");
	#else
	puts("PASS: SIXEL disabled build");
	#endif
	return 0;
}
