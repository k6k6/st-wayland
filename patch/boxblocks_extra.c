/*
 * Cell-aligned blocks, mosaics and fill characters (Cairo backend).
 * MIT/X Consortium License, as with boxdraw.c.
 *
 * Geometry is specified by the Unicode character names and charts:
 *   https://www.unicode.org/charts/PDF/U1FB00.pdf
 *   https://www.unicode.org/charts/PDF/U1CC00.pdf
 *   https://www.unicode.org/charts/PDF/U25A0.pdf
 * The two speckle frames are the dot arrangements in the Unicode chart,
 * represented as coordinates, not font outlines or font rasterizations.
 * Ghostty (MIT) and VTE (LGPL-3.0-or-later) were consulted to cross-check
 * fill semantics; no implementation code from either project is included.
 *
 * The caller provides a clean path, a cell-local origin, the cell clip,
 * OVER compositing and the foreground RGBA source, and saves/restores all
 * Cairo state. Pattern holes leave the existing background untouched;
 * in particular, neither foreground nor background alpha is flattened.
 */

#include <cairo.h>
#include <stdint.h>

static int
isboxblocksextra(Rune u)
{
	return (u >= 0x1fb00 && u <= 0x1fb9f && u != 0x1fb93) ||
	       (u >= 0x1cc40 && u <= 0x1cc47) ||
	       (u >= 0x25e2 && u <= 0x25e5);
}

/* Shared, rounded boundaries, including for complementary right/lower
 * blocks. Never round the width of a band separately from its endpoints.
 * This also agrees with the original boxdraw.c half/eighth boundaries.
 */
static int
bbe_fraction(int size, int numerator, int denominator)
{
	return (int)(((int64_t)size * numerator + denominator / 2) / denominator);
}

static void
bbe_rect(cairo_t *cr, int x0, int y0, int x1, int y1)
{
	if (x1 > x0 && y1 > y0)
		cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
}

/* Clockwise perimeter vertices:
 *
 *       0-----1-----2
 *       |           |
 *       9           3       y = 1/3
 *       |           |
 *       8           4       y = 2/3
 *       |           |
 *       7-----6-----5
 *
 * Entries are the two endpoints literally named by U+1FB3C..1FB51.
 * U+1FB52..1FB67 have the same endpoints and the opposite filled side.
 */
static const unsigned char bbe_diagonals[22][2] = {
	{ 8, 6 }, /* 1FB3C: LOWER MIDDLE LEFT to LOWER CENTRE */
	{ 8, 5 }, /* 1FB3D: LOWER MIDDLE LEFT to LOWER RIGHT */
	{ 9, 6 }, /* 1FB3E: UPPER MIDDLE LEFT to LOWER CENTRE */
	{ 9, 5 }, /* 1FB3F: UPPER MIDDLE LEFT to LOWER RIGHT */
	{ 0, 6 }, /* 1FB40: UPPER LEFT to LOWER CENTRE */
	{ 9, 1 }, /* 1FB41: UPPER MIDDLE LEFT to UPPER CENTRE */
	{ 9, 2 }, /* 1FB42: UPPER MIDDLE LEFT to UPPER RIGHT */
	{ 8, 1 }, /* 1FB43: LOWER MIDDLE LEFT to UPPER CENTRE */
	{ 8, 2 }, /* 1FB44: LOWER MIDDLE LEFT to UPPER RIGHT */
	{ 7, 1 }, /* 1FB45: LOWER LEFT to UPPER CENTRE */
	{ 8, 3 }, /* 1FB46: LOWER MIDDLE LEFT to UPPER MIDDLE RIGHT */
	{ 6, 4 }, /* 1FB47: LOWER CENTRE to LOWER MIDDLE RIGHT */
	{ 7, 4 }, /* 1FB48: LOWER LEFT to LOWER MIDDLE RIGHT */
	{ 6, 3 }, /* 1FB49: LOWER CENTRE to UPPER MIDDLE RIGHT */
	{ 7, 3 }, /* 1FB4A: LOWER LEFT to UPPER MIDDLE RIGHT */
	{ 6, 2 }, /* 1FB4B: LOWER CENTRE to UPPER RIGHT */
	{ 1, 3 }, /* 1FB4C: UPPER CENTRE to UPPER MIDDLE RIGHT */
	{ 0, 3 }, /* 1FB4D: UPPER LEFT to UPPER MIDDLE RIGHT */
	{ 1, 4 }, /* 1FB4E: UPPER CENTRE to LOWER MIDDLE RIGHT */
	{ 0, 4 }, /* 1FB4F: UPPER LEFT to LOWER MIDDLE RIGHT */
	{ 1, 5 }, /* 1FB50: UPPER CENTRE to LOWER RIGHT */
	{ 9, 4 }, /* 1FB51: UPPER MIDDLE LEFT to LOWER MIDDLE RIGHT */
};

