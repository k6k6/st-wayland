#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <cairo.h>
#include <cairo-ft.h>
#include <hb.h>
#include <hb-ft.h>

#include "st.h"
#include "hb.h"

#define FEATURE(c1,c2,c3,c4) { .tag = HB_TAG(c1,c2,c3,c4), .value = 1, .start = HB_FEATURE_GLOBAL_START, .end = HB_FEATURE_GLOBAL_END }
#define BUFFER_STEP 256

hb_font_t *hbfindfont(cairo_scaled_font_t *match, FT_Face face);

typedef struct {
	cairo_scaled_font_t *match;
	hb_font_t *font;
} HbFontMatch;

typedef struct {
	size_t capacity;
	HbFontMatch *fonts;
} HbFontCache;

static HbFontCache hbfontcache = { 0, NULL };

typedef struct {
	size_t capacity;
	Rune *runes;
} RuneBuffer;

static RuneBuffer hbrunebuffer = { 0, NULL };
static hb_buffer_t *hbbuffer;

/*
 * Shaping cache.  Terminals redraw the same text over and over (scrolling,
 * cursor movement, full-screen dirt), and hb_shape() dominated the frame
 * cost.  Cache the shaping result of every (font, rune sequence) segment in
 * a small 2-way set associative table keyed by a hash of the runes.
 */
#define SHAPECACHE_SIZE 4096	/* must be a power of two */
#define SHAPECACHE_MAXLEN 1024	/* segments longer than this are not cached */
#ifndef SHAPECACHE_MAXBYTES
#define SHAPECACHE_MAXBYTES (8u * 1024u * 1024u)
#endif

typedef struct {
	cairo_scaled_font_t *font;
	uint32_t hash;
	int length;		/* number of input runes */
	unsigned int count;	/* number of output glyphs */
	Rune *runes;
	hb_glyph_info_t *glyphs;
	hb_glyph_position_t *positions;
	size_t bytes;		/* requested allocation bytes, excluding allocator overhead */
	unsigned char recent;	/* second-chance eviction bit */
} ShapeEntry;

static ShapeEntry shapecache[SHAPECACHE_SIZE];
static unsigned int shapetick, shapehand;
static size_t shapebytes;

static void
shapeentry_free(ShapeEntry *e)
{
	shapebytes -= e->bytes;
	free(e->runes);
	free(e->glyphs);
	free(e->positions);
	memset(e, 0, sizeof(*e));
}

static void
shapecache_clear(void)
{
	for (int i = 0; i < SHAPECACHE_SIZE; i++)
		if (shapecache[i].runes)
			shapeentry_free(&shapecache[i]);
	shapehand = shapetick = 0;
}

/* Reclaim before allocating: changing TUI text must not grow the cache
 * to 4096 long segments. A clock sweep gives recently used entries a
 * second chance without maintaining an LRU list on every cache hit. */
static void
shapecache_makeroom(size_t bytes)
{
	while (shapebytes > SHAPECACHE_MAXBYTES - bytes) {
		ShapeEntry *e = &shapecache[shapehand];
		shapehand = (shapehand + 1) & (SHAPECACHE_SIZE - 1);
		if (e->recent)
			e->recent = 0;
		else if (e->runes)
			shapeentry_free(e);
	}
}

static inline uint32_t
shapehash(cairo_scaled_font_t *font, const Rune *runes, int length)
{
	uint32_t h = 2166136261u ^ (uint32_t)(uintptr_t)font;

	h *= 16777619u;
	for (int i = 0; i < length; i++) {
		h ^= runes[i];
		h *= 16777619u;
	}
	return h;
}

/*
 * Poplulate the array with a list of font features, wrapped in FEATURE macro,
 * e. g.
 * FEATURE('c', 'a', 'l', 't'), FEATURE('d', 'l', 'i', 'g')
 */
hb_feature_t features[] = {FEATURE('c', 'v', '0', '2'), FEATURE('c', 'v', '0', '1'),
                           FEATURE('c', 'v', '6', '1')};

void
hbcreatebuffer(void)
{
	hbbuffer = hb_buffer_create();
}

void
hbdestroybuffer(void)
{
	hb_buffer_destroy(hbbuffer);
	shapecache_clear();
}

void
hbunloadfonts(void)
{
	for (int i = 0; i < hbfontcache.capacity; i++)
		hb_font_destroy(hbfontcache.fonts[i].font);

	if (hbfontcache.fonts != NULL) {
		free(hbfontcache.fonts);
		hbfontcache.fonts = NULL;
	}
	hbfontcache.capacity = 0;
	/* the cache is keyed on font pointers which may now be reused */
	shapecache_clear();
}

