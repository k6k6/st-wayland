/* fcft fonts and pixman text. Included by wl.c with fcft-owned fonts. */
#ifndef FCFT_CACHE_MAXGLYPHS
#define FCFT_CACHE_MAXGLYPHS 8192u
#endif
#if FCFT_CACHE_MAXGLYPHS < 1
#error FCFT_CACHE_MAXGLYPHS must be positive
#endif
#define FCFT_CACHE_SLOTS (FCFT_CACHE_MAXGLYPHS * 2u)
static struct {
	struct fcft_font *font;
	uint32_t cp;
} fcftseen[FCFT_CACHE_SLOTS];
static size_t fcftglyphcount;
static int fcftrebuild, fcftinitialized;
static pixman_image_t *fcftsolids[16];
static RenderColor fcftcolors[16];
static unsigned int fcftsolidhand;

static void
fcftcache_clear(void)
{
	memset(fcftseen, 0, sizeof(fcftseen));
	fcftglyphcount = 0;
	fcftrebuild = 0;
}

static void
fcfttrack(struct fcft_font *font, uint32_t cp)
{
	/* Count each (style, codepoint) once. Stop recording when a rebuild is
	 * queued; the rest of the frame can still use all its existing specs. */
	if (!fcftrebuild) {
		size_t slot = ((uintptr_t)font >> 4 ^ cp * 2654435761u) % FCFT_CACHE_SLOTS;
		while (fcftseen[slot].font &&
		       (fcftseen[slot].font != font || fcftseen[slot].cp != cp))
			slot = (slot + 1) % FCFT_CACHE_SLOTS;
		if (!fcftseen[slot].font) {
			fcftseen[slot].font = font;
			fcftseen[slot].cp = cp;
			if (++fcftglyphcount >= FCFT_CACHE_MAXGLYPHS)
				fcftrebuild = 1;
		}
	}
}

static const struct fcft_glyph *
fcftglyph(struct fcft_font *font, uint32_t cp)
{
	fcfttrack(font, cp);
	return fcft_rasterize_char_utf32(font, cp, FCFT_SUBPIXEL_DEFAULT);
}

#if LIGATURES_PATCH
#define TEXTRUN_TRACK(font, cp) fcfttrack((font), (cp))
#define TEXTRUN_RASTERIZE(font, cp) fcftglyph((font), (cp))
#include "textrun.c"
static Rune *fcftrunes;
static int *fcftcolumns;
static size_t fcftspecsize;
#if WIDE_GLYPHS_PATCH
/* Bounds relative to each glyph's source cell, including shaping offsets.
 * Keep these until all saved text rows are invalidated: an unchanged glyph
 * anywhere on a row may extend into the pixels of a partial repaint. */
static int fcftspillleft, fcftspillright;

static void
fcftspill(const struct fcft_glyph *glyph, int dx)
{
	if (glyph && glyph->pix && glyph->width > 0) {
		int64_t left = (int64_t)dx + glyph->x;
		int64_t right = left + glyph->width - win.cw;
		fcftspillleft = MAX(fcftspillleft, MIN(INT_MAX, -left));
		fcftspillright = MAX(fcftspillright, MIN(INT_MAX, right));
	}
}
#endif
#endif

static void
fcftsolid_clear(void)
{
	for (size_t i = 0; i < LEN(fcftsolids); i++) {
		if (fcftsolids[i])
			pixman_image_unref(fcftsolids[i]);
		fcftsolids[i] = NULL;
	}
	fcftsolidhand = 0;
}

