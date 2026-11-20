/*
 * fcft text runs, included by wl.c. fcft owns character glyphs; text-run
 * glyphs are temporary, so only changed bitmaps are copied and deduplicated.
 * A result's metadata may be reused by the next lookup. Glyphs referenced by
 * emitted specs remain valid until textrun_endframe(). Clear before destroying
 * any font, and only after all pending specs have been drawn.
 */
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcft/fcft.h>

#ifndef TEXTRUN_SLOTS
#define TEXTRUN_SLOTS 4096u
#endif
#ifndef TEXTRUN_MAXBYTES
#define TEXTRUN_MAXBYTES (8u * 1024u * 1024u)
#endif
#define TEXTRUN_MAXLEN 1024
#if TEXTRUN_SLOTS < 2 || (TEXTRUN_SLOTS & (TEXTRUN_SLOTS - 1))
#error TEXTRUN_SLOTS must be a power of two, at least two
#endif
#ifndef TEXTRUN_MALLOC
#define TEXTRUN_MALLOC malloc
#endif
#ifndef TEXTRUN_RASTERIZE
#define TEXTRUN_RASTERIZE(font, cp) \
	fcft_rasterize_char_utf32((font), (cp), FCFT_SUBPIXEL_DEFAULT)
#endif
#ifndef TEXTRUN_SHAPE
#define TEXTRUN_SHAPE(font, len, runes) \
	fcft_rasterize_text_run_utf32((font), (len), (runes), FCFT_SUBPIXEL_DEFAULT)
#endif
#ifndef TEXTRUN_TRACK
#define TEXTRUN_TRACK(font, cp) ((void)0)
#endif

typedef struct TextrunBitmap {
	struct fcft_glyph glyph;
	struct TextrunBitmap *next, *retirednext;
	uint32_t hash;
	size_t bytes;
	unsigned int refs;
	int retired;
} TextrunBitmap;

typedef struct {
	uintptr_t ref; /* low bit tags a TextrunBitmap; otherwise an fcft glyph */
	int cluster, dx, dy;
} TextrunGlyph;

typedef struct {
	int count;
	TextrunGlyph *glyphs;
} TextRun;

typedef struct {
	TextRun run;
	struct fcft_font *font;
	uint32_t *runes, hash;
	int length;
	size_t bytes;
	uint32_t lastused;
	unsigned char recent;
} TextrunEntry;

typedef struct TextrunTransient {
	TextRun run;
	struct fcft_text_run *source;
	struct TextrunTransient *next;
} TextrunTransient;

static TextrunEntry textrun_entries[TEXTRUN_SLOTS];
static TextrunBitmap *textrun_images[TEXTRUN_SLOTS], *textrun_retired;
static TextrunTransient *textrun_transients;
static size_t textrun_bytes, textrun_imagebytes;
static unsigned int textrun_hand, textrun_tick;
/* Approximate use order, independent of wall time. Wraparound only changes
 * eviction priority for entries surviving a full 2^32 lookups. */
static uint32_t textrun_serial;
static int textrun_activity;

static const struct fcft_glyph *
textrun_glyph(uintptr_t ref)
{
	return ref & 1 ? &((TextrunBitmap *)(ref & ~(uintptr_t)1))->glyph :
	                (const struct fcft_glyph *)ref;
}

static void
textrun_unref(uintptr_t ref)
{
	TextrunBitmap *bitmap;

	if (!(ref & 1))
		return;
	bitmap = (TextrunBitmap *)(ref & ~(uintptr_t)1);
	if (--bitmap->refs == 0 && !bitmap->retired) {
		bitmap->retired = 1;
		bitmap->retirednext = textrun_retired;
		textrun_retired = bitmap;
	}
}

static void
textrun_freeentry(TextrunEntry *entry)
{
	int i;

	for (i = 0; i < entry->run.count; i++)
		textrun_unref(entry->run.glyphs[i].ref);
	textrun_bytes -= entry->bytes;
	free(entry->runes);
	free(entry->run.glyphs);
	memset(entry, 0, sizeof(*entry));
}