static void
bbe_diagonal(cairo_t *cr, int w, int h, Rune u)
{
	int xm = bbe_fraction(w, 1, 2);
	int y1 = bbe_fraction(h, 1, 3), y2 = bbe_fraction(h, 2, 3);
	int p[10][2] = {
		{ 0, 0 }, { xm, 0 }, { w, 0 }, { w, y1 }, { w, y2 },
		{ w, h }, { xm, h }, { 0, h }, { 0, y2 }, { 0, y1 }
	};
	int index = (int)(u - 0x1fb3c), step = 1, a, b, i;

	if (index >= 22) {
		index -= 22;
		step = 9; /* -1 modulo 10: the complementary perimeter arc */
	}
	a = bbe_diagonals[index][0];
	b = bbe_diagonals[index][1];
	cairo_move_to(cr, p[a][0], p[a][1]);
	cairo_line_to(cr, p[b][0], p[b][1]);
	for (i = (b + step) % 10; i != a; i = (i + step) % 10)
		cairo_line_to(cr, p[i][0], p[i][1]);
	cairo_close_path(cr);
}

/* Bits 0..3 are the left, upper, right and lower triangular quarters.
 * All subpaths have the same winding. A single fill computes their union,
 * without seams or double alpha at shared edges/the central vertex.
 */
static void
bbe_quarters(cairo_t *cr, int w, int h, unsigned int mask)
{
	int corners[4][2] = { { 0, h }, { 0, 0 }, { w, 0 }, { w, h } };
	int xm = bbe_fraction(w, 1, 2), ym = bbe_fraction(h, 1, 2), i;

	for (i = 0; i < 4; i++) {
		if (!(mask & (1u << i)))
			continue;
		cairo_move_to(cr, corners[i][0], corners[i][1]);
		cairo_line_to(cr, corners[(i + 1) % 4][0], corners[(i + 1) % 4][1]);
		cairo_line_to(cr, xm, ym);
		cairo_close_path(cr);
	}
}

/* Half-cell corner triangles: 0 = upper left, then clockwise. */
static void
bbe_corner(cairo_t *cr, int w, int h, unsigned int corner)
{
	int p[4][2] = { { 0, 0 }, { w, 0 }, { w, h }, { 0, h } };
	unsigned int next = (corner + 1) % 4, prev = (corner + 3) % 4;

	cairo_move_to(cr, p[corner][0], p[corner][1]);
	cairo_line_to(cr, p[next][0], p[next][1]);
	cairo_line_to(cr, p[prev][0], p[prev][1]);
	cairo_close_path(cr);
}

enum bbe_pattern {
	BBE_MEDIUM, BBE_INVERSE_MEDIUM,
	BBE_CHECKER, BBE_INVERSE_CHECKER, BBE_HEAVY_HORIZONTAL,
	BBE_DOWN_HATCH, BBE_UP_HATCH,
	BBE_SPARSE_HORIZONTAL, BBE_SPARSE_VERTICAL,
	BBE_ORTHOGONAL, BBE_DIAGONAL,
	BBE_DENSE_VERTICAL, BBE_DENSE_HORIZONTAL,
	BBE_SPECKLE1, BBE_SPECKLE2
};

/* Small device-space tiles, deliberately independent of the cell size.
 * Anchoring the mask with the CTM rather than restarting it at (0,0)
 * makes adjacent cells tile even at odd widths/heights. Medium shade is
 * a fine 1-pixel checker; checkerboard fill has 2-pixel squares. Sparse
 * rules and hatches have period 4, dense rules period 2, and heavy rules
 * occupy two rows out of four. Speckle frames have an 8x8 period.
 */
