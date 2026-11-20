/* Cell-aligned geometric Powerline symbols; included by boxdraw.c.
 * Rune and Color come from st.h. No font metrics or background painting.
 */
#include "vec.h"

static int
ispowerline(Rune u)
{
	return (u >= 0xe0b0 && u <= 0xe0bf) || u == 0xe0d2 || u == 0xe0d4;
}

static void
drawpowerline(VecContext *cr, int x, int y, int w, int h,
              const Color *fg, Rune u)
{
	const double arc = 0.5522847498307936; /* quarter-circle Bezier coefficient */
	double width = w, height = h, mid = height / 2.0;
	double thin, gap;
	int outline;
	VecPath *previous_path;

	if (w <= 0 || h <= 0 || !ispowerline(u))
		return;

	thin = (w < h ? width : height) / 8.0;
	if (thin < 1.0)
		thin = 1.0;
	outline = u >= 0xe0b0 && u <= 0xe0bf && (u & 1);

	/* vector save/restore does not preserve the current path. */
	previous_path = vec_copy_path(cr);
	vec_save(cr);
	/* Text rendering leaves a cached clip behind; geometry owns its cell. */
	vec_reset_clip(cr);
	vec_new_path(cr);
	vec_rectangle(cr, x, y, width, height);
	vec_clip(cr);
	vec_translate(cr, x, y);
	vec_set_operator(cr, VEC_OPERATOR_OVER);
	vec_set_antialias(cr, VEC_ANTIALIAS_GRAY);
	vec_set_source_rgba(cr, fg->color.red / 65535.0,
	                     fg->color.green / 65535.0,
	                     fg->color.blue / 65535.0,
	                     fg->color.alpha / 65535.0);
	vec_set_line_width(cr, thin);
	vec_set_line_cap(cr, VEC_LINE_CAP_BUTT);
	vec_set_line_join(cr, VEC_LINE_JOIN_MITER);
	vec_set_miter_limit(cr, 10.0);
	vec_set_dash(cr, NULL, 0, 0);
	vec_set_fill_rule(cr, VEC_FILL_RULE_WINDING);

	/* Left-pointing arrows/caps and D4 are exact horizontal mirrors. */
	if (u == 0xe0b2 || u == 0xe0b3 || u == 0xe0b6 ||
	    u == 0xe0b7 || u == 0xe0d4) {
		vec_translate(cr, width, 0);
		vec_scale(cr, -1, 1);
	}

	switch (u) {
	case 0xe0b0: case 0xe0b1: case 0xe0b2: case 0xe0b3:
		/* Full-height base, with the point at the opposite cell edge. */
		vec_move_to(cr, 0, 0);
		vec_line_to(cr, width, mid);
		vec_line_to(cr, 0, height);
		break;
	case 0xe0b4: case 0xe0b5: case 0xe0b6: case 0xe0b7:
		/* Half ellipse: use both actual cell dimensions, even when odd. */
		vec_move_to(cr, 0, 0);
		vec_curve_to(cr, arc * width, 0, width, mid * (1 - arc),
		               width, mid);
		vec_curve_to(cr, width, mid * (1 + arc), arc * width, height,
		               0, height);
		break;
	case 0xe0b8: /* lower-left */
		vec_move_to(cr, 0, 0);
		vec_line_to(cr, width, height);
		vec_line_to(cr, 0, height);
		break;
	case 0xe0ba: /* lower-right */
		vec_move_to(cr, width, 0);
		vec_line_to(cr, width, height);
		vec_line_to(cr, 0, height);
		break;
	case 0xe0bc: /* upper-left */
		vec_move_to(cr, 0, 0);
		vec_line_to(cr, width, 0);
		vec_line_to(cr, 0, height);
		break;
	case 0xe0be: /* upper-right */
		vec_move_to(cr, 0, 0);
		vec_line_to(cr, width, 0);
		vec_line_to(cr, width, height);
		break;
	case 0xe0b9: case 0xe0bf: /* descending diagonal only */
		vec_move_to(cr, 0, 0);
		vec_line_to(cr, width, height);
		break;
	case 0xe0bb: case 0xe0bd: /* ascending diagonal only */
		vec_move_to(cr, width, 0);
		vec_line_to(cr, 0, height);
		break;
	case 0xe0d2: case 0xe0d4:
		/* Opposed top/bottom trapezoids with a thin horizontal gap.
		 * Limit the gap so even a one-pixel-high cell has both pieces.
		 */
		gap = thin < height / 3.0 ? thin : height / 3.0;
		vec_move_to(cr, 0, 0);
		vec_line_to(cr, width, 0);
		vec_line_to(cr, width / 2.0, mid - gap / 2.0);
		vec_line_to(cr, 0, mid - gap / 2.0);
		vec_close_path(cr);
		/* Keep both disconnected pieces clockwise. FreeType accumulates
		 * signed coverage, so opposing contours could cancel in a 1px cell. */
		vec_move_to(cr, 0, height);
		vec_line_to(cr, 0, mid + gap / 2.0);
		vec_line_to(cr, width / 2.0, mid + gap / 2.0);
		vec_line_to(cr, width, height);
		break;
	}

	/* Thin variants trace only the separator, not its cell-edge base. */
	if (outline) {
		vec_stroke(cr);
	} else {
		vec_close_path(cr);
		vec_fill(cr);
	}
	vec_restore(cr);
	vec_new_path(cr);
	vec_append_path(cr, previous_path);
	vec_path_destroy(previous_path);
}