static int
fcftloadfont(Font *f, FcPattern *pattern)
{
	FcPattern *configured, *p;
	int slant = FC_SLANT_ROMAN, weight = FC_WEIGHT_REGULAR;
	double pixelsize, advance = 0;
	size_t count = 1;

	if (!fcftinitialized) {
		if (!fcft_init(FCFT_LOG_COLORIZE_NEVER, 0, FCFT_LOG_CLASS_WARNING))
			return 1;
		fcftinitialized = 1;
		#if LIGATURES_PATCH
		if (!(fcft_capabilities() & FCFT_CAPABILITY_TEXT_RUN_SHAPING))
			die("fcft was built without text-run shaping support\n");
		#endif
	}
	configured = FcPatternDuplicate(pattern);
	if (!configured)
		return 1;
	pixelsize = patternpixelsize(pattern) * xw.scale;
	FcPatternDel(configured, FC_PIXEL_SIZE);
	FcPatternDel(configured, FC_SIZE);
	FcPatternAddDouble(configured, FC_PIXEL_SIZE, pixelsize);
	FcConfigSubstitute(NULL, configured, FcMatchPattern);
	xdefaultsubstitute(configured);
	FcDefaultSubstitute(configured);
	FcPatternGetInteger(pattern, FC_SLANT, 0, &slant);
	FcPatternGetInteger(pattern, FC_WEIGHT, 0, &weight);
	#if FONT2_PATCH
	count += LEN(font2);
	#endif
	f->names = xmalloc(count * sizeof(*f->names));
	memset(f->names, 0, count * sizeof(*f->names));
	f->nfonts = count;
	f->pattern = configured;
	for (size_t i = 0; i < count; i++) {
		if (!i) {
			p = FcPatternDuplicate(configured);
		} else {
			#if FONT2_PATCH
			double px, dpi;
			p = FcNameParse((const FcChar8 *)font2[i - 1]);
			if (!p)
				goto fail;
			if (FcPatternGetDouble(p, FC_PIXEL_SIZE, 0, &px) != FcResultMatch) {
				if (FcPatternGetDouble(p, FC_SIZE, 0, &px) != FcResultMatch)
					px = 12;
				if (FcPatternGetDouble(p, FC_DPI, 0, &dpi) != FcResultMatch)
					dpi = 96;
				px *= dpi / 72.;
			}
			if (defaultfontsize > 0)
				px *= usedfontsize / defaultfontsize;
			FcPatternDel(p, FC_SIZE);
			FcPatternDel(p, FC_PIXEL_SIZE);
			FcPatternAddDouble(p, FC_PIXEL_SIZE, px * xw.scale);
			FcPatternDel(p, FC_SLANT);
			FcPatternDel(p, FC_WEIGHT);
			FcPatternAddInteger(p, FC_SLANT, slant);
			FcPatternAddInteger(p, FC_WEIGHT, weight);
			FcPatternAddBool(p, FC_SCALABLE, FcTrue);
			xdefaultsubstitute(p);
			#else
			p = NULL;
			#endif
		}
		if (!p)
			goto fail;
		#if LIGATURES_PATCH
		/* Preserve the terminal's shaping feature defaults. */
		FcPatternAddString(p, FC_FONT_FEATURES, (const FcChar8 *)"cv01");
		FcPatternAddString(p, FC_FONT_FEATURES, (const FcChar8 *)"cv02");
		FcPatternAddString(p, FC_FONT_FEATURES, (const FcChar8 *)"cv61");
		#endif
		f->names[i] = (char *)FcNameUnparse(p);
		FcPatternDestroy(p);
		if (!f->names[i])
			goto fail;
	}
	f->match = fcft_from_name(count, (const char **)f->names, NULL);
	if (!f->match)
		goto fail;
	f->ascent = f->match->ascent;
	f->descent = f->match->descent;
	f->height = f->ascent + f->descent;
	f->lbearing = 0;
	f->rbearing = f->match->max_advance.x;
	f->badslant = f->badweight = 0;
	for (const char *c = ascii_printable; *c; c++) {
		const struct fcft_glyph *g = fcftglyph(f->match, (unsigned char)*c);
		if (!g)
			goto fail;
		advance += g->advance.x;
	}
	#if WIDE_GLYPH_SPACING_PATCH
	if (advance > 18 * strlen(ascii_printable))
		advance /= 3;
	#endif
	f->width = MAX(1, (int)ceil(advance / strlen(ascii_printable)));
	return 0;
fail:
	xunloadfont(f);
	return 1;
}

/* Unused styles only retain their requested pattern. Keep selection separate
 * from loading: checking a word boundary must not instantiate its next style. */
