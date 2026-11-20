/* Geometric circles, included by boxdraw.c (Rune and vector already declared).
 * No font lookup and no background painting. The caller supplies an integer
 * cell clip, local origin, foreground source, and saves/restores vector state.
 * References (geometry, not copied implementation): Unicode U25A0/U1CC00/
 * U1FB00 charts; ghostty src/font/sprite/draw/
 * symbols_for_legacy_computing{,_supplement}.zig, circle/circlePiece.
 * U1CC30..3F: twelve outer tiles of a 4x4 ellipse, with a separate 2x2
 * ellipse in the four central cells, as in Notes/box.txt. U1FBE4..7 are
 * rectangular quarter blocks, deliberately not claimed here.
 */
static int
isboxcircle(Rune u)
{
	return (u >= 0x25cb && u <= 0x25d7) ||
	       (u >= 0x25d9 && u <= 0x25e1) || u == 0x25ef ||
	       (u >= 0x25f4 && u <= 0x25f7) || u == 0x2b24 ||
	       (u >= 0x2b2c && u <= 0x2b2f) || u == 0x2b55 ||
	       u == 0x2b57 || u == 0x2b58 || u == 0x2bca || u == 0x2bcb ||
	       (u >= 0x1cc30 && u <= 0x1cc3f) ||
	       (u >= 0x1cc6b && u <= 0x1cc6e) ||
	       u == 0x1ce00 || u == 0x1ce01 || u == 0x1ce0b || u == 0x1ce0c ||
	       u == 0x1f784 || (u >= 0x1f787 && u <= 0x1f78a) ||
	       u == 0x1f907 || (u >= 0x1fbe0 && u <= 0x1fbe3) ||
	       (u >= 0x1fbe8 && u <= 0x1fbef);
}

/* Angles in quarter turns, clockwise in vector's downward Y coordinates.
 * Restore the CTM before stroking, so ellipses retain uniform line weight. */
static void
boxcirclearc(VecContext *cr, double x, double y, double rx, double ry,
             double a, double b)
{
	const double halfpi = 1.57079632679489661923;
	vec_save(cr);
	vec_translate(cr, x, y);
	vec_scale(cr, rx, ry);
	vec_arc(cr, 0, 0, 1, a * halfpi, b * halfpi);
	vec_restore(cr);
}

static void
boxcircledisk(VecContext *cr, double x, double y, double rx, double ry, int filled)
{
	vec_new_sub_path(cr);
	boxcirclearc(cr, x, y, rx, ry, 0, 4);
	vec_close_path(cr);
	if (filled)
		vec_fill(cr);
	else
		vec_stroke(cr);
}