hb_font_t *
hbfindfont(cairo_scaled_font_t *match, FT_Face face)
{
	for (int i = 0; i < hbfontcache.capacity; i++) {
		if (hbfontcache.fonts[i].match == match)
			return hbfontcache.fonts[i].font;
	}

	/* Font not found in cache, caching it now. */
	hbfontcache.fonts = realloc(hbfontcache.fonts, sizeof(HbFontMatch) * (hbfontcache.capacity + 1));
	/*
	 * Take a reference on the FT_Face for the lifetime of the cached
	 * hb_font_t.  The face belongs to cairo's unscaled font, and cairo
	 * calls FT_Done_Face() on idle faces as soon as MAX_OPEN_FACES (10)
	 * are open, which a full-screen redraw pulling in many fallback
	 * fonts reaches easily.  HarfBuzz reads table data straight out of
	 * the memory mapping FreeType made for that face, so a face dying
	 * under a live hb_font_t leaves every cached table pointer
	 * dangling: once another font file is mapped over that address
	 * range, shaping reads garbage table data and crashes.
	 * hb_ft_font_create() keeps no reference, hb_ft_font_create_referenced()
	 * does and drops it when the hb_font_t is destroyed.
	 */
	hb_font_t *font = hb_ft_font_create_referenced(face);
	if (font == NULL)
		die("Failed to load Harfbuzz font.");

	hbfontcache.fonts[hbfontcache.capacity].match = match;
	hbfontcache.fonts[hbfontcache.capacity].font = font;
	hbfontcache.capacity += 1;

	return font;
}

void
hbtransform(HbTransformData *data, cairo_scaled_font_t *sfont, const Glyph *glyphs, int start, int length)
{
	uint32_t mode, hash;
	unsigned int glyph_count;
	int rune_idx, glyph_idx, end = start + length;
	hb_buffer_t *buffer = hbbuffer;
	FT_Face face;
	hb_font_t *font;
	ShapeEntry *e, *victim;
	Rune *runes;

	/* Resize the buffer if required length is larger. */
	if (hbrunebuffer.capacity < length) {
		hbrunebuffer.capacity = (length / BUFFER_STEP + 1) * BUFFER_STEP;
		hbrunebuffer.runes = realloc(hbrunebuffer.runes, hbrunebuffer.capacity * sizeof(Rune));
	}
	runes = hbrunebuffer.runes;

	/* Fill buffer with codepoints. */
	for (rune_idx = 0, glyph_idx = start; glyph_idx < end; glyph_idx++, rune_idx++) {
		runes[rune_idx] = glyphs[glyph_idx].u;
		mode = glyphs[glyph_idx].mode;
		if (mode & ATTR_WDUMMY)
			runes[rune_idx] = 0x0020;
	}

	/* Look the segment up in the shaping cache (2-way set associative). */
	victim = NULL;
	if (length > 0 && length <= SHAPECACHE_MAXLEN) {
		hash = shapehash(sfont, runes, length);
		for (int way = 0; way < 2; way++) {
			e = &shapecache[(hash + way) & (SHAPECACHE_SIZE - 1)];
			if (e->runes && e->font == sfont && e->hash == hash &&
			    e->length == length &&
			    memcmp(e->runes, runes, length * sizeof(Rune)) == 0) {
				e->recent = 1;
				data->buffer = NULL;
				data->glyphs = e->glyphs;
				data->positions = e->positions;
				data->count = e->count;
				return;
			}
			/* prefer an empty slot, otherwise evict pseudo-randomly */
			if (!victim || !e->runes ||
			    (victim->runes && (shapetick & 1) == way))
				victim = e;
		}
		shapetick++;
	}

	/*
	 * The FT_Face is shared with cairo; lock it so that it is scaled for
	 * this scaled font while harfbuzz is using it, and unlock it before
	 * cairo touches the font again.
	 */
	face = cairo_ft_scaled_font_lock_face(sfont);
	if (face == NULL) {
		data->count = 0;
		return;
	}

	font = hbfindfont(sfont, face);
	if (font == NULL) {
		cairo_ft_scaled_font_unlock_face(sfont);
		data->count = 0;
		return;
	}

	hb_buffer_reset(buffer);
	hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
	hb_buffer_set_cluster_level(buffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);
	hb_buffer_add_codepoints(buffer, runes, length, 0, length);

	/* Shape the segment. */
	hb_shape(font, buffer, features, sizeof(features)/sizeof(hb_feature_t));

	cairo_ft_scaled_font_unlock_face(sfont);

	/* Get new glyph info. */
	hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buffer, &glyph_count);
	hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buffer, &glyph_count);

	/* Account for all three allocations, including the zero-glyph guards.
	 * Oversized entries are shaped normally but not retained. */
	size_t bytes = (size_t)length * sizeof(Rune) +
	               (size_t)glyph_count * (sizeof(hb_glyph_info_t) +
	                                      sizeof(hb_glyph_position_t)) + 2;
	if (victim && bytes <= SHAPECACHE_MAXBYTES) {
		if (victim->runes)
			shapeentry_free(victim);
		shapecache_makeroom(bytes);
		victim->runes = malloc(length * sizeof(Rune));
		victim->glyphs = malloc(glyph_count * sizeof(hb_glyph_info_t) + 1);
		victim->positions = malloc(glyph_count * sizeof(hb_glyph_position_t) + 1);
		if (victim->runes && victim->glyphs && victim->positions) {
			memcpy(victim->runes, runes, length * sizeof(Rune));
			memcpy(victim->glyphs, info, glyph_count * sizeof(hb_glyph_info_t));
			memcpy(victim->positions, pos, glyph_count * sizeof(hb_glyph_position_t));
			victim->font = sfont;
			victim->hash = hash;
			victim->length = length;
			victim->count = glyph_count;
			victim->bytes = bytes;
			victim->recent = 1;
			shapebytes += bytes;
		} else {
			shapeentry_free(victim);
		}
	}

	/* Fill the output. */
	data->buffer = buffer;
	data->glyphs = info;
	data->positions = pos;
	data->count = glyph_count;
}