static void
bbe_pattern_fill(cairo_t *cr, enum bbe_pattern kind)
{
	static const unsigned char speckles[2][8] = {
		{ 0x11, 0x40, 0x02, 0x00, 0x11, 0x82, 0x00, 0x89 },
		{ 0x02, 0x80, 0x11, 0x00, 0x02, 0x20, 0x01, 0x88 }
	};
	unsigned char *data;
	cairo_surface_t *surface;
	cairo_pattern_t *pattern;
	cairo_matrix_t matrix;
	int x, y, on;

	/* Cairo owns the pixels: recording targets may retain the mask after
	 * this function returns, even after our surface reference is dropped.
	 */
	surface = cairo_image_surface_create(CAIRO_FORMAT_A8, 8, 8);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		return;
	}
	data = cairo_image_surface_get_data(surface);
	for (y = 0; y < 8; y++) {
		for (x = 0; x < 8; x++) {
			on = 0;
			switch (kind) {
			case BBE_MEDIUM: on = (x + y) % 2 == 0; break;
			case BBE_INVERSE_MEDIUM: on = (x + y) % 2 != 0; break;
			case BBE_CHECKER: on = (x / 2 + y / 2) % 2 == 0; break;
			case BBE_INVERSE_CHECKER: on = (x / 2 + y / 2) % 2 != 0; break;
			case BBE_HEAVY_HORIZONTAL: on = y % 4 >= 2; break;
			case BBE_DOWN_HATCH: on = x % 4 == y % 4; break;
			case BBE_UP_HATCH: on = (x + y) % 4 == 2; break;
			case BBE_SPARSE_HORIZONTAL: on = y % 4 == 1; break;
			case BBE_SPARSE_VERTICAL: on = x % 4 == 1; break;
			case BBE_ORTHOGONAL: on = x % 4 == 1 || y % 4 == 1; break;
			case BBE_DIAGONAL:
				on = x % 4 == y % 4 || (x + y) % 4 == 2;
				break;
			case BBE_DENSE_VERTICAL: on = x % 2 != 0; break;
			case BBE_DENSE_HORIZONTAL: on = y % 2 != 0; break;
			case BBE_SPECKLE1:
			case BBE_SPECKLE2:
				on = speckles[kind - BBE_SPECKLE1][y] & (1u << x);
				break;
			}
			data[y * 8 + x] = on ? 255 : 0;
		}
	}

	cairo_surface_mark_dirty(surface);
	pattern = cairo_pattern_create_for_surface(surface);
	cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
	cairo_pattern_set_filter(pattern, CAIRO_FILTER_NEAREST);
	cairo_get_matrix(cr, &matrix);
	cairo_pattern_set_matrix(pattern, &matrix);
	cairo_mask(cr, pattern);
	cairo_pattern_destroy(pattern);
	cairo_surface_destroy(surface);
}

