/* Additional Unicode mosaics and outlined/negative geometry.
 * MIT/X Consortium License. Original Cairo implementation using Unicode
 * character names; codepoint semantics cross-checked against Ghostty.
 * Included after boxlines_extra.c and boxblocks_extra.c. The caller owns
 * cell clipping, foreground RGBA, translation and Cairo state preservation.
 */
static int
isboxmosaic(Rune u)
{
	return (u >= 0x25f8 && u <= 0x25fa) || u == 0x25ff ||
	       (u >= 0x1cd00 && u <= 0x1cde5) ||
	       (u >= 0x1cc21 && u <= 0x1cc2f) ||
	       (u >= 0x1ce51 && u <= 0x1ceaf) ||
	       (u >= 0x1fbbd && u <= 0x1fbbf) ||
	       u == 0x1fbce || u == 0x1fbcf ||
	       (u >= 0x1fbe4 && u <= 0x1fbe7);
}

/* Unicode BLOCK OCTANT names, in codepoint order. Bits are row-major:
 * 1 2 / 3 4 / 5 6 / 7 8. Existing block characters account for the masks
 * omitted from this range. This is data, not an assumption of cp == mask.
 */
static const unsigned char box_octants[230] = {
	0x04, 0x06, 0x07, 0x08, 0x09, 0x0b, 0x0c, 0x0d, 0x0e, 0x10, 0x11, 0x12, 0x13, 0x15, 0x16, 0x17,
	0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
	0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
	0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a,
	0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x51, 0x52, 0x53, 0x54, 0x56, 0x57, 0x58, 0x59, 0x5b, 0x5c, 0x5d,
	0x5e, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e,
	0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e,
	0x7f, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
	0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
	0xa1, 0xa2, 0xa3, 0xa4, 0xa6, 0xa7, 0xa8, 0xa9, 0xab, 0xac, 0xad, 0xae, 0xb0, 0xb1, 0xb2, 0xb3,
	0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 0xc1, 0xc2, 0xc3, 0xc4,
	0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, 0xd0, 0xd1, 0xd2, 0xd3, 0xd4,
	0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf, 0xe0, 0xe1, 0xe2, 0xe3, 0xe4,
	0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef, 0xf1, 0xf2, 0xf3, 0xf4, 0xf6,
	0xf7, 0xf8, 0xf9, 0xfb, 0xfd, 0xfe,
};

static void
boxmosaic_grid(cairo_t *cr, int w, int h, int rows, unsigned mask, int separated)
{
	int m = w < h ? w : h;
	int gap = separated && w >= 6 && h >= rows*3 ? (m/12 > 0 ? m/12 : 1) : 0;
	/* At tiny sizes drop the separation rather than making negative boxes.
	 * Rounding shared grid boundaries keeps adjacent fractions consistent. */
	for (int i = 0; i < rows*2; i++) if (mask & (1u << i)) {
		int x0 = bbe_fraction(w, i%2, 2), x1 = bbe_fraction(w, i%2+1, 2);
		int y0 = bbe_fraction(h, i/2, rows), y1 = bbe_fraction(h, i/2+1, rows);
		bbe_rect(cr, x0+gap, y0+gap, x1-gap, y1-gap);
	}
	cairo_fill(cr);
}

static void
boxmosaic_negative(cairo_t *cr, int w, int h, Rune u, int bold)
{
	static const Rune holes[] = {0x2573, 0x1fba3, 0x1fbae};
	cairo_pattern_t *mask;
	/* Invert coverage inside a temporary alpha group, NOT the terminal
	 * background. Apply the saved foreground once, retaining its alpha. */
	cairo_save(cr);
	cairo_push_group_with_content(cr, CAIRO_CONTENT_ALPHA);
	cairo_set_source_rgba(cr, 1, 1, 1, 1);
	cairo_rectangle(cr, 0, 0, w, h);
	cairo_fill(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_DEST_OUT);
	drawboxlinesextra(cr, w, h, holes[u-0x1fbbd], bold);
	mask = cairo_pop_group(cr);
	cairo_restore(cr);
	cairo_mask(cr, mask);
	cairo_pattern_destroy(mask);
}

