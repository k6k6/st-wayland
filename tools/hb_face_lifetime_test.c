/* Headless regression test of the cached HarfBuzz font lifetime in hb.c.
 *
 * hbfindfont() caches one hb_font_t per cairo scaled font and builds it from
 * the FT_Face returned by cairo_ft_scaled_font_lock_face().  That face
 * belongs to cairo's unscaled font: cairo calls FT_Done_Face() on idle faces
 * as soon as MAX_OPEN_FACES (10) are open, which a full screen redraw that
 * pulls in fallback fonts reaches easily.  HarfBuzz reads table data straight
 * out of the memory mapping FreeType made for that face, so a face dying
 * under a live hb_font_t leaves every cached table pointer dangling; once
 * another font file is mapped over that address range, shaping reads garbage
 * table data and dies in hb_shape_plan_create_cached2().  Keeping a reference
 * on the face (hb_ft_font_create_referenced()) is what prevents this.
 *
 * The test drives that eviction deterministically: it holds every other face
 * locked, so when cairo has to close an idle face the primary one is the only
 * candidate.  Before the fix the cached shaping below either segfaults or
 * reads the freed mapping; the assertions on the primary font's live face and
 * file mapping catch the rest.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hb.c"

void
die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	exit(1);
}

/* cairo's face limit; the test needs more faces than this to force eviction */
#define FACE_LIMIT 10
#define EXTRA_FACES (FACE_LIMIT + 4)
#define MAX_FONTS 64

typedef struct {
	cairo_scaled_font_t *sf;
	FT_Face face;		/* held locked while the entry is alive */
	char *file;
} HeldFont;

static HeldFont held[MAX_FONTS];
static int nheld;

/* names fontconfig resolves on most systems; ones that do not exist are
 * simply skipped, entries that resolve to an already used file are dropped */
static const char *candidates[] = {
	"DejaVu Sans", "DejaVu Serif", "DejaVu Sans Mono",
	"Liberation Sans", "Liberation Serif", "Liberation Mono",
	"Noto Sans", "Noto Serif", "Noto Sans Mono", "Noto Sans Devanagari",
	"Noto Sans Tamil", "Noto Sans Mono CJK KR", "Noto Sans CJK SC",
	"Ubuntu", "Cantarell", "FreeSans", "FreeSerif", "FreeMono",
	"Nimbus Sans", "Nimbus Mono PS", "Symbola", "Unifont",
	"WenQuanYi Zen Hei", "Ebrima", "Courier New",
};

static cairo_scaled_font_t *
openscaledfont(const char *name, double size, char **file, char **family)
{
	FcChar8 *value;
	FcPattern *pattern, *match;
	cairo_font_face_t *face;
	cairo_font_options_t *fo;
	cairo_scaled_font_t *sf;
	cairo_matrix_t fm, ctm;
	FcResult res;

	pattern = FcNameParse((const FcChar8 *)name);
	if (!pattern)
		return NULL;
	FcConfigSubstitute(NULL, pattern, FcMatchPattern);
	FcDefaultSubstitute(pattern);
	match = FcFontMatch(NULL, pattern, &res);
	FcPatternDestroy(pattern);
	if (!match)
		return NULL;

	if (file)
		*file = FcPatternGetString(match, FC_FILE, 0, &value) == FcResultMatch ?
		        strdup((char *)value) : NULL;
	if (family)
		*family = FcPatternGetString(match, FC_FAMILY, 0, &value) == FcResultMatch ?
		          strdup((char *)value) : NULL;

	face = cairo_ft_font_face_create_for_pattern(match);
	FcPatternDestroy(match);
	cairo_matrix_init_scale(&fm, size, size);
	cairo_matrix_init_identity(&ctm);
	fo = cairo_font_options_create();
	sf = cairo_scaled_font_create(face, &fm, &ctm, fo);
	cairo_font_options_destroy(fo);
	cairo_font_face_destroy(face);
	if (cairo_scaled_font_status(sf) != CAIRO_STATUS_SUCCESS) {
		cairo_scaled_font_destroy(sf);
		return NULL;
	}
	return sf;
}

/* is the font file currently mapped read-only (FreeType's file mapping)? */
static int
filemapped(const char *path)
{
	char line[4096];
	FILE *maps;
	int found = 0;

	maps = fopen("/proc/self/maps", "r");
	if (!maps)
		return -1;
	while (fgets(line, sizeof(line), maps))
		if (strstr(line, path) && strstr(line, "r--p")) {
			found = 1;
			break;
		}
	fclose(maps);
	return found;
}

static void
shapeinto(HbTransformData *out, cairo_scaled_font_t *sf, const char *text)
{
	Glyph glyphs[64];

	assert(strlen(text) <= sizeof(glyphs) / sizeof(glyphs[0]));
	for (size_t i = 0; i < strlen(text); i++)
		glyphs[i] = (Glyph){.u = (Rune)text[i]};
	hbtransform(out, sf, glyphs, 0, (int)strlen(text));
	assert(out->count == strlen(text));
	for (unsigned i = 0; i < out->count; i++)
		assert(out->glyphs[i].codepoint != 0);
}