/* Called only after every spec using a retired bitmap has been consumed. */
static void
textrun_collect(void)
{
	TextrunBitmap *bitmap, **link;
	TextrunTransient *transient;

	while ((transient = textrun_transients)) {
		textrun_transients = transient->next;
		fcft_text_run_destroy(transient->source);
		free(transient->run.glyphs);
		free(transient);
	}
	while ((bitmap = textrun_retired)) {
		textrun_retired = bitmap->retirednext;
		bitmap->retired = 0;
		if (bitmap->refs)
			continue; /* a subsequent lookup reused it before this boundary */
		link = &textrun_images[bitmap->hash & (TEXTRUN_SLOTS - 1)];
		while (*link != bitmap)
			link = &(*link)->next;
		*link = bitmap->next;
		textrun_imagebytes -= bitmap->bytes;
		if (bitmap->glyph.pix)
			pixman_image_unref(bitmap->glyph.pix);
		free(bitmap);
	}
}

static void
textrun_endframe(void)
{
	TextrunEntry *entry;

	textrun_collect();
	/* Bitmaps retired during a frame are intentionally counted until here.
	 * Each sweep clears recent bits before evicting, so progress is bounded. */
	while (textrun_bytes + textrun_imagebytes > TEXTRUN_MAXBYTES) {
		entry = &textrun_entries[textrun_hand];
		textrun_hand = (textrun_hand + 1) & (TEXTRUN_SLOTS - 1);
		if (entry->recent)
			entry->recent = 0;
		else if (entry->runes) {
			textrun_freeentry(entry);
			textrun_collect();
		}
	}
}

static void
textrun_clear(void)
{
	unsigned int i;

	for (i = 0; i < TEXTRUN_SLOTS; i++)
		if (textrun_entries[i].runes)
			textrun_freeentry(&textrun_entries[i]);
	textrun_collect();
	textrun_hand = textrun_tick = 0;
	textrun_serial = 0;
	textrun_activity = 0;
}

static int
textrun_oldest(const void *a, const void *b)
{
	const TextrunEntry *const *left = a, *const *right = b;
	uint32_t ageleft = textrun_serial - (*left)->lastused;
	uint32_t ageright = textrun_serial - (*right)->lastused;

	return (ageleft < ageright) - (ageleft > ageright);
}

/* No emitted glyph specs may remain: only call at a completed frame boundary.
 * Keep the most recently used words within an idle budget. Local repaints may
 * not touch every visible word, so this is a hot set, not a screen snapshot. */
static size_t
textrun_trim(size_t target)
{
	TextrunEntry *entries[TEXTRUN_SLOTS];
	size_t before = textrun_bytes + textrun_imagebytes, count = 0;

	textrun_collect();
	if (textrun_bytes + textrun_imagebytes > target) {
		for (size_t i = 0; i < TEXTRUN_SLOTS; i++)
			if (textrun_entries[i].runes)
				entries[count++] = &textrun_entries[i];
		qsort(entries, count, sizeof(*entries), textrun_oldest);
		for (size_t i = 0; i < count &&
		     textrun_bytes + textrun_imagebytes > target; i++) {
			textrun_freeentry(entries[i]);
			textrun_collect();
		}
	}
	return before - textrun_bytes - textrun_imagebytes;
}

static int
textrun_same(const struct fcft_glyph *a, const struct fcft_glyph *b)
{
	const unsigned char *ap, *bp;
	size_t rowbytes;
	int astride, bstride, y;
	pixman_format_code_t format;

	if (!a || !b || a->x != b->x || a->y != b->y ||
	    a->width != b->width || a->height != b->height ||
	    a->is_color_glyph != b->is_color_glyph)
		return 0;
	if (!a->pix || !b->pix)
		return a->pix == b->pix;
	format = pixman_image_get_format(a->pix);
	if (format != pixman_image_get_format(b->pix) ||
	    pixman_image_get_component_alpha(a->pix) !=
	    pixman_image_get_component_alpha(b->pix))
		return 0;
	rowbytes = ((size_t)a->width * PIXMAN_FORMAT_BPP(format) + 7) / 8;
	ap = (const unsigned char *)pixman_image_get_data(a->pix);
	bp = (const unsigned char *)pixman_image_get_data(b->pix);
	astride = pixman_image_get_stride(a->pix);
	bstride = pixman_image_get_stride(b->pix);
	/* Image layout is constant across rows. Only compare visible bytes;
	 * padding is not part of glyph identity and strides may differ. */
	for (y = 0; y < a->height; y++, ap += astride, bp += bstride)
		if (memcmp(ap, bp, rowbytes))
			return 0;
	return 1;
}

