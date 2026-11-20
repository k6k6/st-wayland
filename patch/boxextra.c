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
drawboxextra(VecContext *cr, int x, int y, int w, int h,
             const Color *fg, const Color *bg, ushort index)
{
	Rune u = boxextrarune(index);
	VecPath *previous_path;

	if (w <= 0 || h <= 0 || !isboxextra(u))
		return;
	previous_path = vec_copy_path(cr);
	vec_save(cr);
	/* Normal text leaves a cached clip on the shared vector context. */
	vec_reset_clip(cr);
	vec_new_path(cr);
	vec_rectangle(cr, x, y, w, h);
	vec_clip(cr);
	vec_translate(cr, x, y);
	vec_set_operator(cr, VEC_OPERATOR_OVER);
	vec_set_antialias(cr, VEC_ANTIALIAS_GRAY);
	vec_set_source_rgba(cr, fg->color.red / 65535.0,
	                     fg->color.green / 65535.0,
	                     fg->color.blue / 65535.0,
	                     fg->color.alpha / 65535.0);
	vec_set_line_cap(cr, VEC_LINE_CAP_BUTT);
	vec_set_line_join(cr, VEC_LINE_JOIN_MITER);
	vec_set_miter_limit(cr, 10);
	vec_set_dash(cr, NULL, 0, 0);
	vec_set_fill_rule(cr, VEC_FILL_RULE_WINDING);
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
	vec_restore(cr);
	vec_new_path(cr);
	vec_append_path(cr, previous_path);
	vec_path_destroy(previous_path);
}