static void
drawboxblocksextra(cairo_t *cr, int w, int h, Rune u,
                  const Color *fg, const Color *bg)
{
	static const unsigned char eighths[5] = { 2, 3, 5, 6, 7 };
	int xm, ym, i, n, x[9], y[9];
	unsigned int mask;

	/* The wrapper installed fg, including its alpha. Do not replace it
	 * with an opaque average, or repaint background into pattern holes.
	 */
	(void)fg;
	(void)bg;
	if (w <= 0 || h <= 0 || !isboxblocksextra(u))
		return;
	cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
	xm = bbe_fraction(w, 1, 2);
	ym = bbe_fraction(h, 1, 2);

	if (u >= 0x1fb00 && u <= 0x1fb3b) {
		/* Name order counts six-bit masks, omitting 0, 21, 42, 63:
		 * blank, LEFT HALF BLOCK, RIGHT HALF BLOCK, FULL BLOCK.
		 * Bits 0..5: top left/right, middle left/right, lower left/right.
		 */
		mask = u - 0x1fb00 + 1;
		if (mask >= 21) mask++;
		if (mask >= 42) mask++;
		for (i = 0; i < 6; i++) {
			if (mask & (1u << i))
				bbe_rect(cr, i % 2 ? xm : 0,
				         bbe_fraction(h, i / 2, 3),
				         i % 2 ? w : xm,
				         bbe_fraction(h, i / 2 + 1, 3));
		}
		cairo_fill(cr);
		return;
	}
	if (u >= 0x1fb3c && u <= 0x1fb67) {
		bbe_diagonal(cr, w, h, u);
		cairo_fill(cr);
		return;
	}
	if (u >= 0x1fb68 && u <= 0x1fb6f) {
		mask = 1u << ((u - 0x1fb68) % 4);
		if (u <= 0x1fb6b) mask ^= 15;
		bbe_quarters(cr, w, h, mask);
		cairo_fill(cr);
		return;
	}
	if (u >= 0x1fb70 && u <= 0x1fb8b) {
		for (i = 0; i <= 8; i++) {
			x[i] = bbe_fraction(w, i, 8);
			y[i] = bbe_fraction(h, i, 8);
		}
		if (u <= 0x1fb75) {
			n = (int)(u - 0x1fb70) + 1;
			bbe_rect(cr, x[n], 0, x[n + 1], h);
		} else if (u <= 0x1fb7b) {
			n = (int)(u - 0x1fb76) + 1;
			bbe_rect(cr, 0, y[n], w, y[n + 1]);
		} else if (u <= 0x1fb7f) {
			if (u == 0x1fb7c || u == 0x1fb7d)
				bbe_rect(cr, 0, 0, x[1], h);
			else
				bbe_rect(cr, x[7], 0, w, h);
			if (u == 0x1fb7d || u == 0x1fb7e)
				bbe_rect(cr, 0, 0, w, y[1]);
			else
				bbe_rect(cr, 0, y[7], w, h);
		} else if (u <= 0x1fb81) {
			mask = u == 0x1fb80 ? 0x81 : 0x95; /* 18 / 1358 */
			for (i = 0; i < 8; i++)
				if (mask & (1u << i))
					bbe_rect(cr, 0, y[i], w, y[i + 1]);
		} else if (u <= 0x1fb86) {
			bbe_rect(cr, 0, 0, w, y[eighths[u - 0x1fb82]]);
		} else {
			bbe_rect(cr, x[8 - eighths[u - 0x1fb87]], 0, w, h);
		}
		cairo_fill(cr);
		return;
	}
	if (u >= 0x1fb8c && u <= 0x1fb94) {
		switch (u) {
		case 0x1fb8c: bbe_rect(cr, 0, 0, xm, h); break;
		case 0x1fb8d: bbe_rect(cr, xm, 0, w, h); break;
		case 0x1fb8e: bbe_rect(cr, 0, 0, w, ym); break;
		case 0x1fb8f: bbe_rect(cr, 0, ym, w, h); break;
		case 0x1fb90: bbe_rect(cr, 0, 0, w, h); break;
		case 0x1fb91:
			bbe_rect(cr, 0, 0, w, ym);
			cairo_fill(cr);
			bbe_rect(cr, 0, ym, w, h);
			break;
		case 0x1fb92:
			bbe_rect(cr, 0, ym, w, h);
			cairo_fill(cr);
			bbe_rect(cr, 0, 0, w, ym);
			break;
		case 0x1fb94:
			bbe_rect(cr, xm, 0, w, h);
			cairo_fill(cr);
			bbe_rect(cr, 0, 0, xm, h);
			break;
		}
		cairo_clip(cr);
		bbe_pattern_fill(cr, u < 0x1fb90 ? BBE_MEDIUM : BBE_INVERSE_MEDIUM);
		return;
	}
	if (u >= 0x1fb95 && u <= 0x1fb99) {
		bbe_pattern_fill(cr, (enum bbe_pattern)(BBE_CHECKER + u - 0x1fb95));
		return;
	}
	if (u == 0x1fb9a || u == 0x1fb9b) {
		bbe_quarters(cr, w, h, u == 0x1fb9a ? 10 : 5);
		cairo_fill(cr);
		return;
	}
	if (u >= 0x1fb9c && u <= 0x1fb9f) {
		bbe_corner(cr, w, h, u - 0x1fb9c);
		cairo_clip(cr);
		bbe_pattern_fill(cr, BBE_MEDIUM);
		return;
	}
	if (u >= 0x1cc40 && u <= 0x1cc47) {
		bbe_pattern_fill(cr, (enum bbe_pattern)(BBE_SPARSE_HORIZONTAL + u - 0x1cc40));
		return;
	}
	if (u >= 0x25e2 && u <= 0x25e5) {
		bbe_corner(cr, w, h, (u - 0x25e2 + 2) % 4);
		cairo_fill(cr);
	}
}
