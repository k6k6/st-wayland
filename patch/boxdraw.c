/*
 * Copyright 2018 Avi Halachmi (:avih) avihpit@yahoo.com https://github.com/avih
 * MIT/X Consortium License
 */

#include <cairo.h>

/* Rounded non-negative integers division of n / d  */
#define DIV(n, d) (((n) + (d) / 2) / (d))

static cairo_t *bdcr;

static void drawbox(int, int, int, int, Color *, Color *, ushort);
static void drawboxlines(int, int, int, int, Color *, ushort);

/* fill a rectangle with a solid colour (source operator, like XftDrawRect) */
static void
bdrect(Color *c, int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0)
		return;
	/* direct pixel writes, much cheaper than going through cairo */
	wlfillrect(c, x, y, w, h);
}

/* public API */

void
boxdraw_xinit(cairo_t *cr)
{
	bdcr = cr;
}

int
isboxdraw(Rune u)
{
	Rune block = u & ~0xff;
	return (boxdraw && block == 0x2500 && boxdata[(uint8_t)u]) ||
	       (boxdraw_braille && block == 0x2800);
}

/* the "index" is actually the entire shape data encoded as ushort */
ushort
boxdrawindex(const Glyph *g)
{
	if (boxdraw_braille && (g->u & ~0xff) == 0x2800)
		return BRL | (uint8_t)g->u;
	if (boxdraw_bold && (g->mode & ATTR_BOLD))
		return BDB | boxdata[(uint8_t)g->u];
	return boxdata[(uint8_t)g->u];
}

void
drawboxes(int x, int y, int cw, int ch, Color *fg, Color *bg,
          const GlyphFontSpec *specs, int len)
{
	for ( ; len-- > 0; x += cw, specs++)
		drawbox(x, y, cw, ch, fg, bg, (ushort)specs->glyph);
}

/* implementation */

void
drawbox(int x, int y, int w, int h, Color *fg, Color *bg, ushort bd)
{
	ushort cat = bd & ~(BDB | 0xff);  /* mask out bold and data */
	if (bd & (BDL | BDA)) {
		/* lines (light/double/heavy/arcs) */
		drawboxlines(x, y, w, h, fg, bd);

	} else if (cat == BBD) {
		/* lower (8-X)/8 block */
		int d = DIV((uint8_t)bd * h, 8);
		bdrect(fg, x, y + d, w, h - d);

	} else if (cat == BBU) {
		/* upper X/8 block */
		bdrect(fg, x, y, w, DIV((uint8_t)bd * h, 8));

	} else if (cat == BBL) {
		/* left X/8 block */
		bdrect(fg, x, y, DIV((uint8_t)bd * w, 8), h);

	} else if (cat == BBR) {
		/* right (8-X)/8 block */
		int d = DIV((uint8_t)bd * w, 8);
		bdrect(fg, x + d, y, w - d, h);

	} else if (cat == BBQ) {
		/* Quadrants */
		int w2 = DIV(w, 2), h2 = DIV(h, 2);
		if (bd & TL)
			bdrect(fg, x, y, w2, h2);
		if (bd & TR)
			bdrect(fg, x + w2, y, w - w2, h2);
		if (bd & BL)
			bdrect(fg, x, y + h2, w2, h - h2);
		if (bd & BR)
			bdrect(fg, x + w2, y + h2, w - w2, h - h2);

	} else if (bd & BBS) {
		/* Shades - data is 1/2/3 for 25%/50%/75% alpha, respectively */
		int d = (uint8_t)bd;
		Color xfc = { .color = { .alpha = 0xffff } };

		xfc.color.red = DIV(fg->color.red * d + bg->color.red * (4 - d), 4);
		xfc.color.green = DIV(fg->color.green * d + bg->color.green * (4 - d), 4);
		xfc.color.blue = DIV(fg->color.blue * d + bg->color.blue * (4 - d), 4);

		bdrect(&xfc, x, y, w, h);

	} else if (cat == BRL) {
		/* braille, each data bit corresponds to one dot at 2x4 grid */
		int w1 = DIV(w, 2);
		int h1 = DIV(h, 4), h2 = DIV(h, 2), h3 = DIV(3 * h, 4);

		if (bd & 1)   bdrect(fg, x, y, w1, h1);
		if (bd & 2)   bdrect(fg, x, y + h1, w1, h2 - h1);
		if (bd & 4)   bdrect(fg, x, y + h2, w1, h3 - h2);
		if (bd & 8)   bdrect(fg, x + w1, y, w - w1, h1);
		if (bd & 16)  bdrect(fg, x + w1, y + h1, w - w1, h2 - h1);
		if (bd & 32)  bdrect(fg, x + w1, y + h2, w - w1, h3 - h2);
		if (bd & 64)  bdrect(fg, x, y + h3, w1, h - h3);
		if (bd & 128) bdrect(fg, x + w1, y + h3, w - w1, h - h3);

	}
}

