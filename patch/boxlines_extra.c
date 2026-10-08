/* Extra cell-local box lines for the Cairo backend: dashed/rounded U+2500
 * forms, diagonal legacy-computing forms, and double diagonals. Geometry is
 * original, following Unicode character names (including Unicode 16 forms).
 * Include after Rune and Cairo declarations; requires <math.h>. The caller
 * supplies foreground, cell translation/clip and saves/restores Cairo state
 * AND path. No backgrounds are painted here. Diagonal anchors use actual
 * cell edges/halves, not rounded pixel centres, so odd-sized cells tile too.
 */
#include <math.h>

static int
isboxlinesextra(Rune u)
{
	return (u >= 0x2504 && u <= 0x250b) ||
	       (u >= 0x254c && u <= 0x254f) ||
	       (u >= 0x256d && u <= 0x2573) ||
	       (u >= 0x1fba0 && u <= 0x1fbaf) ||
	       (u >= 0x1fbd0 && u <= 0x1fbdf) ||
	       (u >= 0x1cc1b && u <= 0x1cc20) ||
	       (u >= 0x1ce16 && u <= 0x1ce19) ||
	       u == 0x1ce09 || u == 0x1ce0a;
}

/* Stroke a two/three-point polyline, optionally offset along its normals.
 * Extend terminal tangents beyond the clip: butt caps AT cell corners leave
 * triangular pinholes, especially in double diagonals. Offset joins use the
 * intersection of the parallel lines, keeping both rails equally spaced.
 */
static void
boxextra_poly(cairo_t *cr, const double *x, const double *y, int n,
              double offset, double extension)
{
	double nx[2], ny[2], px[3], py[3], dx, dy, len, k;
	int i;
	for (i = 0; i < n - 1; ++i) {
		dx = x[i+1] - x[i]; dy = y[i+1] - y[i];
		len = hypot(dx, dy);
		nx[i] = -dy / len; ny[i] = dx / len;
	}
	px[0] = x[0] + offset * nx[0] - extension * ny[0];
	py[0] = y[0] + offset * ny[0] + extension * nx[0];
	if (n == 3) {
		k = offset / (1 + nx[0]*nx[1] + ny[0]*ny[1]);
		px[1] = x[1] + k * (nx[0] + nx[1]);
		py[1] = y[1] + k * (ny[0] + ny[1]);
	}
	px[n-1] = x[n-1] + offset * nx[n-2] + extension * ny[n-2];
	py[n-1] = y[n-1] + offset * ny[n-2] - extension * nx[n-2];
	cairo_move_to(cr, px[0], py[0]);
	for (i = 1; i < n; ++i)
		cairo_line_to(cr, px[i], py[i]);
}

