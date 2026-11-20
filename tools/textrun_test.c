/* Exercise real fcft shaping, cache budgets, bitmap ownership, and OOM fallback. */
#include <assert.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcft/fcft.h>

static int fail_after = -1, fail_shape;
static size_t shapes, tracked;

static void *
test_malloc(size_t bytes)
{
	if (fail_after == 0)
		return NULL;
	if (fail_after > 0)
		fail_after--;
	return malloc(bytes);
}

static struct fcft_text_run *
test_shape(struct fcft_font *font, size_t length, const uint32_t *runes)
{
	shapes++;
	return fail_shape ? NULL :
		fcft_rasterize_text_run_utf32(font, length, runes, FCFT_SUBPIXEL_DEFAULT);
}

#define TEXTRUN_SLOTS 64u
#define TEXTRUN_MALLOC test_malloc
#define TEXTRUN_SHAPE test_shape
#define TEXTRUN_TRACK(font, cp) ((void)(font), (void)(cp), tracked++)
#include "../patch/textrun.c"

static size_t
entries(void)
{
	size_t count = 0;
	for (size_t i = 0; i < TEXTRUN_SLOTS; i++)
		count += !!textrun_entries[i].runes;
	return count;
}

static void
assert_empty(void)
{
	assert(entries() == 0);
	assert(textrun_bytes == 0 && textrun_imagebytes == 0);
	assert(!textrun_retired && !textrun_transients);
	for (size_t i = 0; i < TEXTRUN_SLOTS; i++)
		assert(!textrun_images[i]);
}

static void
check_hit_and_font_key(struct fcft_font *font, struct fcft_font *other)
{
	const uint32_t text[] = {'r', 'e', 'p', 'e', 'a', 't'};
	const TextRun *run;
	size_t before = shapes, tracked_before = tracked;

	run = textrun(font, text, sizeof(text) / sizeof(text[0]));
	assert(run && run->count > 0 && shapes == before + 1);
	assert(tracked == tracked_before + 6);
	for (int i = 0; i < run->count; i++) {
		const struct fcft_glyph *glyph = textrun_glyph(run->glyphs[i].ref);
		assert(glyph && run->glyphs[i].cluster >= 0 && run->glyphs[i].cluster < 6);
		/* Printable monospaced characters reuse fcft's character cache. */
		assert(!(run->glyphs[i].ref & 1));
		assert(glyph == fcft_rasterize_char_utf32(font, text[run->glyphs[i].cluster], FCFT_SUBPIXEL_DEFAULT));
	}
	assert(textrun(font, text, 6) == run);
	assert(shapes == before + 1);
	assert(tracked == tracked_before + 6);
	assert(textrun(other, text, 6));
	assert(shapes == before + 2);
	assert(tracked == tracked_before + 12);
	textrun_clear();
	assert_empty();
}

static void
check_budget_and_eviction(struct fcft_font *font)
{
	uint32_t text[16];
	const struct fcft_glyph *pending[128];
	size_t before = shapes;

	for (int i = 0; i < 16; i++)
		text[i] = 'a';
	for (int i = 0; i < 128; i++) {
		text[0] = '!' + i % 80;
		text[1] = '!' + i / 80;
		const TextRun *run = textrun(font, text, 16);
		assert(run && run->count > 0);
		pending[i] = textrun_glyph(run->glyphs[0].ref);
		assert(entries() <= TEXTRUN_SLOTS);
		assert(textrun_bytes <= TEXTRUN_MAXBYTES);
	}
	assert(shapes == before + 128);
	assert(entries() < 128);
	/* Saved specs remain safe to inspect after the cache entries are evicted. */
	for (int i = 0; i < 128; i++)
		assert(pending[i]->width >= 0 && pending[i]->height >= 0);
	textrun_endframe();
	assert(textrun_bytes + textrun_imagebytes <= TEXTRUN_MAXBYTES);
	textrun_clear();
	assert_empty();
}

static void
check_bitmap_ownership(struct fcft_font *font)
{
	struct fcft_glyph glyph = {.cp = 'a', .x = -3, .y = 7, .width = 3, .height = 2};
	uintptr_t first, second, third;
	const struct fcft_glyph *saved;
	size_t bytes;

	glyph.pix = pixman_image_create_bits(PIXMAN_a8, 3, 2, NULL, 0);
	assert(glyph.pix);
	memset(pixman_image_get_data(glyph.pix), 0x5a, 8);
	first = textrun_retain(font, 'a', &glyph);
	assert(first & 1);
	bytes = textrun_imagebytes;
	second = textrun_retain(font, 'a', &glyph);
	assert(second == first && textrun_imagebytes == bytes);
	/* Advance does not belong to bitmap identity. Positions are held per run. */
	glyph.advance.x = 99;
	assert((third = textrun_retain(font, 'a', &glyph)) == first);
	textrun_unref(third);
	saved = textrun_glyph(first);
	assert(saved->pix != glyph.pix);
	memset(pixman_image_get_data(glyph.pix), 0, 8);
	assert(*(unsigned char *)pixman_image_get_data(saved->pix) == 0x5a);
	pixman_image_unref(glyph.pix);
	textrun_unref(first);
	textrun_unref(second);
	assert(textrun_imagebytes == bytes && textrun_retired);
	/* Revive an evicted bitmap before its frame-end reclamation. */
	third = textrun_retain(font, 'a', saved);
	assert(third == first);
	textrun_endframe();
	assert(textrun_imagebytes == bytes && !textrun_retired);
	textrun_unref(third);
	assert(*(unsigned char *)pixman_image_get_data(saved->pix) == 0x5a);
	textrun_endframe();
	assert_empty();
}

