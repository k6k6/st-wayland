/* Headless tests of the actual Wayland font loader and glyph selection. */
#define main st_program_main
#include "../wl.c"
#undef main
#include <assert.h>

static void
checksize(cairo_scaled_font_t *font, double size)
{
	cairo_matrix_t matrix;
	cairo_scaled_font_get_font_matrix(font, &matrix);
	assert(fabs(matrix.xx - size) < 1e-6);
	assert(fabs(matrix.yy - size) < 1e-6);
}

static void
checkprimary(void)
{
	const ushort modes[] = {0, ATTR_ITALIC, ATTR_ITALIC | ATTR_BOLD, ATTR_BOLD};
	Font *fonts[] = {&dc.font, &dc.ifont, &dc.ibfont, &dc.bfont};
	GlyphFontSpec specs[4];
	Glyph glyph = {.u = 'A'};

	for (int i = 0; i < 4; i++) {
		glyph.mode = modes[i];
		assert(xmakeglyphfontspecs(specs, &glyph, 1, 0, 0) == 1);
		assert(specs[0].font == fonts[i]->match);
	}
}

#if FONT2_PATCH
static void
checkspares(double ratio)
{
	const int flags[] = {FRC_NORMAL, FRC_ITALIC, FRC_ITALICBOLD, FRC_BOLD};
	const double sizes[] = {10, 16, 18, 16};
	unsigned int glyph;

	assert(frclen == 4 * (int)LEN(font2));
	for (int i = 0; i < frclen; i++) {
		assert(frc[i].flags == flags[i % 4]);
		assert(frc[i].unicodep == 0);
		checksize(frc[i].font, sizes[i / 4] * ratio);
	}
	if (LEN(font2)) {
		/* Every spare covers ASCII: the first configured font must win. */
		for (int i = 0; i < 4; i++) {
			assert(fallbackfont(&dc.font, flags[i], 'A', &glyph) == i);
			assert(glyph != 0);
		}
	}
}

static void
checkfallback(void)
{
	unsigned int glyph;
	Rune rune;
	int expected, index, before = frclen;
	GlyphFontSpec specs[4];
	Glyph g = {0};

	if (!LEN(font2))
		return;

	/* Discover a real glyph covered by a spare, but not by the primary. */
	for (rune = 0x2200; rune < 0xd800; rune++) {
		if (glyphindex(dc.font.match, rune))
			continue;
		for (expected = 0; expected < before; expected += 4)
			if (glyphindex(frc[expected].font, rune))
				break;
		if (expected < before)
			break;
	}
	assert(rune < 0xd800);
	index = fallbackfont(&dc.font, FRC_NORMAL, rune, &glyph);
	assert(index == expected && glyph != 0 && frclen == before);
	g.u = rune;
	assert(xmakeglyphfontspecs(specs, &g, 1, 0, 0) == 1);
	assert(specs[0].font == frc[expected].font);

	/* The second configured family supplies glyphs absent in the first. */
	for (rune = 0x2200; rune < 0xd800; rune++)
		if (!glyphindex(frc[0].font, rune) && glyphindex(frc[4].font, rune))
			break;
	assert(rune < 0xd800);
	assert(fallbackfont(&dc.font, FRC_NORMAL, rune, &glyph) == 4);
	assert(glyph != 0 && frclen == before);
}
#endif

int
main(void)
{
	unsigned int glyph;
	int before, index;
	assert(FcInit());
	xw.scale = 1;
	#if LIGATURES_PATCH
	hbcreatebuffer();
	#endif
	xloadfonts("DejaVu Sans Mono:pixelsize=16", 0);
	checkprimary();
	#if FONT2_PATCH
	checkspares(1);
	checkfallback();
	#else
	assert(frclen == 0);
	#endif

	/* No font covers this noncharacter; automatic fallback is cached once. */
	before = frclen;
	index = fallbackfont(&dc.font, FRC_NORMAL, 0x10ffff, &glyph);
	assert(index == before && frclen == before + 1);
	assert(fallbackfont(&dc.font, FRC_NORMAL, 0x10ffff, &glyph) == index);
	assert(frclen == before + 1);

	/* The common reload path used by zoom and output-scale changes. */
	for (int i = 0; i < 12; i++) {
		xunloadfonts();
		assert(frclen == 0);
		for (unsigned int j = 0; j < LEN(glyphcache); j++)
			assert(glyphcache[j].font == NULL);
		xw.scale = i % 3 == 0 ? 1.0 : i % 3 == 1 ? 1.25 : 2.0;
		xloadfonts("DejaVu Sans Mono:pixelsize=16", i % 2 ? 16 : 24);
		checksize(dc.font.match, usedfontsize * xw.scale);
		#if FONT2_PATCH
		checkspares(usedfontsize / defaultfontsize * xw.scale);
		#endif
		checkprimary();
	}
	xunloadfonts();
	free(frc);
	#if LIGATURES_PATCH
	hbdestroybuffer();
	#endif
	puts("PASS: primary priority, spare order/styles/sizes, fallback cache, zoom/scale/reload");
	return 0;
}