static uintptr_t
textrun_retain(struct fcft_font *font, uint32_t cp, const struct fcft_glyph *glyph)
{
	const struct fcft_glyph *single = TEXTRUN_RASTERIZE(font, cp);
	TextrunBitmap *bitmap;
	uint32_t hash = 2166136261u;
	pixman_format_code_t format = glyph->pix ? pixman_image_get_format(glyph->pix) : 0;
	const int fields[] = {glyph->x, glyph->y, glyph->width, glyph->height,
		glyph->is_color_glyph, format,
		glyph->pix ? pixman_image_get_component_alpha(glyph->pix) : 0};
	size_t rowbytes = ((size_t)glyph->width * PIXMAN_FORMAT_BPP(format) + 7) / 8;
	unsigned int i, slot;
	int y;

	if (textrun_same(glyph, single))
		return (uintptr_t)single;
	for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
		hash = (hash ^ (uint32_t)fields[i]) * 16777619u;
	for (y = 0; glyph->pix && y < glyph->height; y++) {
		const unsigned char *p = (const void *)((char *)pixman_image_get_data(glyph->pix) +
			(size_t)y * pixman_image_get_stride(glyph->pix));
		for (size_t x = 0; x < rowbytes; x++)
			hash = (hash ^ p[x]) * 16777619u;
	}
	slot = hash & (TEXTRUN_SLOTS - 1);
	for (bitmap = textrun_images[slot]; bitmap; bitmap = bitmap->next)
		if (bitmap->hash == hash && textrun_same(glyph, &bitmap->glyph)) {
			bitmap->refs++;
			return (uintptr_t)bitmap | 1;
		}
	bitmap = TEXTRUN_MALLOC(sizeof(*bitmap));
	if (!bitmap)
		return 0;
	*bitmap = (TextrunBitmap){.glyph = *glyph, .hash = hash,
		.refs = 1, .bytes = sizeof(*bitmap), .next = textrun_images[slot]};
	bitmap->glyph.font_name = NULL;
	bitmap->glyph.pix = NULL;
	if (glyph->pix) {
		bitmap->glyph.pix = pixman_image_create_bits(format, glyph->width, glyph->height, NULL, 0);
		if (!bitmap->glyph.pix) {
			free(bitmap);
			return 0;
		}
		pixman_image_set_component_alpha(bitmap->glyph.pix, pixman_image_get_component_alpha(glyph->pix));
		for (y = 0; y < glyph->height; y++)
			memcpy((char *)pixman_image_get_data(bitmap->glyph.pix) + (size_t)y * pixman_image_get_stride(bitmap->glyph.pix),
			       (char *)pixman_image_get_data(glyph->pix) + (size_t)y * pixman_image_get_stride(glyph->pix), rowbytes);
		bitmap->bytes += (size_t)pixman_image_get_stride(bitmap->glyph.pix) * glyph->height;
	}
	textrun_imagebytes += bitmap->bytes;
	textrun_images[slot] = bitmap;
	return (uintptr_t)bitmap | 1;
}

/* Validate fcft cluster indices and accumulation before exposing them to the
 * renderer. Character-cell coordinates come from clusters, not advances. */
static int
textrun_positions(const struct fcft_text_run *source, TextrunGlyph *glyphs, int length)
{
	int64_t dx = 0, dy = 0;
	int previous = -1, cluster;
	size_t i;

	for (i = 0; i < source->count; i++) {
		cluster = source->cluster[i];
		if (cluster < 0 || cluster >= length || !source->glyphs[i])
			return 0;
		if (cluster != previous)
			dx = dy = 0;
		if (dx < INT_MIN || dx > INT_MAX || dy < INT_MIN || dy > INT_MAX)
			return 0;
		glyphs[i] = (TextrunGlyph){.ref = (uintptr_t)source->glyphs[i],
			.cluster = cluster, .dx = dx, .dy = dy};
		dx += source->glyphs[i]->advance.x;
		dy -= source->glyphs[i]->advance.y;
		previous = cluster;
	}
	return 1;
}