static void
check_ligatures(struct fcft_font *font)
{
	const uint32_t first[] = {'f', 'i', 'X'}, second[] = {'f', 'i', 'Y'};
	const uint32_t two_ligatures[] = {'f', 'i', 'f', 'l'};
	const TextRun *run;
	const struct fcft_glyph *saved;
	uintptr_t ref;
	size_t imagebytes;
	int succeeded = 0;

	run = textrun(font, first, 3);
	assert(run && run->count == 2 && (run->glyphs[0].ref & 1));
	ref = run->glyphs[0].ref;
	saved = textrun_glyph(ref);
	assert(saved->width > 0 && saved->pix);
	imagebytes = textrun_imagebytes;
	run = textrun(font, second, 3);
	assert(run && run->count == 2 && run->glyphs[0].ref == ref);
	assert(textrun_imagebytes == imagebytes);
	/* Retire the real ligature's cache entries while keeping rendered specs. */
	for (size_t i = 0; i < TEXTRUN_SLOTS; i++)
		if (textrun_entries[i].runes)
			textrun_freeentry(&textrun_entries[i]);
	assert(saved->width > 0 && pixman_image_get_data(saved->pix));
	textrun_endframe();
	assert_empty();
	for (int failure = 0; failure < 20; failure++) {
		fail_after = failure;
		run = textrun(font, two_ligatures, 4);
		fail_after = -1;
		if (run) {
			succeeded = 1;
			textrun_clear();
			break;
		}
		textrun_endframe();
		assert_empty();
	}
	assert(succeeded);
	assert_empty();
}

static void
check_bitmap_layout(struct fcft_font *font)
{
	uint32_t left[3], right[6];
	struct fcft_glyph a = {.cp = 'a', .x = -2, .y = 8, .width = 3, .height = 3};
	struct fcft_glyph b = a;
	uintptr_t first, second;

	/* The unused byte and extra stride bytes intentionally differ. */
	memset(left, 0xa5, sizeof left);
	memset(right, 0x5a, sizeof right);
	for (int y = 0; y < 3; y++) {
		memset((unsigned char *)left + 4 * y, 0x10 + y, 3);
		memset((unsigned char *)right + 8 * y, 0x10 + y, 3);
	}
	a.pix = pixman_image_create_bits(PIXMAN_a8, 3, 3, left, 4);
	b.pix = pixman_image_create_bits(PIXMAN_a8, 3, 3, right, 8);
	assert(a.pix && b.pix && textrun_same(&a, &b));
	first = textrun_retain(font, 'a', &a);
	second = textrun_retain(font, 'a', &b);
	assert((first & 1) && first == second);
	/* A change on the final row must not be missed. */
	((unsigned char *)right)[18] ^= 1;
	assert(!textrun_same(&a, &b));
	((unsigned char *)right)[18] ^= 1;
	pixman_image_set_component_alpha(b.pix, 1);
	assert(!textrun_same(&a, &b));
	pixman_image_set_component_alpha(b.pix, 0);
	b.x++;
	assert(!textrun_same(&a, &b));
	textrun_unref(first);
	textrun_unref(second);
	textrun_endframe();
	pixman_image_unref(a.pix);
	pixman_image_unref(b.pix);
	assert_empty();
}

static void
check_transient_and_failures(struct fcft_font *font)
{
	uint32_t text[1100];
	const uint32_t shorttext[] = {'s', 'h', 'o', 'r', 't'};
	const TextRun *run;
	int succeeded = 0;
	size_t tracked_before = tracked;

	for (int i = 0; i < 1100; i++)
		text[i] = 'x';
	run = textrun(font, text, 1100);
	assert(run && run->count == 1100);
	assert(tracked == tracked_before + 1100);
	assert(entries() == 0 && textrun_transients);
	assert(textrun_glyph(run->glyphs[1099].ref)->pix);
	textrun_endframe();
	assert_empty();
	fail_shape = 1;
	assert(!textrun(font, shorttext, 5));
	fail_shape = 0;
	assert_empty();
	/* Each failed allocation leaves neither a partially installed entry nor
	 * references that survive the frame boundary. The caller can draw singles. */
	for (int failure = 0; failure < 20; failure++) {
		fail_after = failure;
		run = textrun(font, shorttext, 5);
		fail_after = -1;
		if (run) {
			succeeded = 1;
			textrun_clear();
			break;
		}
		assert(fcft_rasterize_char_utf32(font, 's', FCFT_SUBPIXEL_DEFAULT));
		textrun_endframe();
		assert_empty();
	}
	assert(succeeded);
	assert_empty();
	for (int failure = 0; failure < 2; failure++) {
		fail_after = failure;
		assert(!textrun(font, text, 1100));
		fail_after = -1;
		textrun_endframe();
		assert_empty();
	}
	/* Bitmap allocation failure is recoverable too. */
	struct fcft_glyph glyph = {.x = 100, .y = 100, .width = 1, .height = 1};
	glyph.pix = pixman_image_create_bits(PIXMAN_a8, 1, 1, NULL, 0);
	assert(glyph.pix);
	fail_after = 0;
	assert(!textrun_retain(font, 'a', &glyph));
	fail_after = -1;
	pixman_image_unref(glyph.pix);
	assert_empty();
}