static void
fcftdeferfont(Font *f, FcPattern *pattern)
{
	f->pattern = FcPatternDuplicate(pattern);
	if (!f->pattern)
		die("can't save fcft font pattern\n");
}

static void
fcftensurefont(Font *f)
{
	FcPattern *pattern;
	int failed;

	if (f->match)
		return;
	pattern = f->pattern;
	f->pattern = NULL;
	failed = !pattern || fcftloadfont(f, pattern);
	if (pattern)
		FcPatternDestroy(pattern);
	if (failed)
		die("can't load fcft font style\n");
}

static void
fcftcache_endframe(void)
{
	#if LIGATURES_PATCH
	textrun_endframe();
	#endif
	if (!fcftrebuild)
		return;
	Font *fonts[] = {&dc.font, &dc.bfont, &dc.ifont, &dc.ibfont};
	int loaded[LEN(fonts)];
	#if LIGATURES_PATCH
	textrun_clear();
	#endif
	/* Destroy all loaded styles first: fcft can share matching instances.
	 * No emitted glyph pointer is live beyond the frame boundary. */
	for (size_t i = 0; i < LEN(fonts); i++) {
		loaded[i] = fonts[i]->match != NULL;
		fcft_destroy(fonts[i]->match);
		fonts[i]->match = NULL;
	}
	fcftcache_clear();
	for (size_t i = 0; i < LEN(fonts); i++) {
		Font *f = fonts[i];
		if (!loaded[i])
			continue;
		f->match = fcft_from_name(f->nfonts, (const char **)f->names, NULL);
		if (!f->match)
			die("can't reload fcft font cache\n");
	}
}

static void
fcftdraw(Color *fg, const GlyphFontSpec *specs, int len,
         int cx, int cy, int cw, int ch)
{
	pixman_image_t *solid = NULL;
	int x2, y2;
	if (!xw.pix || len <= 0 || cw <= 0 || ch <= 0)
		return;
	x2 = MIN(pixman_image_get_width(xw.pix), cx + cw);
	y2 = MIN(pixman_image_get_height(xw.pix), cy + ch);
	cx = MAX(0, cx); cy = MAX(0, cy);
	if (x2 <= cx || y2 <= cy)
		return;
	for (size_t i = 0; i < LEN(fcftsolids); i++)
		if (fcftsolids[i] && !memcmp(&fcftcolors[i], &fg->color, sizeof(RenderColor))) {
			solid = fcftsolids[i];
			break;
		}
	if (!solid) {
		unsigned int slot = fcftsolidhand++ % LEN(fcftsolids);
		uint32_t a = fg->color.alpha;
		pixman_color_t color = {
			(fg->color.red * a + 32767) / 65535,
			(fg->color.green * a + 32767) / 65535,
			(fg->color.blue * a + 32767) / 65535, a};
		if (fcftsolids[slot])
			pixman_image_unref(fcftsolids[slot]);
		fcftcolors[slot] = fg->color;
		solid = fcftsolids[slot] = pixman_image_create_solid_fill(&color);
		if (!solid)
			die("can't allocate pixman text color\n");
	}
	for (int i = 0; i < len; i++) {
		const struct fcft_glyph *g = specs[i].imageglyph;
		if (!g || !g->pix || !g->width || !g->height)
			continue;
		int gx = (int)roundf(specs[i].x) + g->x;
		int gy = (int)roundf(specs[i].y) - g->y;
		int x = MAX(gx, cx), y = MAX(gy, cy);
		int w = MIN(gx + g->width, x2) - x;
		int h = MIN(gy + g->height, y2) - y;
		if (w <= 0 || h <= 0)
			continue;
		if (g->is_color_glyph)
			pixman_image_composite32(PIXMAN_OP_OVER, g->pix, NULL, xw.pix,
				x - gx, y - gy, 0, 0, x, y, w, h);
		else
			pixman_image_composite32(PIXMAN_OP_OVER, solid, g->pix, xw.pix,
				0, 0, x - gx, y - gy, x, y, w, h);
	}
}

static Font *
fcftstyle(uint32_t mode)
{
	if (mode & ATTR_ITALIC)
		return mode & ATTR_BOLD ? &dc.ibfont : &dc.ifont;
	return mode & ATTR_BOLD ? &dc.bfont : &dc.font;
}

