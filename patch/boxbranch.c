/* Git-style branch drawing PUA U+F5D0..F60D.
 * MIT/X Consortium License. Original Cairo renderer for the character
 * convention documented by Kitty PRs 7681/7805 and Ghostty branch.zig.
 * These are PUA conventions, not standard Unicode character meanings.
 * All overlapping pieces first form one alpha mask, so translucent text
 * is applied once and hollow nodes leave the existing background intact.
 */
static int
isboxbranch(Rune u)
{
	return u >= 0xf5d0 && u <= 0xf60d;
}

static void
drawboxbranch(cairo_t *cr, int w, int h, Rune u, int bold)
{
	/* Corner flags: right+down, left+down, right+up, left+up;
	 * extra bits 16/32 add a full horizontal/vertical line. */
	static const unsigned char turns[24] = {
		1, 2, 4, 8, 36, 33, 5, 40, 34, 10, 18, 17,
		3, 24, 20, 12, 44, 35, 26, 21, 41, 38, 25, 22
	};
	static const Rune corners[4] = {0x256d, 0x256e, 0x2570, 0x256f};
	/* Node edge flags: up=1, right=2, down=4, left=8. Each entry
	 * is followed by its hollow variant at the next codepoint. */
	static const unsigned char nodes[16] = {
		0, 2, 8, 10, 4, 1, 5, 6, 12, 3, 9, 7, 13, 14, 11, 15
	};
	cairo_pattern_t *mask, *gradient;
	int m, bits, i;
	double s, cx, cy, r;
	if (w <= 0 || h <= 0 || !isboxbranch(u)) return;
	m = w < h ? w : h;
	s = fmax(1, round(m/8.0));
	if (bold && m >= 6) s = fmax(s+1, round(1.5*s));
	/* Use the original st box-rule pixel centres, not w/2 and h/2. */
	cx = ceil((w-s)/2) + s/2;
	cy = ceil((h-s)/2) + s/2;
	cairo_save(cr);
	cairo_push_group_with_content(cr, CAIRO_CONTENT_ALPHA);
	cairo_set_source_rgba(cr, 1, 1, 1, 1);
	cairo_new_path(cr);
	if (u <= 0xf5d1) {
		if (u == 0xf5d0) cairo_rectangle(cr, 0, cy-s/2, w, s);
		else cairo_rectangle(cr, cx-s/2, 0, s, h);
		cairo_fill(cr);
	} else if (u <= 0xf5d5) {
		int vertical = u >= 0xf5d4;
		int reverse = (u & 1) != 0;
		int length = vertical ? h : w;
		if (length > 1) {
			gradient = vertical ? cairo_pattern_create_linear(0, .5, 0, h-.5) :
			                      cairo_pattern_create_linear(.5, 0, w-.5, 0);
			cairo_pattern_add_color_stop_rgba(gradient, 0, 1, 1, 1, reverse ? 0 : 1);
			cairo_pattern_add_color_stop_rgba(gradient, 1, 1, 1, 1, reverse ? 1 : 0);
			cairo_set_source(cr, gradient);
			cairo_pattern_destroy(gradient);
		}
		if (vertical) cairo_rectangle(cr, cx-s/2, 0, s, h);
		else cairo_rectangle(cr, 0, cy-s/2, w, s);
		cairo_fill(cr);
	} else if (u <= 0xf5ed) {
		bits = turns[u-0xf5d6];
		if (bits & 16) cairo_rectangle(cr, 0, cy-s/2, w, s);
		if (bits & 32) cairo_rectangle(cr, cx-s/2, 0, s, h);
		cairo_fill(cr);
		for (i = 0; i < 4; i++) if (bits & (1 << i))
			drawboxlinesextra(cr, w, h, corners[i], bold);
	} else {
		bits = nodes[(u-0xf5ee)/2];
		r = fmin(fmin(cx, w-cx), fmin(cy, h-cy));
		if (bits & 1) cairo_rectangle(cr, cx-s/2, 0, s, cy);
		if (bits & 2) cairo_rectangle(cr, cx, cy-s/2, w-cx, s);
		if (bits & 4) cairo_rectangle(cr, cx-s/2, cy, s, h-cy);
		if (bits & 8) cairo_rectangle(cr, 0, cy-s/2, cx, s);
		cairo_new_sub_path(cr);
		cairo_arc(cr, cx, cy, r, 0, 2*M_PI);
		cairo_fill(cr);
		if ((u & 1) && r > s) {
			/* Remove the hole from coverage only. This also prevents any
			 * connector extending across the inside of a hollow node. */
			cairo_set_operator(cr, CAIRO_OPERATOR_DEST_OUT);
			cairo_arc(cr, cx, cy, r-s, 0, 2*M_PI);
			cairo_fill(cr);
		}
	}
	mask = cairo_pop_group(cr);
	cairo_restore(cr);
	cairo_mask(cr, mask);
	cairo_pattern_destroy(mask);
}
