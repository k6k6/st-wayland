/* Headless tests of the actual Wayland font loader and glyph selection. */
#define main st_program_main
#include "../wl.c"
#undef main
#include <assert.h>

static void
checksize(const char *name, double size)
{
	FcPattern *pattern = FcNameParse((const FcChar8 *)name);
	double actual;
	assert(pattern);
	assert(FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &actual) == FcResultMatch);
	assert(fabs(actual - size) < 1e-5);
	FcPatternDestroy(pattern);
}

static void
sameglyph(const struct fcft_glyph *a, const struct fcft_glyph *b)
{
	assert(a && b);
	assert(a->width == b->width && a->height == b->height);
	assert(a->x == b->x && a->y == b->y);
	assert(a->is_color_glyph == b->is_color_glyph);
	assert(pixman_image_get_format(a->pix) == pixman_image_get_format(b->pix));
	int bytes = (a->width * PIXMAN_FORMAT_BPP(pixman_image_get_format(a->pix)) + 7) / 8;
	for (int y = 0; y < a->height; y++)
		assert(!memcmp((unsigned char *)pixman_image_get_data(a->pix) + y * pixman_image_get_stride(a->pix),
		               (unsigned char *)pixman_image_get_data(b->pix) + y * pixman_image_get_stride(b->pix), bytes));
}

static void
checkprimary(void)
{
	const ushort modes[] = {0, ATTR_ITALIC, ATTR_ITALIC | ATTR_BOLD, ATTR_BOLD};
	Font *fonts[] = {&dc.font, &dc.ifont, &dc.ibfont, &dc.bfont};
	GlyphFontSpec specs[4];
	Glyph glyph = {.u = 'A'};
	for (int i = 0; i < 4; i++) {
		Font *f = fonts[i];
		glyph.mode = modes[i];
		assert(xmakeglyphfontspecs(specs, &glyph, 1, 0, 0) == 1);
		assert(f->match && f->nfonts >= 1);
		checksize(f->names[0], usedfontsize * xw.scale);
		struct fcft_font *expected = fcft_from_name(f->nfonts, (const char **)f->names, NULL);
		assert(expected);
		const struct fcft_glyph *reference = fcft_rasterize_char_utf32(expected, 'A', FCFT_SUBPIXEL_DEFAULT);
		sameglyph(specs[0].imageglyph, reference);
		assert(f->ascent == f->match->ascent && f->descent == f->match->descent);
		assert(f->height == f->ascent + f->descent);
		fcft_destroy(expected);
	}
}

static void
checklazy(void)
{
	const ushort modes[] = {ATTR_ITALIC, ATTR_ITALIC | ATTR_BOLD, ATTR_BOLD};
	Font *fonts[] = {&dc.ifont, &dc.ibfont, &dc.bfont};
	GlyphFontSpec specs[12];
	assert(dc.font.match);
	for (int i = 0; i < 3; i++) {
		assert(!fonts[i]->match && !fonts[i]->names && fonts[i]->pattern);
		Glyph text[] = {{.u = 'A'}, {.u = ' ', .mode = modes[i]},
			{.u = 0, .mode = modes[i] | ATTR_WDUMMY}};
		assert(xmakeglyphfontspecs(specs, text, LEN(text), 0, 0) == 2);
		assert(!specs[1].imageglyph);
		#if BOXDRAW_PATCH
		Glyph box = {.u = 0x2500, .mode = modes[i] | ATTR_BOXDRAW};
		assert(xmakeglyphfontspecs(specs, &box, 1, 0, 0) == 1);
		assert(specs[0].glyph == boxdrawindex(&box));
		#endif
		/* No following style is opened just to discover a word boundary. */
		for (int j = 0; j < 3; j++)
			assert(!fonts[j]->match && !fonts[j]->names);
	}
}