static void
drawboxlinesextra(cairo_t *cr, int w, int h, Rune u, int bold)
{
	/* Point indices: TL TC TR / ML MC MR / BL BC BR. */
	static const unsigned char diamond[] = {
		1, 2, 4, 8, 5, 10, 12, 3, 9, 6, 14, 13, 11, 7, 15
	};
	static const unsigned char edges[4][2] = {{1,3},{1,5},{3,7},{5,7}};
	static const unsigned char paths[16][3] = {
		{5,6,6}, {2,3,3}, {0,5,5}, {3,8,8},
		{0,7,7}, {1,8,8}, {2,7,7}, {1,6,6},
		{0,4,2}, {2,4,8}, {6,4,8}, {0,4,6},
		{0,7,2}, {2,3,8}, {6,1,8}, {0,5,6}
	};
	double s, cx, cy, thick, a, b, radius, sx, sy, ex, ey;
	double x[3], y[3], xx[9], yy[9];
	int m, vertical, count, i, j, n, mask, ids[3], doubled = 0;
	if (w <= 0 || h <= 0 || !isboxlinesextra(u))
		return;
	m = w < h ? w : h;
	s = fmax(1, round(m / 8.0));
	if (bold && m >= 6)
		s = fmax(s + 1, round(1.5 * s));
	cx = ceil((w - s) / 2) + s / 2;
	cy = ceil((h - s) / 2) + s / 2;
	cairo_new_path(cr);
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_GRAY);
	cairo_set_dash(cr, NULL, 0, 0);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
	cairo_set_miter_limit(cr, 1000);
	cairo_set_line_width(cr, s);
	if ((u >= 0x2504 && u <= 0x250b) || (u >= 0x254c && u <= 0x254f)) {
		vertical = (u & 2) != 0;
		count = u >= 0x254c ? 2 : u >= 0x2508 ? 4 : 3;
		/* Existing heavy rules are the union of light + two double rails. */
		thick = (u & 1) ? 3*s : s;
		/* Integer rectangles, equal periods, half a gap at either edge.
		 * At undersized cells reduce count rather than disappear entirely. */
		m = vertical ? h : w;
		if (count > (m+1)/2) count = (m+1)/2;
		for (i = 0; i < count; ++i) {
			a = floor((i + 0.15) * m / count);
			b = floor((i + 0.85) * m / count);
			if (b <= a) b = a + 1;
			if (vertical)
				cairo_rectangle(cr, ceil(cx-thick/2), a, thick, b-a);
			else
				cairo_rectangle(cr, a, ceil(cy-thick/2), b-a, thick);
		}
		cairo_fill(cr);
		return;
	}
	if (u >= 0x256d && u <= 0x2570) {
		sx = (u == 0x256d || u == 0x2570) ? 1 : -1;
		sy = (u == 0x256d || u == 0x256e) ? 1 : -1;
		ex = sx > 0 ? w : 0; ey = sy > 0 ? h : 0;
		a = fmin(fabs(ex-cx), fabs(ey-cy));
		/* Leave a full straight pixel at each edge whenever space permits. */
		radius = fmax(0, fmin(a * 0.8, a - 1));
		cairo_move_to(cr, ex, cy);
		cairo_line_to(cr, cx + sx*radius, cy);
		/* True tangent quarter-circle approximation, not a bevel. */
		a = 0.5522847498307936;
		cairo_curve_to(cr, cx+sx*radius*(1-a), cy,
		               cx, cy+sy*radius*(1-a), cx, cy+sy*radius);
		cairo_line_to(cr, cx, ey);
		cairo_stroke(cr);
		return;
	}
	if (u >= 0x1cc1b && u <= 0x1cc1e) {
		if (u <= 0x1cc1c) {
			cairo_rectangle(cr, 0, cy-s/2, w, s);
			cairo_rectangle(cr, w-s, u == 0x1cc1b ? 0 : cy-s/2,
			                s, u == 0x1cc1b ? cy+s/2 : h-cy+s/2);
		} else {
			cairo_rectangle(cr, 0, u == 0x1cc1d ? 0 : h-s, w, s);
			cairo_rectangle(cr, 0, u == 0x1cc1d ? 0 : cy-s/2,
			                s, u == 0x1cc1d ? cy+s/2 : h-cy+s/2);
		}
		cairo_fill(cr);
		return;
	}
	if (u >= 0x1ce16 && u <= 0x1ce19) {
		int right = u <= 0x1ce17, bottom = u & 1;
		cairo_rectangle(cr, cx-s/2, 0, s, h);
		cairo_rectangle(cr, right ? cx-s/2 : 0, bottom ? h-s : 0,
		                right ? w-cx+s/2 : cx+s/2, s);
		cairo_fill(cr);
		return;
	}
	if (u == 0x1fbaf) {
		cairo_rectangle(cr, 0, cy-s/2, w, s);
		cairo_rectangle(cr, cx-s/2, floor(h/4.0), s, ceil(3*h/4.0)-floor(h/4.0));
		cairo_fill(cr);
		return;
	}
	for (i = 0; i < 9; ++i) {
		xx[i] = (i%3) * w/2.0;
		yy[i] = (i/3) * h/2.0;
	}
	if (u >= 0x1fba0 && u <= 0x1fbae) {
		mask = diamond[u-0x1fba0];
		/* Square caps cover shared edge vertices without round bulges;
		 * all subpaths are stroked together, avoiding alpha seams. */
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
		for (i = 0; i < 4; ++i) if (mask & (1 << i)) {
			cairo_move_to(cr, xx[edges[i][0]], yy[edges[i][0]]);
			cairo_line_to(cr, xx[edges[i][1]], yy[edges[i][1]]);
		}
	} else {
		n = 2;
		if (u >= 0x1fbd0 && u <= 0x1fbdf) {
			for (i = 0; i < 3; ++i) ids[i] = paths[u-0x1fbd0][i];
			if (ids[1] != ids[2]) n = 3;
		} else if (u == 0x1ce09 || u == 0x1ce0a) {
			ids[0] = u == 0x1ce09 ? 6 : 0;
			ids[1] = 4; ids[2] = u == 0x1ce09 ? 8 : 2;
			n = 3; doubled = 1;
		} else {
			ids[0] = (u == 0x2571 || u == 0x1cc1f) ? 2 : 0;
			ids[1] = ids[0] == 2 ? 6 : 8;
			doubled = u == 0x1cc1f || u == 0x1cc20;
		}
		for (j = 0; j < n; ++j) { x[j] = xx[ids[j]]; y[j] = yy[ids[j]]; }
		/* Two rails, one stem of clear space. Clamp for tiny cells. */
		a = fmin(s, m/4.0);
		if (doubled) {
			cairo_set_line_width(cr, fmin(s, m/4.0));
			boxextra_poly(cr, x, y, n, a, w+h+2*s);
			boxextra_poly(cr, x, y, n, -a, w+h+2*s);
		} else {
			boxextra_poly(cr, x, y, n, 0, w+h+2*s);
			if (u == 0x2573) {
				x[0] = w; y[0] = 0; x[1] = 0; y[1] = h;
				boxextra_poly(cr, x, y, 2, 0, w+h+2*s);
			}
		}
	}
	cairo_stroke(cr);
}
