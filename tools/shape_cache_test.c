/* Headless regression tests of the production HarfBuzz shaping cache. */
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>

static int fail_allocation;
static void *
test_malloc(size_t size)
{
	if (fail_allocation && --fail_allocation == 0)
		return NULL;
	return malloc(size);
}

#define malloc test_malloc
#include "../hb.c"
#undef malloc

void
die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	exit(1);
}

static size_t
checkaccounting(void)
{
	size_t total = 0;
	for (int i = 0; i < SHAPECACHE_SIZE; i++) {
		ShapeEntry *e = &shapecache[i];
		if (!e->runes) {
			assert(!e->bytes && !e->glyphs && !e->positions);
			continue;
		}
		assert(e->bytes == (size_t)e->length * sizeof(Rune) +
		       (size_t)e->count * (sizeof(hb_glyph_info_t) +
		                           sizeof(hb_glyph_position_t)) + 2);
		total += e->bytes;
	}
	assert(total == shapebytes);
	assert(total <= SHAPECACHE_MAXBYTES);
	return total;
}

static void
fill(Glyph *glyphs, int length, unsigned seed)
{
	for (int i = 0; i < length; i++) {
		seed = seed * 1664525u + 1013904223u;
		glyphs[i] = (Glyph){.u = 'a' + ((seed >> 16) % 26)};
	}
}

int
main(void)
{
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *cr = cairo_create(surface);
	cairo_select_font_face(cr, "DejaVu Sans Mono", CAIRO_FONT_SLANT_NORMAL,
	                      CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 14);
	cairo_scaled_font_t *font = cairo_get_scaled_font(cr);
	assert(cairo_scaled_font_status(font) == CAIRO_STATUS_SUCCESS);
	Glyph glyphs[SHAPECACHE_MAXLEN + 1];
	HbTransformData cold, hot;
	size_t peak = 0;
	hbcreatebuffer();

	/* Cache hits preserve both glyph identities and exact positioning. */
	fill(glyphs, 8, 1);
	hbtransform(&cold, font, glyphs, 0, 8);
	assert(cold.buffer != NULL);
	hbtransform(&hot, font, glyphs, 0, 8);
	assert(hot.buffer == NULL && cold.count == hot.count);
	assert(!memcmp(cold.glyphs, hot.glyphs, hot.count * sizeof(*hot.glyphs)));
	assert(!memcmp(cold.positions, hot.positions, hot.count * sizeof(*hot.positions)));
	checkaccounting();
	shapecache_clear();
	assert(checkaccounting() == 0);

	/* Any of the three allocation failures leaves no partial entry/accounting. */
	for (int n = 1; n <= 3; n++) {
		fail_allocation = n;
		hbtransform(&cold, font, glyphs, 0, 8);
		assert(cold.count == 8 && cold.buffer != NULL);
		assert(checkaccounting() == 0);
	}

	/* Long, changing segments exceed the byte budget well before the slot cap.
	 * Interleaved short/long lengths also exercise full/half-width workloads. */
	for (unsigned i = 0; i < 6000; i++) {
		int length = i % 3 == 0 ? 120 : SHAPECACHE_MAXLEN;
		fill(glyphs, length, i + 2);
		hbtransform(&cold, font, glyphs, 0, length);
		assert(cold.count == (unsigned)length);
		size_t bytes = checkaccounting();
		if (bytes > peak) peak = bytes;
		/* Immediate second lookup must hit unless this entry exceeds budget. */
		hbtransform(&hot, font, glyphs, 0, length);
		assert(hot.count == cold.count);
		if ((size_t)length * sizeof(Rune) + (size_t)cold.count *
		    (sizeof(hb_glyph_info_t) + sizeof(hb_glyph_position_t)) + 2 <=
		    SHAPECACHE_MAXBYTES) {
			assert(hot.buffer == NULL);
			assert(!memcmp(cold.glyphs, hot.glyphs, hot.count * sizeof(*hot.glyphs)));
			assert(!memcmp(cold.positions, hot.positions, hot.count * sizeof(*hot.positions)));
		} else {
			assert(hot.buffer != NULL);
		}
	}
	if (SHAPECACHE_MAXBYTES >= 65536)
		assert(peak > SHAPECACHE_MAXBYTES - 65536);

	/* Overlength runs bypass the cache; unloading fonts resets accounting. */
	size_t before = shapebytes;
	fill(glyphs, SHAPECACHE_MAXLEN + 1, 42);
	hbtransform(&cold, font, glyphs, 0, SHAPECACHE_MAXLEN + 1);
	assert(cold.buffer != NULL && checkaccounting() == before);
	hbunloadfonts();
	assert(checkaccounting() == 0 && hbfontcache.capacity == 0);
	fill(glyphs, 8, 3);
	hbtransform(&cold, font, glyphs, 0, 8);
	assert(checkaccounting() > 0);
	hbdestroybuffer();
	assert(checkaccounting() == 0);
	hbunloadfonts();
	free(hbrunebuffer.runes);
	cairo_destroy(cr);
	cairo_surface_destroy(surface);
	cairo_debug_reset_static_data();
	FcFini();
	printf("PASS budget=%u peak=%zu: hits, pressure, bypass, allocation failures, teardown\n",
	       (unsigned)SHAPECACHE_MAXBYTES, peak);
	return 0;
}