static void
shapeexpect(HbTransformData *got, HbTransformData *want)
{
	assert(got->count == want->count);
	assert(!memcmp(got->glyphs, want->glyphs, got->count * sizeof(*got->glyphs)));
	assert(!memcmp(got->positions, want->positions, got->count * sizeof(*got->positions)));
}

/* open and lock one more face, which is what pushes cairo into closing idle
 * faces; every held face stays locked, so the idle primary is chosen */
static int
holdanotherfont(const char *name)
{
	char *file = NULL;
	cairo_scaled_font_t *sf;
	FT_Face face;

	if (nheld >= MAX_FONTS)
		return 0;
	sf = openscaledfont(name, 14, &file, NULL);
	if (!sf || !file) {
		if (sf)
			cairo_scaled_font_destroy(sf);
		free(file);
		return 0;
	}
	face = cairo_ft_scaled_font_lock_face(sf);
	if (!face) {
		cairo_scaled_font_destroy(sf);
		free(file);
		return 0;
	}
	held[nheld].sf = sf;
	held[nheld].face = face;
	held[nheld].file = file;
	nheld++;
	return 1;
}

int
main(void)
{
	HbTransformData first, cached, reference;
	cairo_scaled_font_t *sf, *check;
	FT_Face ft, evicted;
	char *primaryfile = NULL, *primaryfamily = NULL;
	int distinct = 0, mapped;

	FcInit();
	sf = openscaledfont("DejaVu Sans Mono:size=10", 14, &primaryfile, &primaryfamily);
	if (!sf)
		sf = openscaledfont("monospace:size=10", 14, &primaryfile, &primaryfamily);
	assert(sf != NULL && primaryfile != NULL);

	ft = cairo_ft_scaled_font_lock_face(sf);
	assert(ft != NULL);
	if (ft->stream->read != NULL) {
		printf("SKIP %s: FreeType serves tables without a file mapping\n",
		       primaryfamily);
		return 0;
	}
	if (filemapped(primaryfile) != 1) {
		printf("SKIP %s: font file is not mapped\n", primaryfamily);
		return 0;
	}

	hbcreatebuffer();

	/* st caches the hb_font_t here, keyed on the cairo scaled font */
	shapeinto(&first, sf, "abcdefgh");
	assert(hbfontcache.capacity == 1);
	cairo_ft_scaled_font_unlock_face(sf);

	/* hold more distinct faces than cairo keeps open */
	for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
		if (nheld >= EXTRA_FACES)
			break;
		if (!holdanotherfont(candidates[i]))
			continue;
		if (!strcmp(held[nheld - 1].file, primaryfile)) {
			/* same file: shares the primary face, no extra pressure */
			nheld--;
			free(held[nheld].file);
			cairo_ft_scaled_font_unlock_face(held[nheld].sf);
			cairo_scaled_font_destroy(held[nheld].sf);
			continue;
		}
		distinct++;
	}
	if (distinct < FACE_LIMIT + 1) {
		printf("SKIP %s: only %d other mapped fonts available\n",
		       primaryfamily, distinct);
		return 0;
	}
	/* this lock is the one that closes the idle primary face */
	holdanotherfont("Symbola:size=10");
	holdanotherfont("Unifont:size=10");

	/* the cached hb_font_t must still shape, and its face must be alive:
	 * it owns a reference precisely so cairo cannot pull the file mapping
	 * out from under HarfBuzz's zero-copy table pointers */
	assert(ft->num_glyphs > 0 && ft->family_name != NULL);
	assert(filemapped(primaryfile) == 1);
	cached.buffer = NULL;
	shapeinto(&cached, sf, "yazi scrolling");

	/* prove the face really was closed and re-created by cairo, then check
	 * the cached result against a font built from that fresh face */
	check = sf;
	evicted = cairo_ft_scaled_font_lock_face(check);
	assert(evicted != ft);
	shapeinto(&reference, check, "yazi scrolling");
	shapeexpect(&cached, &reference);
	cairo_ft_scaled_font_unlock_face(check);

	/* fallback churn as a full screen redraw produces it, then shape again */
	for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++)
		holdanotherfont(candidates[i]);
	while (nheld > 0) {
		nheld--;
		cairo_ft_scaled_font_unlock_face(held[nheld].sf);
		cairo_scaled_font_destroy(held[nheld].sf);
		free(held[nheld].file);
	}
	shapeinto(&cached, sf, "applications");
	shapeinto(&reference, check, "applications");
	shapeexpect(&cached, &reference);

	hbunloadfonts();
	assert(hbfontcache.capacity == 0);
	hbdestroybuffer();
	free(hbrunebuffer.runes);
	cairo_scaled_font_destroy(sf);
	free(primaryfile);
	free(primaryfamily);
	cairo_debug_reset_static_data();
	FcFini();
	printf("PASS: cached hb_font_t survived FT_Face eviction (%d other faces held)\n",
	       distinct);
	return 0;
}