/* Idle reclamation should retain recently used words, release cold metadata,
 * and keep shared ligature bitmaps alive as long as a retained word uses them.
 * Cache misses after reclamation must still reproduce the same glyphs. */
static void
check_idle_reclamation(struct fcft_font *font)
{
	const uint32_t cold[] = {'f', 'i', 'X'}, hot[] = {'f', 'i', 'Y'};
	const TextRun *run;
	const struct fcft_glyph *ligature;
	uintptr_t shared;
	size_t coldbytes, bytes, imagebytes, before;
	int count, cluster, dx, dy;

	assert_empty();
	run = textrun(font, cold, 3);
	assert(run && run->count == 2 && (run->glyphs[0].ref & 1));
	shared = run->glyphs[0].ref;
	ligature = textrun_glyph(shared);
	coldbytes = textrun_bytes;
	imagebytes = textrun_imagebytes;
	run = textrun(font, hot, 3);
	assert(run && run->count == 2 && run->glyphs[0].ref == shared);
	assert(entries() == 2 && textrun_imagebytes == imagebytes);
	count = run->count;
	cluster = run->glyphs[0].cluster;
	dx = run->glyphs[0].dx; dy = run->glyphs[0].dy;
	textrun_endframe();
	bytes = textrun_bytes + textrun_imagebytes;
	assert(textrun_trim(bytes) == 0 && entries() == 2);
	assert(textrun_trim(bytes - coldbytes) == coldbytes && entries() == 1);
	assert(ligature->pix && textrun_imagebytes == imagebytes);
	before = shapes;
	run = textrun(font, hot, 3);
	assert(run && shapes == before && run->glyphs[0].ref == shared);
	assert(run->count == count && run->glyphs[0].cluster == cluster &&
	       run->glyphs[0].dx == dx && run->glyphs[0].dy == dy);
	assert(textrun(font, cold, 3) && shapes == before + 1);

	/* A hit must refresh recency too, including across the serial wrap. */
	textrun_serial = UINT32_MAX - 1;
	assert(textrun(font, cold, 3));
	assert(textrun(font, hot, 3));
	before = shapes;
	assert(textrun_trim(bytes - coldbytes) == coldbytes && entries() == 1);
	assert(textrun(font, hot, 3) && shapes == before);
	assert(textrun(font, cold, 3) && shapes == before + 1);

	/* Zero target releases the last owner; collecting again is harmless. */
	bytes = textrun_bytes + textrun_imagebytes;
	assert(textrun_trim(0) == bytes);
	assert_empty();
	assert(textrun_trim(0) == 0);
	before = shapes;
	run = textrun(font, hot, 3);
	assert(run && shapes == before + 1 && run->count == count);
	assert(run->glyphs[0].cluster == cluster && run->glyphs[0].dx == dx &&
	       run->glyphs[0].dy == dy && textrun_glyph(run->glyphs[0].ref)->pix);
	textrun_clear();
	assert_empty();
	assert(!textrun_activity && textrun_serial == 0);
}

int
main(void)
{
	const char *name = "DejaVu Sans Mono:pixelsize=16";
	const char *othername = "DejaVu Sans Mono:pixelsize=18";
	const char *serifname = "DejaVu Serif:pixelsize=16";
	struct fcft_font *font, *other, *serif;

	setlocale(LC_ALL, "");
	assert(fcft_init(FCFT_LOG_COLORIZE_NEVER, 0, FCFT_LOG_CLASS_WARNING));
	assert(fcft_capabilities() & FCFT_CAPABILITY_TEXT_RUN_SHAPING);
	font = fcft_from_name(1, &name, NULL);
	other = fcft_from_name(1, &othername, NULL);
	serif = fcft_from_name(1, &serifname, NULL);
	assert(font && other && serif);
	check_hit_and_font_key(font, other);
	check_budget_and_eviction(font);
	check_bitmap_ownership(font);
	check_bitmap_layout(font);
	check_ligatures(serif);
	check_transient_and_failures(font);
	check_idle_reclamation(serif);
	textrun_clear();
	fcft_destroy(font);
	fcft_destroy(other);
	fcft_destroy(serif);
	fcft_fini();
	printf("textrun: hits, font keys, eviction, budget %u, shared/copied glyphs, deferred lifetime, oversized runs, allocation failures and idle reclamation passed\n", (unsigned)TEXTRUN_MAXBYTES);
	return 0;
}