static void
drawboxmosaic(cairo_t *cr, int w, int h, Rune u, int bold)
{
	/* Quarter-grid rectangles: x0,y0,x1,y1. U+1CEA0..AF are partial
	 * strips, NOT all sixteen possible single-sixteenth positions. */
	static const unsigned char strips[16][4] = {
		{2,3,4,4}, {1,3,4,4}, {0,3,3,4}, {0,3,2,4},
		{0,2,1,4}, {0,1,1,4}, {0,0,1,3}, {0,0,1,2},
		{0,0,2,1}, {0,0,3,1}, {1,0,4,1}, {2,0,4,1},
		{3,0,4,2}, {3,0,4,3}, {3,1,4,4}, {3,2,4,4}
	};
	static const unsigned char centres[4][4] = {
		{1,0,3,2}, {1,2,3,4}, {0,1,2,3}, {2,1,4,3}
	};
	int i, x0, x1;
	const unsigned char *r;
	if (w <= 0 || h <= 0 || !isboxmosaic(u)) return;
	cairo_new_path(cr);
	if (u >= 0x1cd00 && u <= 0x1cde5) {
		boxmosaic_grid(cr, w, h, 4, box_octants[u-0x1cd00], 0);
		return;
	}
	if (u >= 0x1cc21 && u <= 0x1cc2f) {
		boxmosaic_grid(cr, w, h, 2, u-0x1cc20, 1);
		return;
	}
	if (u >= 0x1ce51 && u <= 0x1ce8f) {
		boxmosaic_grid(cr, w, h, 3, u-0x1ce50, 1);
		return;
	}
	if (u >= 0x1fbbd && u <= 0x1fbbf) {
		boxmosaic_negative(cr, w, h, u, bold);
		return;
	}
	if (u == 0x1fbce || u == 0x1fbcf) {
		bbe_rect(cr, 0, 0, bbe_fraction(w, u == 0x1fbce ? 2 : 1, 3), h);
	} else if (u >= 0x1ce90 && u <= 0x1ce9f) {
		i = u-0x1ce90;
		bbe_rect(cr, bbe_fraction(w, i%4, 4), bbe_fraction(h, i/4, 4),
		         bbe_fraction(w, i%4+1, 4), bbe_fraction(h, i/4+1, 4));
	} else if ((u >= 0x1cea0 && u <= 0x1ceaf) || (u >= 0x1fbe4 && u <= 0x1fbe7)) {
		r = u >= 0x1fbe4 ? centres[u-0x1fbe4] : strips[u-0x1cea0];
		bbe_rect(cr, bbe_fraction(w, r[0], 4), bbe_fraction(h, r[1], 4),
		         bbe_fraction(w, r[2], 4), bbe_fraction(h, r[3], 4));
	} else {
		/* Outline triangle orthogonal edges lie entirely inside the cell.
		 * Border centres are inset half a stem, avoiding clipped half-lines. */
		int m = w < h ? w : h;
		double s = fmax(1, round(m/8.0));
		if (bold && m >= 6) s = fmax(s+1, round(s*1.5));
		double p[4][2] = {{s/2,s/2}, {w-s/2,s/2}, {w-s/2,h-s/2}, {s/2,h-s/2}};
		i = u == 0x25f8 ? 0 : u == 0x25f9 ? 1 : u == 0x25fa ? 3 : 2;
		x0 = (i+1)%4; x1 = (i+3)%4;
		cairo_move_to(cr, p[i][0], p[i][1]);
		cairo_line_to(cr, p[x0][0], p[x0][1]);
		cairo_line_to(cr, p[x1][0], p[x1][1]);
		cairo_close_path(cr);
		cairo_set_line_width(cr, s);
		cairo_stroke(cr);
		return;
	}
	cairo_fill(cr);
}
