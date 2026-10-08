/* Cell-aligned geometric Powerline symbols; included by boxdraw.c.
 * Rune and Color come from st.h. No font metrics or background painting.
 */
#include <cairo.h>

static int
ispowerline(Rune u)
{
	return (u >= 0xe0b0 && u <= 0xe0bf) || u == 0xe0d2 || u == 0xe0d4;
}

static void
drawpowerline(cairo_t *cr, int x, int y, int w, int h,
              const Color *fg, Rune u)
{
	const double arc = 0.5522847498307936; /* quarter-circle Bezier coefficient */
	double width = w, height = h, mid = height / 2.0;
	double thin, gap;
	int outline;
	cairo_path_t *previous_path;

	if (w <= 0 || h <= 0 || !ispowerline(u))
		return;

	thin = (w < h ? width : height) / 8.0;
	if (thin < 1.0)
		thin = 1.0;
	outline = u >= 0xe0b0 && u <= 0xe0bf && (u & 1);

	/* Cairo save/restore does not preserve the current path. */
	previous_path = cairo_copy_path(cr);
	cairo_save(cr);
	/* Text rendering leaves a cached clip behind; geometry owns its cell. */
	cairo_reset_clip(cr);
	cairo_new_path(cr);
	cairo_rectangle(cr, x, y, width, height);
	cairo_clip(cr);
	cairo_translate(cr, x, y);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_GRAY);
	cairo_set_source_rgba(cr, fg->color.red / 65535.0,
	                     fg->color.green / 65535.0,
	                     fg->color.blue / 65535.0,
	                     fg->color.alpha / 65535.0);
	cairo_set_line_width(cr, thin);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
	cairo_set_miter_limit(cr, 10.0);
	cairo_set_dash(cr, NULL, 0, 0);
	cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);

	/* Left-pointing arrows/caps and D4 are exact horizontal mirrors. */
	if (u == 0xe0b2 || u == 0xe0b3 || u == 0xe0b6 ||
	    u == 0xe0b7 || u == 0xe0d4) {
		cairo_translate(cr, width, 0);
		cairo_scale(cr, -1, 1);
	}

	switch (u) {
	case 0xe0b0: case 0xe0b1: case 0xe0b2: case 0xe0b3:
		/* Full-height base, with the point at the opposite cell edge. */
		cairo_move_to(cr, 0, 0);
		cairo_line_to(cr, width, mid);
		cairo_line_to(cr, 0, height);
		break;
	case 0xe0b4: case 0xe0b5: case 0xe0b6: case 0xe0b7:
		/* Half ellipse: use both actual cell dimensions, even when odd. */
		cairo_move_to(cr, 0, 0);
		cairo_curve_to(cr, arc * width, 0, width, mid * (1 - arc),
		               width, mid);
		cairo_curve_to(cr, width, mid * (1 + arc), arc * width, height,
		               0, height);
		break;
	case 0xe0b8: /* lower-left */
		cairo_move_to(cr, 0, 0);
		cairo_line_to(cr, width, height);
		cairo_line_to(cr, 0, height);
		break;
	case 0xe0ba: /* lower-right */
		cairo_move_to(cr, width, 0);
		cairo_line_to(cr, width, height);
		cairo_line_to(cr, 0, height);
		break;
	case 0xe0bc: /* upper-left */
		cairo_move_to(cr, 0, 0);
		cairo_line_to(cr, width, 0);
		cairo_line_to(cr, 0, height);
		break;
	case 0xe0be: /* upper-right */
		cairo_move_to(cr, 0, 0);
		cairo_line_to(cr, width, 0);
		cairo_line_to(cr, width, height);
		break;
	case 0xe0b9: case 0xe0bf: /* descending diagonal only */
		cairo_move_to(cr, 0, 0);
		cairo_line_to(cr, width, height);
		break;
	case 0xe0bb: case 0xe0bd: /* ascending diagonal only */
		cairo_move_to(cr, width, 0);
		cairo_line_to(cr, 0, height);
		break;
	case 0xe0d2: case 0xe0d4:
		/* Opposed top/bottom trapezoids with a thin horizontal gap.
		 * Limit the gap so even a one-pixel-high cell has both pieces.
		 */
		gap = thin < height / 3.0 ? thin : height / 3.0;
		cairo_move_to(cr, 0, 0);
		cairo_line_to(cr, width, 0);
		cairo_line_to(cr, width / 2.0, mid - gap / 2.0);
		cairo_line_to(cr, 0, mid - gap / 2.0);
		cairo_close_path(cr);
		cairo_move_to(cr, 0, height);
		cairo_line_to(cr, width, height);
		cairo_line_to(cr, width / 2.0, mid + gap / 2.0);
		cairo_line_to(cr, 0, mid + gap / 2.0);
		break;
	}

	/* Thin variants trace only the separator, not its cell-edge base. */
	if (outline) {
		cairo_stroke(cr);
	} else {
		cairo_close_path(cr);
		cairo_fill(cr);
	}
	cairo_restore(cr);
	cairo_new_path(cr);
	cairo_append_path(cr, previous_path);
	cairo_path_destroy(previous_path);
}