#if FONT2_PATCH
static void
checkspares(double ratio)
{
	Font *fonts[] = {&dc.font, &dc.ifont, &dc.ibfont, &dc.bfont};
	const double sizes[] = {10, 16, 18, 16};
	const int bold = DISABLE_BOLD_FONTS_PATCH ? FC_WEIGHT_NORMAL : FC_WEIGHT_BOLD;
	const int italic = DISABLE_ITALIC_FONTS_PATCH ? FC_SLANT_ROMAN : FC_SLANT_ITALIC;
	const int weights[] = {FC_WEIGHT_NORMAL, FC_WEIGHT_NORMAL, bold, bold};
	const int slants[] = {FC_SLANT_ROMAN, italic, italic, FC_SLANT_ROMAN};
	for (int style = 0; style < 4; style++) {
		assert(fonts[style]->nfonts == 1 + LEN(font2));
		for (size_t i = 0; i < LEN(font2); i++) {
			const char *name = fonts[style]->names[i + 1];
			FcPattern *p = FcNameParse((const FcChar8 *)name);
			int weight, slant;
			checksize(name, sizes[i] * ratio);
			assert(FcPatternGetInteger(p, FC_WEIGHT, 0, &weight) == FcResultMatch);
			assert(FcPatternGetInteger(p, FC_SLANT, 0, &slant) == FcResultMatch);
			assert(weight == weights[style] && slant == slants[style]);
			FcPatternDestroy(p);
		}
	}
}

static FcCharSet *
coverage(const char *name)
{
	FcPattern *p = FcNameParse((const FcChar8 *)name), *match;
	FcCharSet *set, *copy;
	FcResult result;
	assert(p);
	FcConfigSubstitute(NULL, p, FcMatchPattern); FcDefaultSubstitute(p);
	match = FcFontMatch(NULL, p, &result); assert(match);
	assert(FcPatternGetCharSet(match, FC_CHARSET, 0, &set) == FcResultMatch);
	copy = FcCharSetCopy(set); assert(copy);
	FcPatternDestroy(match); FcPatternDestroy(p);
	return copy;
}

static void
checkfallback(void)
{
	if (!LEN(font2)) return;
	/* Discover characters absent from primary and all earlier configured
	 * families. Compare with the expected standalone font at its own size. */
	FcCharSet *sets[3];
	for (int i = 0; i < 3; i++) sets[i] = coverage(dc.font.names[i]);
	for (int spare = 1; spare <= 2; spare++) {
		Rune rune;
		for (rune = 0x2200; rune < 0xd800; rune++) {
			int covered = 0;
			for (int i = 0; i < spare; i++) covered |= FcCharSetHasChar(sets[i], rune);
			if (!covered && FcCharSetHasChar(sets[spare], rune)) break;
		}
		assert(rune < 0xd800);
		const char *name = dc.font.names[spare];
		struct fcft_font *expected = fcft_from_name(1, &name, NULL);
		assert(expected);
		const struct fcft_glyph *want = fcft_rasterize_char_utf32(expected, rune, FCFT_SUBPIXEL_DEFAULT);
		const struct fcft_glyph *got = fcft_rasterize_char_utf32(dc.font.match, rune, FCFT_SUBPIXEL_DEFAULT);
		assert(want && got && !strcmp(want->font_name, got->font_name));
		sameglyph(got, want);
		Glyph g = {.u = rune}; GlyphFontSpec spec[4];
		assert(xmakeglyphfontspecs(spec, &g, 1, 0, 0) == 1);
		sameglyph(spec[0].imageglyph, want);
		fcft_destroy(expected);
	}
	for (int i = 0; i < 3; i++) FcCharSetDestroy(sets[i]);
}
#endif

static void
checkcacheboundary(void)
{
	int cw = win.cw, ch = win.ch, ascent = dc.font.ascent;
	Glyph text[] = {{.u = '!'}, {.u = '='}};
	GlyphFontSpec specs[8];
	assert(xmakeglyphfontspecs(specs, text, LEN(text), 0, 0) > 0);
	const struct fcft_glyph *held = specs[0].imageglyph;
	struct fcft_font *oldfont = dc.font.match;
	int loaded[] = {!!dc.font.match, !!dc.bfont.match, !!dc.ifont.match, !!dc.ibfont.match};
	assert(held);
	/* Trigger the production bounded-cache path with a small test budget.
	 * Its old font and pending specs remain live until the frame finishes. */
	for (unsigned int i = 0; i < FCFT_CACHE_MAXGLYPHS; i++)
		assert(fcftglyph(dc.font.match, 0x4e00 + i));
	assert(fcftrebuild && fcftglyphcount == FCFT_CACHE_MAXGLYPHS);
	assert(dc.font.match == oldfont && held->pix);
	assert(pixman_image_get_width(held->pix) == held->width);
	fcftcache_endframe();
	assert(loaded[0] == !!dc.font.match && loaded[1] == !!dc.bfont.match &&
		loaded[2] == !!dc.ifont.match && loaded[3] == !!dc.ibfont.match);
	assert(!fcftrebuild && !fcftglyphcount);
	assert(win.cw == cw && win.ch == ch && dc.font.ascent == ascent);
	for (size_t i = 0; i < LEN(fcftseen); i++) assert(!fcftseen[i].font);
	assert(xmakeglyphfontspecs(specs, text, LEN(text), 0, 0) > 0);
	assert(specs[0].imageglyph);
}

