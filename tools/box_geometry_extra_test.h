/* Semantic tests for the 423 Ghostty-compatible additions. Included by the
 * headless production-renderer harness, after render()/pixel() helpers. */
static void
check_all_routes(void)
{
	static unsigned char wanted[0x110000];
	for (unsigned i = 0; i < LEN(geometry_inventory); i++) wanted[geometry_inventory[i]] = 1;
	wanted[0x25ce] = 1; /* Existing bullseye, absent from the original box.txt. */
	for (Rune u = 0x2800; u <= 0x28ff; u++) wanted[u] = 1;
	for (Rune u = 0xe0b0; u <= 0xe0bf; u++) wanted[u] = 1;
	wanted[0xe0d2] = wanted[0xe0d4] = 1;
	for (int branch = 0; branch <= 1; branch++) {
		boxdraw_branch = branch;
		unsigned count = 0;
		for (Rune u = 0; u <= 0x10ffff; u++) {
			int want = wanted[u] && (branch || u < 0xf5d0 || u > 0xf60d);
			assert(!!isboxdraw(u) == want);
			count += want;
		}
		assert(count == (branch ? 1143u : 1081u));
	}
	assert(boxextraindex(0xf5d0) & (1 << 11));
}

static void
check_mosaic_semantics(void)
{
	/* Check all 230 octant masks against the frozen Unicode-name data,
	 * at both even and odd dimensions. */
	for (unsigned n = 0; n < LEN(expected_octants); n++) for (int odd = 0; odd <= 1; odd++) {
		int w = 8+odd, h = 16+odd;
		clear_image(); render(0x1cd00 + n, PX, PY, w, h, 0);
		for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
			int row = 0;
			while (row < 3 && y >= (h*(row+1)+2)/4) row++;
			int bit = row*2 + (x >= (w+1)/2);
			assert(pixel(PX+x,PY+y) == ((expected_octants[n] & (1<<bit)) ? 0xffffffff : 0xff121212));
		}
	}
	/* Every separated quadrant/sextant has the intended occupancy and
	 * retains blank margins, rather than becoming a connected block. */
	for (int rows = 2; rows <= 3; rows++) for (int bits = 1; bits < (1 << (rows*2)); bits++) {
		Rune u = (rows == 2 ? 0x1cc20 : 0x1ce50) + bits;
		clear_image(); render(u, PX, PY, 12, 24, 0);
		for (int b = 0; b < rows*2; b++) {
			int x = b%2*6+3, y = b/2*(24/rows)+12/rows;
			assert(pixel(PX+x,PY+y) == ((bits & (1<<b)) ? 0xffffffff : 0xff121212));
		}
		for (int y = 0; y < 24; y++) {
			assert(pixel(PX,PY+y) == 0xff121212);
			assert(pixel(PX+5,PY+y) == 0xff121212);
			assert(pixel(PX+6,PY+y) == 0xff121212);
		}
	}
	/* Sixteen single-sixteenth cells must union to precisely one cell. */
	clear_image();
	for (int i = 0; i < 16; i++) render(0x1ce90 + i, PX, PY, 9, 17, 0);
	for (int y = 0; y < 17; y++) for (int x = 0; x < 9; x++)
		assert(pixel(PX+x,PY+y) == 0xffffffff);
	for (int n = 0; n < 2; n++) {
		clear_image(); render(0x1fbce + n, PX, PY, 10, 17, 0);
		for (int x = 0; x < 10; x++)
			assert(pixel(PX+x,PY+8) == (x < (n ? 3 : 7) ? 0xffffffff : 0xff121212));
	}
	const Rune triangles[] = {0x25f8,0x25f9,0x25fa,0x25ff};
	for (unsigned n = 0; n < LEN(triangles); n++) {
		clear_image(); render(triangles[n], PX, PY, 24, 40, 0);
		assert(pixel(PX+6,PY+5) == 0xff121212);
		assert(pixel(PX+12,PY+(n < 2 ? 0 : 39)) == 0xffffffff);
		assert(pixel(PX+(n%2 ? 23 : 0),PY+20) == 0xffffffff);
	}
	/* Inverted glyph coverage is the exact complement of its line form. */
	const Rune holes[] = {0x2573,0x1fba3,0x1fbae};
	unsigned char original[16*32];
	for (unsigned n = 0; n < LEN(holes); n++) {
		clear_image(); render(holes[n], PX, PY, 16, 32, 0);
		for (int y = 0; y < 32; y++) for (int x = 0; x < 16; x++)
			original[y*16+x] = pixel(PX+x,PY+y)&255;
		clear_image(); render(0x1fbbd + n, PX, PY, 16, 32, 0);
		for (int y = 0; y < 32; y++) for (int x = 0; x < 16; x++)
			assert(abs((int)(pixel(PX+x,PY+y)&255)+original[y*16+x]-273) <= 1);
	}
}