static const TextRun *
textrun(struct fcft_font *font, const uint32_t *runes, int length)
{
	uint32_t hash = 2166136261u ^ (uint32_t)(uintptr_t)font;
	TextrunEntry pending = {0}, *entry, *victim = NULL;
	TextrunTransient *transient;
	struct fcft_text_run *source;
	size_t bytes;
	unsigned int way;
	int i;

	if (!font || !runes || length <= 0)
		return NULL;
	textrun_serial++;
	textrun_activity = 1;
	hash *= 16777619u;
	for (i = 0; i < length; i++)
		hash = (hash ^ runes[i]) * 16777619u;
	for (way = 0; way < 2; way++) {
		entry = &textrun_entries[(hash + way) & (TEXTRUN_SLOTS - 1)];
		if (entry->runes && entry->font == font && entry->hash == hash &&
		    entry->length == length && !memcmp(entry->runes, runes, (size_t)length * sizeof(*runes))) {
			entry->recent = 1;
			entry->lastused = textrun_serial;
			return &entry->run;
		}
		if (!victim || !entry->runes || (victim->runes && (textrun_tick & 1) == way))
			victim = entry;
	}
	textrun_tick++;
	/* Include uncached runs and input absorbed into ligature clusters in
	 * the font's codepoint budget, without rasterizing unused singles. */
	for (i = 0; i < length; i++)
		TEXTRUN_TRACK(font, runes[i]);
	source = TEXTRUN_SHAPE(font, length, runes);
	if (!source)
		return NULL;
	if (source->count > INT_MAX || source->count > SIZE_MAX / sizeof(TextrunGlyph) ||
	    (size_t)length > (SIZE_MAX - source->count * sizeof(TextrunGlyph)) / sizeof(*runes))
		goto fail;
	bytes = (size_t)length * sizeof(*runes) +
		(source->count ? source->count : 1) * sizeof(TextrunGlyph);
	pending.run.glyphs = TEXTRUN_MALLOC((source->count ? source->count : 1) * sizeof(TextrunGlyph));
	if (!pending.run.glyphs || !textrun_positions(source, pending.run.glyphs, length))
		goto fail;
	if (length > TEXTRUN_MAXLEN || bytes > TEXTRUN_MAXBYTES) {
		/* Oversized runs still shape correctly, but never enter the cache.
		 * fcft's temporary glyphs are retained only through this frame. */
		transient = TEXTRUN_MALLOC(sizeof(*transient));
		if (!transient)
			goto fail;
		*transient = (TextrunTransient){.run = {.count = source->count,
			.glyphs = pending.run.glyphs}, .source = source, .next = textrun_transients};
		textrun_transients = transient;
		return &transient->run;
	}
	pending.runes = TEXTRUN_MALLOC((size_t)length * sizeof(*runes));
	if (!pending.runes)
		goto fail;
	memcpy(pending.runes, runes, (size_t)length * sizeof(*runes));
	for (i = 0; i < (int)source->count; i++) {
		uintptr_t ref = textrun_retain(font, runes[pending.run.glyphs[i].cluster], source->glyphs[i]);
		if (!ref)
			goto fail;
		pending.run.glyphs[i].ref = ref;
		pending.run.count++;
	}
	fcft_text_run_destroy(source);
	if (victim->runes)
		textrun_freeentry(victim);
	while (textrun_bytes > TEXTRUN_MAXBYTES - bytes) {
		entry = &textrun_entries[textrun_hand];
		textrun_hand = (textrun_hand + 1) & (TEXTRUN_SLOTS - 1);
		if (entry->recent)
			entry->recent = 0;
		else if (entry->runes)
			textrun_freeentry(entry);
	}
	pending.font = font;
	pending.hash = hash;
	pending.length = length;
	pending.bytes = bytes;
	pending.lastused = textrun_serial;
	pending.recent = 1;
	*victim = pending;
	textrun_bytes += bytes;
	return &victim->run;

fail:
	for (i = 0; i < pending.run.count; i++)
		textrun_unref(pending.run.glyphs[i].ref);
	free(pending.run.glyphs);
	free(pending.runes);
	fcft_text_run_destroy(source);
	return NULL;
}