/* Serialized final pixels for comparison with a frozen eager-loading build.
 * Run each case cold and after a cache rebuild; include style boundaries,
 * spaces, wide glyphs, missing glyphs and font2 fallback candidates. */
static void
pixelreference(void)
{
	const ushort modes[] = {0, ATTR_BOLD, ATTR_ITALIC, ATTR_BOLD | ATTR_ITALIC};
	const Rune runes[] = {'a', ' ', '!', '=', ' ', 'f', 'i', 0x4e2d,
		0x2500, 0x03a9, 0x1f642, 0x10ffff};
	Color foreground = {.color = {0xdddd, 0xeeee, 0xffff, 0xffff}};
	for (int size = 0; size < 3; size++) {
		if (size) {
			xunloadfonts(); xw.scale = size == 1 ? 1.25 : 2;
			xloadfonts("DejaVu Sans Mono:pixelsize=16", size == 1 ? 24 : 16);
		}
		const int width = 18 * win.cw, height = 4 * win.ch;
		uint32_t *pixels = xmalloc((size_t)width * height * sizeof(*pixels));
		xw.pix = pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height,
			pixels, width * sizeof(*pixels));
		assert(xw.pix);
		for (int pass = 0; pass < 2; pass++) {
			if (pass) { fcftrebuild = 1; fcftcache_endframe(); }
			for (int i = 0; i < width * height; i++) pixels[i] = 0xff102030;
			for (int style = 0; style < 4; style++) {
				Glyph glyphs[LEN(runes)]; GlyphFontSpec specs[4 * LEN(runes)];
				for (size_t i = 0; i < LEN(runes); i++)
					glyphs[i] = (Glyph){.u = runes[i], .mode = modes[style]};
				int n = xmakeglyphfontspecs(specs, glyphs, LEN(glyphs), 0, style);
				fcftdraw(&foreground, specs, n, 0, style * win.ch, width, win.ch);
			}
			assert(fwrite(&width, sizeof width, 1, stdout) == 1);
			assert(fwrite(&height, sizeof height, 1, stdout) == 1);
			assert(fwrite(pixels, sizeof(*pixels), (size_t)width * height, stdout) == (size_t)width * height);
		}
		pixman_image_unref(xw.pix); xw.pix = NULL; free(pixels);
	}
}

int
main(int argc, char **argv)
{
	assert(FcInit()); xw.scale = 1;
	xloadfonts("DejaVu Sans Mono:pixelsize=16", 0);
	if (argc == 2 && !strcmp(argv[1], "--pixels")) {
		pixelreference(); xunloadfonts(); fcft_fini(); FcFini(); return 0;
	}
	checklazy();
	checkcacheboundary();
	checklazy();
	for (int i = 0; i < 13; i++) {
		checkprimary();
		checkcacheboundary();
		#if FONT2_PATCH
		checkspares(usedfontsize / defaultfontsize * xw.scale);
		checkfallback();
		#else
		assert(dc.font.nfonts == 1);
		#endif
		/* Unknown characters still produce the renderer's missing-glyph box. */
		Glyph missing = {.u = 0x10ffff}; GlyphFontSpec spec[4];
		assert(xmakeglyphfontspecs(spec, &missing, 1, 0, 0) == 1);
		assert(spec[0].imageglyph);
		xunloadfonts();
		assert(!dc.font.match && !dc.bfont.match && !dc.ifont.match && !dc.ibfont.match);
		if (i == 12) break;
		xw.scale = i % 3 == 0 ? 1 : i % 3 == 1 ? 1.25 : 2;
		xloadfonts("DejaVu Sans Mono:pixelsize=16", i % 2 ? 16 : 24);
		checklazy();
	}
	/* Unloading before any optional style is needed frees saved patterns. */
	for (int i = 0; i < 3; i++) {
		xloadfonts("DejaVu Sans Mono:pixelsize=16", 0);
		checklazy(); xunloadfonts();
		assert(!dc.ifont.pattern && !dc.ibfont.pattern && !dc.bfont.pattern);
	}
	fcft_fini(); FcFini();
	puts("PASS: fcft lazy styles, whitespace/box elision, primary/fallback pixels, independent sizes/styles, missing glyph, zoom/scale/reload, loaded-only frame-boundary cache rebuild");
	return 0;
}