static int
fcftspecs(GlyphFontSpec *specs, const Glyph *glyphs, int len, int x, int y)
{
	#if ANYSIZE_PATCH
	float xp = win.hborderpx + x * win.cw, yp = win.vborderpx + y * win.ch;
	#else
	float xp = borderpx + x * win.cw, yp = borderpx + y * win.ch;
	#endif
	#if VERTCENTER_PATCH
	yp += win.cyo;
	#endif
	int n = 0;
	if (len <= 0)
		return 0;
	#if LIGATURES_PATCH
	if ((size_t)len > fcftspecsize) {
		fcftrunes = xrealloc(fcftrunes, (size_t)len * sizeof(*fcftrunes));
		fcftcolumns = xrealloc(fcftcolumns, (size_t)len * sizeof(*fcftcolumns));
		fcftspecsize = len;
	}
	#endif
	for (int i = 0; i < len;) {
		Font *font = fcftstyle(glyphs[i].mode);
		if (glyphs[i].mode & ATTR_WDUMMY) {
			i++;
			continue;
		}
		/* Spaces still occupy a spec slot (and receive backgrounds and
		 * decorations), but neither whitespace nor synthetic boxes need a
		 * font style. Box drawing uses cell coordinates, not this baseline. */
		if (glyphs[i].u == ' ') {
			specs[n++] = (GlyphFontSpec){.x = xp + i * win.cw,
				.y = yp + dc.font.ascent};
			i++;
			continue;
		}
		#if BOXDRAW_PATCH
		if (glyphs[i].mode & ATTR_BOXDRAW) {
			specs[n++] = (GlyphFontSpec){.glyph = boxdrawindex(&glyphs[i]),
				.x = xp + i * win.cw, .y = yp + dc.font.ascent};
			i++;
			continue;
		}
		#endif
		fcftensurefont(font);
		#if LIGATURES_PATCH
		if (glyphs[i].u != ' ') {
			int count = 0;
			/* Words keep common source tokens reusable across line edits. */
			while (i < len && glyphs[i].u != ' ' &&
			       fcftstyle(glyphs[i].mode) == font
			       #if BOXDRAW_PATCH
			       && !(glyphs[i].mode & ATTR_BOXDRAW)
			       #endif
			       ) {
				if (!(glyphs[i].mode & ATTR_WDUMMY)) {
					fcftrunes[count] = glyphs[i].u;
					fcftcolumns[count++] = i;
				}
				i++;
			}
			const TextRun *run = textrun(font->match, fcftrunes, count);
			/* The terminal reserves four specs per source cell. A font may
			 * expand a character arbitrarily; fall back safely in that case. */
			if (run && run->count <= count * 4) {
				for (int j = 0; j < run->count; j++) {
					const TextrunGlyph *g = &run->glyphs[j];
					const struct fcft_glyph *image = textrun_glyph(g->ref);
					#if WIDE_GLYPHS_PATCH
					fcftspill(image, g->dx);
					#endif
					specs[n++] = (GlyphFontSpec){.imageglyph = image,
						.x = xp + fcftcolumns[g->cluster] * win.cw + g->dx,
						.y = yp + font->ascent + g->dy};
				}
			} else {
				for (int j = 0; j < count; j++) {
					const struct fcft_glyph *image = fcftglyph(font->match, fcftrunes[j]);
					#if WIDE_GLYPHS_PATCH
					fcftspill(image, 0);
					#endif
					specs[n++] = (GlyphFontSpec){.imageglyph = image,
						.x = xp + fcftcolumns[j] * win.cw, .y = yp + font->ascent};
				}
			}
			continue;
		}
		#endif
		const struct fcft_glyph *image = fcftglyph(font->match, glyphs[i].u);
		#if LIGATURES_PATCH && WIDE_GLYPHS_PATCH
		fcftspill(image, 0);
		#endif
		specs[n++] = (GlyphFontSpec){.imageglyph = image,
			.x = xp + i * win.cw, .y = yp + font->ascent};
		i++;
	}
	return n;
}
