/* Cell-local geometric symbols beyond the original U+25xx box table.
 * Included by boxdraw.c; the individual renderers never consult fonts.
 */
#include "boxlines_extra.c"
#include "boxblocks_extra.c"
#include "boxcircles.c"
#include "boxmosaics.c"
#include "boxbranch.c"

static const Rune boxextra_pages[] = {
	0x2500, 0x2b00, 0x1fb00, 0x1cc00, 0x1ce00, 0x1f700, 0x1f900,
	0x1cd00, 0xf500, 0xf600
};

static int
isboxextra(Rune u)
{
	/* This predicate is also called while writing ordinary text cells. */
	if (u < 0x2500)
		return 0;
	switch (u & ~0xffu) {
	case 0x2500: case 0x2b00: case 0x1fb00: case 0x1cc00:
	case 0x1ce00: case 0x1f700: case 0x1f900: case 0x1cd00:
		return isboxlinesextra(u) || isboxblocksextra(u) || isboxcircle(u) ||
		       isboxmosaic(u);
	case 0xf500: case 0xf600:
		return boxdraw_branch && isboxbranch(u);
	default:
		return 0;
	}
}

/* Keep the existing 16-bit boxdraw API: 4 page bits + 8 codepoint bits.
 * Bit 11 extends the page field without changing existing indexes; BDX
 * distinguishes this layout before the old category bits are tested.
 * Supplementary-plane symbols must never be truncated to their low byte.
 */
static ushort
boxextraindex(Rune u)
{
	for (unsigned int i = 0; i < sizeof(boxextra_pages) / sizeof(*boxextra_pages); i++)
		if ((u & ~0xffu) == boxextra_pages[i])
			return BDX | (i << 8) | (u & 0xff);
	return 0;
}

static Rune
boxextrarune(ushort index)
{
	unsigned int page = (index >> 8) & 15;
	if (page >= sizeof(boxextra_pages) / sizeof(*boxextra_pages))
		return 0;
	return boxextra_pages[page] | (index & 0xff);
}

static void
drawboxextra(cairo_t *cr, int x, int y, int w, int h,
             const Color *fg, const Color *bg, ushort index)
{
	Rune u = boxextrarune(index);
	cairo_path_t *previous_path;

	if (w <= 0 || h <= 0 || !isboxextra(u))
		return;
	previous_path = cairo_copy_path(cr);
	cairo_save(cr);
	/* Normal text leaves a cached clip on the shared Cairo context. */
	cairo_reset_clip(cr);
	cairo_new_path(cr);
	cairo_rectangle(cr, x, y, w, h);
	cairo_clip(cr);
	cairo_translate(cr, x, y);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_GRAY);
	cairo_set_source_rgba(cr, fg->color.red / 65535.0,
	                     fg->color.green / 65535.0,
	                     fg->color.blue / 65535.0,
	                     fg->color.alpha / 65535.0);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
	cairo_set_miter_limit(cr, 10);
	cairo_set_dash(cr, NULL, 0, 0);
	cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
	if (isboxlinesextra(u))
		drawboxlinesextra(cr, w, h, u, !!(index & BDB));
	else if (isboxblocksextra(u))
		drawboxblocksextra(cr, w, h, u, fg, bg);
	else if (isboxmosaic(u))
		drawboxmosaic(cr, w, h, u, !!(index & BDB));
	else if (isboxbranch(u))
		drawboxbranch(cr, w, h, u, !!(index & BDB));
	else
		drawboxcircle(cr, w, h, u);
	cairo_restore(cr);
	cairo_new_path(cr);
	cairo_append_path(cr, previous_path);
	cairo_path_destroy(previous_path);
}