static void
check_branch_semantics(void)
{
	const unsigned char edges[] = {0,2,8,10,4,1,5,6,12,3,9,7,13,14,11,15};
	for (int i = 0; i < 32; i++) {
		int bits = edges[i/2];
		clear_image(); render(0xf5ee + i, PX, PY, 16, 32, 0);
		assert(pixel(PX+8,PY+16) == (i%2 ? 0xff121212 : 0xffffffff));
		assert(pixel(PX+8,PY) == ((bits & 1) ? 0xffffffff : 0xff121212));
		assert(pixel(PX+8,PY+31) == ((bits & 4) ? 0xffffffff : 0xff121212));
		clear_image(); render(0xf5ee + i, PX, PY, 32, 16, 0);
		assert(pixel(PX,PY+8) == ((bits & 8) ? 0xffffffff : 0xff121212));
		assert(pixel(PX+31,PY+8) == ((bits & 2) ? 0xffffffff : 0xff121212));
	}
	for (int i = 0; i < 4; i++) {
		clear_image(); render(0xf5d2 + i, PX, PY, 16, 32, 0);
		int length = i < 2 ? 16 : 32, prev = i%2 ? 18 : 255;
		for (int j = 0; j < length; j++) {
			int value = pixel(PX+(i<2 ? j : 8),PY+(i<2 ? 16 : j)) & 255;
			assert(i%2 ? value >= prev : value <= prev);
			if (j == 0) assert(value == (i%2 ? 18 : 255));
			if (j == length-1) assert(value == (i%2 ? 255 : 18));
			prev = value;
		}
	}
	/* A corner's horizontal/vertical edge connectivity must match its
	 * PUA definition. Bit convention: up/right/down/left. */
	const unsigned char turn_edges[] = {
		6,12,3,9,7,7,7,13,13,13,14,14,14,11,11,11,
		15,15,15,15,15,15,15,15
	};
	for (int i = 0; i < 24; i++) {
		int bits = turn_edges[i];
		clear_image(); render(0xf5d6 + i, PX, PY, 16, 32, 0);
		assert(pixel(PX+8,PY) == ((bits&1) ? 0xffffffff : 0xff121212));
		assert(pixel(PX+15,PY+16) == ((bits&2) ? 0xffffffff : 0xff121212));
		assert(pixel(PX+8,PY+31) == ((bits&4) ? 0xffffffff : 0xff121212));
		assert(pixel(PX,PY+16) == ((bits&8) ? 0xffffffff : 0xff121212));
	}
}

static void
check_scaled_alpha(void)
{
	const double scales[] = {1.25,1.5,2};
	for (unsigned i = 0; i < LEN(geometry_inventory); i++) {
		Rune u = geometry_inventory[i];
		if (!isboxextra(u)) continue;
		/* Branch overlaps and inverse masks must not apply FG alpha twice. */
		if (isboxmosaic(u) || isboxbranch(u) ||
		    (u >= 0x1cc1b && u <= 0x1cc1e) || (u >= 0x1ce16 && u <= 0x1ce19)) {
			foreground.color.alpha = 32768;
			clear_image(); render(u, PX, PY, 16, 32, ATTR_BOLD);
			for (int y = PY; y < PY+32; y++) for (int x = PX; x < PX+16; x++) {
				uint32_t p = pixel(x,y);
				assert((p>>24) == 255 && (p&255) <= 137);
			}
			foreground.color.alpha = 65535;
		}
		for (unsigned k = 0; k < LEN(scales); k++) {
			double s = scales[k];
			clear_image();
			cairo_save(xw.draw); cairo_scale(xw.draw,s,s);
			render(u, PX, PY, 9, 17, 0);
			cairo_restore(xw.draw);
			for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++)
				if (x < floor(PX*s) || x >= ceil((PX+9)*s) ||
				    y < floor(PY*s) || y >= ceil((PY+17)*s))
					assert(pixel(x,y) == 0xff121212);
		}
	}
}
