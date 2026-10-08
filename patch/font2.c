int
xloadsparefont(FcPattern *pattern, int flags)
{
	Font spare = {0};

	/* Use the same Fontconfig defaults and output scale as the primary font. */
	if (xloadfont(&spare, pattern))
		return 1;

	/* The fallback cache owns the scaled font, but needs no search pattern. */
	frc[frclen].font = spare.match;
	FcPatternDestroy(spare.pattern);
	frc[frclen].flags = flags;
	frc[frclen].unicodep = 0;
	frclen++;

	return 0;
}

void
xloadsparefonts(void)
{
	FcPattern *pattern;
	double fontval, dpi;
	int fc;
	char **fp;

	if (frclen != 0)
		die("can't embed spare fonts. cache isn't empty\n");

	fc = LEN(font2);
	if (fc == 0)
		return;

	/* Reserve four styles per font, before any automatic fallback entries. */
	if (frccap < 4 * fc) {
		frccap = 4 * fc;
		frc = xrealloc(frc, frccap * sizeof(Fontcache));
	}

	for (fp = font2; fp - font2 < fc; ++fp) {
		/* Wayland uses Fontconfig names, not X11 XLFD names. */
		pattern = FcNameParse((const FcChar8 *)*fp);
		if (!pattern)
			die("can't open spare font %s\n", *fp);

		/* Convert points to logical pixels before xloadfont applies scale.
		 * Fontconfig's default for an unspecified size is 12 points. */
		if (FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &fontval) != FcResultMatch) {
			if (FcPatternGetDouble(pattern, FC_SIZE, 0, &fontval) != FcResultMatch)
				fontval = 12;
			if (FcPatternGetDouble(pattern, FC_DPI, 0, &dpi) != FcResultMatch)
				dpi = 96.0;
			fontval *= dpi / 72.0;
		}
		if (defaultfontsize > 0)
			fontval *= usedfontsize / defaultfontsize;
		FcPatternDel(pattern, FC_PIXEL_SIZE);
		FcPatternDel(pattern, FC_SIZE);
		FcPatternAddDouble(pattern, FC_PIXEL_SIZE, fontval);
		FcPatternAddBool(pattern, FC_SCALABLE, FcTrue);

		if (xloadsparefont(pattern, FRC_NORMAL))
			die("can't open spare font %s\n", *fp);

		FcPatternDel(pattern, FC_SLANT);
		#if !DISABLE_ITALIC_FONTS_PATCH
		FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ITALIC);
		#endif
		if (xloadsparefont(pattern, FRC_ITALIC))
			die("can't open spare font %s\n", *fp);

		FcPatternDel(pattern, FC_WEIGHT);
		#if !DISABLE_BOLD_FONTS_PATCH
		FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_BOLD);
		#endif
		if (xloadsparefont(pattern, FRC_ITALICBOLD))
			die("can't open spare font %s\n", *fp);

		FcPatternDel(pattern, FC_SLANT);
		#if !DISABLE_ROMAN_FONTS_PATCH
		FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
		#endif
		if (xloadsparefont(pattern, FRC_BOLD))
			die("can't open spare font %s\n", *fp);

		FcPatternDestroy(pattern);
	}
}