static void
drawboxcircle(VecContext *cr, int w, int h, Rune u)
{
	double d, t, r, rx, ry, x, y, a = 0, b = 4;
	int n, filled = 0;

	if (w <= 0 || h <= 0 || !isboxcircle(u))
		return;
	d = w < h ? w : h;
	/* At 1px retain positive radii, never feed a singular CTM to vector. */
	t = d / 12.0;
	if (t < 1.0) t = 1.0;
	if (t > d * 0.6) t = d * 0.6;
	r = (d - t) / 2;
	x = w / 2.0; y = h / 2.0;
	vec_new_path(cr);
	vec_set_line_width(cr, t);
	vec_set_line_cap(cr, VEC_LINE_CAP_BUTT);
	vec_set_line_join(cr, VEC_LINE_JOIN_ROUND);
	vec_set_fill_rule(cr, VEC_FILL_RULE_WINDING);

	if (u >= 0x1cc30 && u <= 0x1cc3f) {
		int col = (u - 0x1cc30) % 4, row = (u - 0x1cc30) / 4;
		int inner = (col == 1 || col == 2) && (row == 1 || row == 2);
		/* Shared absolute ellipse for every tile: endpoints and tangents
		 * remain identical at tile boundaries even for odd cell sizes. */
		rx = (inner ? w : 2.0*w) - t/2;
		ry = (inner ? h : 2.0*h) - t/2;
		boxcircledisk(cr, (2-col)*(double)w, (2-row)*(double)h, rx, ry, 0);
		return;
	}
	if (u == 0x1ce0b || u == 0x1ce0c) {
		boxcircledisk(cr, u == 0x1ce0b ? w : 0, y, w-t/2, (h-t)/2, 0);
		return;
	}
	if ((u >= 0x1fbe0 && u <= 0x1fbe3) ||
	    (u >= 0x1fbe8 && u <= 0x1fbef) || u == 0x1ce00 || u == 0x1ce01) {
		if (u == 0x1ce00 || u == 0x1ce01) {
			/* Two outward-facing edge-centred semicircles. */
			if (u == 0x1ce00) {
				boxcircledisk(cr, 0, y, r, r, 0);
				boxcircledisk(cr, w, y, r, r, 0);
			} else {
				boxcircledisk(cr, x, 0, r, r, 0);
				boxcircledisk(cr, x, h, r, r, 0);
			}
			return;
		}
		filled = u >= 0x1fbe8;
		n = u - (filled ? 0x1fbe8 : 0x1fbe0);
		if (filled) r = d/2;
		switch (n) {
		case 0: y = 0; break;
		case 1: x = w; break;
		case 2: y = h; break;
		case 3: x = 0; break;
		case 4: x = w; y = 0; break;
		case 5: x = 0; y = h; break;
		case 6: x = w; y = h; break;
		case 7: x = 0; y = 0; break;
		}
		boxcircledisk(cr, x, y, r, r, filled);
		return;
	}
	/* Standalone symbols share a centre and base circle, not font metrics. */
	if (u == 0x25ef || u == 0x2b24 || u == 0x2b55 ||
	    (u >= 0x1cc6b && u <= 0x1cc6e)) r = (d-t)/2;
	else r *= 0.85;
	if (u == 0x1f784) r *= 0.7;
	if (u == 0x2b55 || u == 0x2b57 || u == 0x2b58 ||
	    (u >= 0x1f787 && u <= 0x1f789)) {
		double weight = (u >= 0x1f787 && u <= 0x1f789) ?
		                0.22 + (u-0x1f787)*0.10 : 0.20;
		t = d * weight;
		r = (d-t)/2;
		vec_set_line_width(cr, t);
	}
	rx = ry = r;
	if (u == 0x2b2c || u == 0x2b2d) ry *= 0.55;
	if (u == 0x2b2e || u == 0x2b2f) { rx *= 0.65; ry = (h-t)*0.425; }
	if (u == 0x25cc) {
		double dash[2] = { t, t*1.8 };
		vec_set_dash(cr, dash, 2, 0);
		vec_set_line_cap(cr, VEC_LINE_CAP_ROUND);
	}
	if (u >= 0x25d9 && u <= 0x25db) {
		/* Invert the outline, not the whole disk: the annulus is left
		 * unpainted, while its centre remains foreground (unlike U+25D8).
		 * Never clear existing pixels or paint a guessed background. */
		if (u == 0x25da) vec_rectangle(cr, 0, 0, w, y);
		else if (u == 0x25db) vec_rectangle(cr, 0, y, w, h-y);
		else vec_rectangle(cr, 0, 0, w, h);
		vec_clip(cr);
		vec_rectangle(cr, 0, 0, w, h);
		vec_new_sub_path(cr);
		boxcirclearc(cr, x, y, r+t/2, r+t/2, 0, 4);
		vec_close_path(cr);
		if (r > t/2) {
			vec_new_sub_path(cr);
			boxcirclearc(cr, x, y, r-t/2, r-t/2, 0, 4);
			vec_close_path(cr);
		}
		vec_set_fill_rule(cr, VEC_FILL_RULE_EVEN_ODD);
		vec_fill(cr);
		return;
	}
	if (u >= 0x25dc && u <= 0x25df) {
		static const unsigned char starts[] = { 2, 3, 0, 1 };
		a = starts[u-0x25dc]; b = a+1;
	} else if (u == 0x25e0) { a = 2; b = 4; }
	else if (u == 0x25e1) { a = 0; b = 2; }
	else if (u == 0x1f907) { a = 1; b = 3; }
	if (b-a < 4) {
		boxcirclearc(cr, x, y, rx, ry, a, b);
		vec_stroke(cr);
		return;
	}
	if (u >= 0x1cc6b && u <= 0x1cc6e) {
		a = 2.5 + (u-0x1cc6b); /* omit a 90-degree cardinal wedge */
		vec_move_to(cr, x, y);
		boxcirclearc(cr, x, y, r, r, a, a+3);
		vec_close_path(cr);
		vec_fill(cr);
		return;
	}
	if (u == 0x25d6 || u == 0x25d7 || u == 0x2bca || u == 0x2bcb) {
		a = u == 0x25d6 ? 1 : u == 0x25d7 ? -1 : u == 0x2bca ? 2 : 0;
		boxcirclearc(cr, x, y, r, r, a, a+2);
		vec_close_path(cr);
		vec_fill(cr);
		return;
	}
	filled = u == 0x25cf || u == 0x2b24 || u == 0x2b2c ||
	         u == 0x2b2e || u == 0x1f784;
	boxcircledisk(cr, x, y, rx, ry, filled);
	if (u == 0x25ce || u == 0x2b57)
		boxcircledisk(cr, x, y, r*0.48, r*0.48, 0);
	if (u == 0x1f78a)
		boxcircledisk(cr, x, y, r*0.40, r*0.40, 1);
	if (u == 0x25cd) {
		double xx, step = t*2.5;
		vec_new_sub_path(cr);
		boxcirclearc(cr, x, y, r, r, 0, 4);
		vec_clip(cr);
		for (xx = x-r+step/2; xx < x+r; xx += step) {
			vec_move_to(cr, xx, y-r);
			vec_line_to(cr, xx, y+r);
		}
		vec_stroke(cr);
	} else if (u >= 0x25d0 && u <= 0x25d5) {
		static const double start[] = { 1, -1, 0, 2, 3, 3 };
		static const double extent[] = { 2, 2, 2, 2, 1, 3 };
		n = u-0x25d0;
		vec_move_to(cr, x, y);
		boxcirclearc(cr, x, y, r, r, start[n], start[n]+extent[n]);
		vec_close_path(cr);
		vec_fill(cr);
	} else if (u >= 0x25f4 && u <= 0x25f7) {
		/* Two radii mark the named (unfilled) quadrant. */
		n = u-0x25f4;
		vec_move_to(cr, x + ((n == 0 || n == 1) ? -r : r), y);
		vec_line_to(cr, x, y);
		vec_line_to(cr, x, y + ((n == 0 || n == 3) ? -r : r));
		vec_stroke(cr);
	}
}