void
drawboxlines(int x, int y, int w, int h, Color *fg, ushort bd)
{
	/* s: stem thickness. width/8 roughly matches underscore thickness. */
	/* We draw bold as 1.5 * normal-stem and at least 1px thicker.      */
	/* doubles draw at least 3px, even when w or h < 3. bold needs 6px. */
	int mwh = MIN(w, h);
	int base_s = MAX(1, DIV(mwh, 8));
	int bold = (bd & BDB) && mwh >= 6;  /* possibly ignore boldness */
	int s = bold ? MAX(base_s + 1, DIV(3 * base_s, 2)) : base_s;
	int w2 = DIV(w - s, 2), h2 = DIV(h - s, 2);
	/* the s-by-s square (x + w2, y + h2, s, s) is the center texel.    */
	/* The base length (per direction till edge) includes this square.  */

	int light = bd & (LL | LU | LR | LD);
	int double_ = bd & (DL | DU | DR | DD);

	if (light) {
		/* d: additional (negative) length to not-draw the center   */
		/* texel - at arcs and avoid drawing inside (some) doubles  */
		int arc = bd & BDA;
		int multi_light = light & (light - 1);
		int multi_double = double_ & (double_ - 1);
		/* light crosses double only at DH+LV, DV+LH (ref. shapes)  */
		int d = arc || (multi_double && !multi_light) ? -s : 0;

		if (bd & LL)
			bdrect(fg, x, y + h2, w2 + s + d, s);
		if (bd & LU)
			bdrect(fg, x + w2, y, s, h2 + s + d);
		if (bd & LR)
			bdrect(fg, x + w2 - d, y + h2, w - w2 + d, s);
		if (bd & LD)
			bdrect(fg, x + w2, y + h2 - d, s, h - h2 + d);
	}

	/* double lines - also align with light to form heavy when combined */
	if (double_) {
		/*
		* going clockwise, for each double-ray: p is additional length
		* to the single-ray nearer to the previous direction, and n to
		* the next. p and n adjust from the base length to lengths
		* which consider other doubles - shorter to avoid intersections
		* (p, n), or longer to draw the far-corner texel (n).
		*/
		int dl = bd & DL, du = bd & DU, dr = bd & DR, dd = bd & DD;
		if (dl) {
			int p = dd ? -s : 0, n = du ? -s : dd ? s : 0;
			bdrect(fg, x, y + h2 + s, w2 + s + p, s);
			bdrect(fg, x, y + h2 - s, w2 + s + n, s);
		}
		if (du) {
			int p = dl ? -s : 0, n = dr ? -s : dl ? s : 0;
			bdrect(fg, x + w2 - s, y, s, h2 + s + p);
			bdrect(fg, x + w2 + s, y, s, h2 + s + n);
		}
		if (dr) {
			int p = du ? -s : 0, n = dd ? -s : du ? s : 0;
			bdrect(fg, x + w2 - p, y + h2 - s, w - w2 + p, s);
			bdrect(fg, x + w2 - n, y + h2 + s, w - w2 + n, s);
		}
		if (dd) {
			int p = dr ? -s : 0, n = dl ? -s : dr ? s : 0;
			bdrect(fg, x + w2 + s, y + h2 - p, s, h - h2 + p);
			bdrect(fg, x + w2 - s, y + h2 - n, s, h - h2 + n);
		}
	}
}