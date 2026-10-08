/* Headless tests of the real Wayland callbacks, palette and background renderer.
 * Only the protocol acknowledgement is replaced: no fonts, display or pty. */
#define _GNU_SOURCE
#include "xdg-shell-client-protocol.h"
static void test_ack_configure(struct xdg_surface *, uint32_t);
#define xdg_surface_ack_configure test_ack_configure
#define main st_program_main
#include "../wl.c"
#undef main
#undef xdg_surface_ack_configure
#include <assert.h>

#define ROWS 4
#define COLS 3
static int dirty[ROWS], acknowledgements;
static uint32_t expectedserial;
static Color ackbg;
#if ALPHA_PATCH
static int ackfullscreen;
#endif

static void
samecolor(Color a, Color b)
{
	assert(a.color.red == b.color.red && a.color.green == b.color.green);
	assert(a.color.blue == b.color.blue && a.color.alpha == b.color.alpha);
	assert(a.pixel == b.pixel);
}

static void
test_ack_configure(struct xdg_surface *surface, uint32_t serial)
{
	assert(surface == NULL && serial == expectedserial);
	/* The previous state/palette must still be in force at acknowledgement. */
	#if ALPHA_PATCH
	assert(isfullscreen == ackfullscreen);
	#endif
	samecolor(dc.col[defaultbg], ackbg);
	for (int y = 0; y < ROWS; y++) assert(!dirty[y]);
	acknowledgements++;
}

static float
windowalpha(void)
{
	#if ALPHA_PATCH
	#if ALPHA_FOCUS_HIGHLIGHT_PATCH
	return focused ? alpha : alphaUnfocused;
	#else
	return alpha;
	#endif
	#else
	return 1.0f;
	#endif
}

static float
renderalpha(void)
{
	#if ALPHA_PATCH
	if (isfullscreen) return 1.0f;
	#endif
	return windowalpha();
}

static void
checkalpha(Color c, float a)
{
	unsigned short want = (unsigned short)(0xffff * a);
	assert(c.color.alpha == want);
	assert((c.pixel >> 24) == (want >> 8));
}

static void
palettealpha(void)
{
	checkalpha(dc.col[defaultbg], renderalpha());
	#if SELECTION_COLORS_PATCH
	#if ALPHA_PATCH && SELECTIONBG_ALPHA_PATCH
	checkalpha(dc.col[selectionbg], renderalpha());
	#else
	checkalpha(dc.col[selectionbg], 1.0f);
	#endif
	#endif
}

static void
queueconfigure(int w, int h, uint32_t *values, size_t count, int fullscreen)
{
	/* Valid non-NULL storage even for an empty wl_array. */
	uint32_t empty = 0;
	struct wl_array states = {.size = count * sizeof(*values),
		.data = count ? values : &empty};
	Color before = dc.col[defaultbg];
	int events = wlevents, beforedirty[ROWS];
	memcpy(beforedirty, dirty, sizeof(dirty));
	#if ALPHA_PATCH
	int applied = isfullscreen;
	#else
	(void)fullscreen;
	#endif
	toplevel_configure(NULL, NULL, w, h, &states);
	assert(pendingw == w && pendingh == h && wlevents == events);
	#if ALPHA_PATCH
	assert(pendingfullscreen == fullscreen && isfullscreen == applied);
	#endif
	samecolor(dc.col[defaultbg], before);
	assert(!memcmp(beforedirty, dirty, sizeof(dirty)));
}

static void
commitconfigure(int fullscreen)
{
	int events = wlevents, acks = acknowledgements, changed = 0;
	int w = xw.lw, h = xw.lh;
	Color *before = xmalloc(dc.collen * sizeof(*before));
	memcpy(before, dc.col, dc.collen * sizeof(*before));
	#if ALPHA_PATCH
	float savedalpha = alpha;
	#if ALPHA_FOCUS_HIGHLIGHT_PATCH
	float savedunfocused = alphaUnfocused;
	#endif
	ackfullscreen = isfullscreen;
	changed = isfullscreen != fullscreen;
	#else
	(void)fullscreen;
	#endif
	if (pendingw > 0 && pendingh > 0) w = pendingw, h = pendingh;
	ackbg = dc.col[defaultbg];
	memset(dirty, 0, sizeof(dirty));
	expectedserial++;
	xdgsurface_configure(NULL, NULL, expectedserial);
	assert(acknowledgements == acks + 1 && wlevents == events + 1);
	assert(xw.configured && IS_SET(MODE_VISIBLE));
	assert(xw.lw == w && xw.lh == h && win.w == w && win.h == h);
	#if ALPHA_PATCH
	assert(isfullscreen == fullscreen && alpha == savedalpha);
	#if ALPHA_FOCUS_HIGHLIGHT_PATCH
	assert(alphaUnfocused == savedunfocused);
	#endif
	#endif
	/* Real tfulldirt(), including same-size and zero-size state changes. */
	for (int y = 0; y < ROWS; y++) assert(dirty[y] == changed);
	for (int i = 0; i < dc.collen; i++) {
		/* Transitions must not reload OSC RGB values or unrelated colors. */
		assert(dc.col[i].color.red == before[i].color.red);
		assert(dc.col[i].color.green == before[i].color.green);
		assert(dc.col[i].color.blue == before[i].color.blue);
		if (i == defaultbg) continue;
		#if ALPHA_PATCH && SELECTION_COLORS_PATCH && SELECTIONBG_ALPHA_PATCH
		if (i == selectionbg) continue;
		#endif
		samecolor(dc.col[i], before[i]);
	}
	free(before);
	palettealpha();
}

static void
setfullscreen(int fullscreen)
{
	uint32_t state = XDG_TOPLEVEL_STATE_FULLSCREEN;
	queueconfigure(xw.lw, xw.lh, &state, fullscreen ? 1 : 0, fullscreen);
	commitconfigure(fullscreen);
}

static void
configuretests(void)
{
	uint32_t mixed[] = {XDG_TOPLEVEL_STATE_ACTIVATED, XDG_TOPLEVEL_STATE_MAXIMIZED,
		XDG_TOPLEVEL_STATE_FULLSCREEN};
	uint32_t maximized = XDG_TOPLEVEL_STATE_MAXIMIZED;
	uint32_t full = XDG_TOPLEVEL_STATE_FULLSCREEN;
	queueconfigure(0, 0, NULL, 0, 0); commitconfigure(0);
	queueconfigure(0, 0, mixed, LEN(mixed), 1); commitconfigure(1);
	queueconfigure(xw.lw, xw.lh, &full, 1, 1); commitconfigure(1);
	commitconfigure(1); /* repeated surface configure, no new toplevel event */
	queueconfigure(xw.lw, xw.lh, NULL, 0, 0); commitconfigure(0);
	/* Only the last pending configure applies at the surface acknowledgement. */
	queueconfigure(80, 60, &full, 1, 1);
	queueconfigure(xw.lw, xw.lh, &maximized, 1, 0); commitconfigure(0);
	queueconfigure(80, 0, &full, 1, 1); commitconfigure(1);
	queueconfigure(0, 60, &maximized, 1, 0); commitconfigure(0);
	queueconfigure(80, 60, mixed, LEN(mixed), 1); commitconfigure(1);
	queueconfigure(28, 36, NULL, 0, 0); commitconfigure(0);
}

static void
osccolors(void)
{
	assert(!xsetcolorname(defaultbg, "#123456"));
	assert(!xsetcolorname(3, "#abcdef"));
	#if SELECTION_COLORS_PATCH
	assert(!xsetcolorname(selectionbg, "#654321"));
	#endif
	palettealpha();
	setfullscreen(1);
	/* OSC 11/4 use this real color setter while fullscreen too. */
	assert(!xsetcolorname(defaultbg, "#345678"));
	assert(!xsetcolorname(3, "#fedcba"));
	#if SELECTION_COLORS_PATCH
	assert(!xsetcolorname(selectionbg, "#765432"));
	#endif
	palettealpha();
	setfullscreen(0);
	unsigned char r, g, b;
	assert(!xgetcolor(defaultbg, &r, &g, &b));
	assert(r == 0x34 && g == 0x56 && b == 0x78);
	assert(!xgetcolor(3, &r, &g, &b));
	assert(r == 0xfe && g == 0xdc && b == 0xba);
	checkalpha(dc.col[3], 1.0f);
}

#if ALPHA_PATCH
static void
optionandfocus(void)
{
	/* Exercise the -A storage consumed by xloadcols, without starting main. */
	alpha = 0.73f;
	opt_alpha = "0.42";
	xloadcols();
	assert(alpha == 0.42f);
	palettealpha();
	#if ALPHA_FOCUS_HIGHLIGHT_PATCH
	focusin(); palettealpha();
	/* Parsing -A must precede choosing the focused opacity, even on reload. */
	alpha = 0.73f;
	xloadcols();
	assert(alpha == 0.42f);
	checkalpha(dc.col[defaultbg], 0.42f);
	focusout(); palettealpha();
	#endif
	setfullscreen(1);
	xloadcols(); palettealpha(); /* reloads must honor fullscreen */
	#if ALPHA_FOCUS_HIGHLIGHT_PATCH
	focusin(); palettealpha();
	focusout(); palettealpha();
	#endif
	setfullscreen(0);
	#if ALPHA_FOCUS_HIGHLIGHT_PATCH
	checkalpha(dc.col[defaultbg], alphaUnfocused);
	focusin(); palettealpha();
	#endif
	checkalpha(dc.col[defaultbg], 0.42f);
	setfullscreen(1);
	setfullscreen(0);
	assert(alpha == 0.42f);
	opt_alpha = NULL;
	alpha = 0.67f;
	xloadalpha(); palettealpha();
	setfullscreen(1); setfullscreen(0);
	assert(alpha == 0.67f);
}
#endif

static uint32_t
pixel(int x, int y)
{
	cairo_surface_flush(xw.buf);
	return ((uint32_t *)(cairo_image_surface_get_data(xw.buf) +
		y * cairo_image_surface_get_stride(xw.buf)))[x];
}

static void
backgroundpixels(int fullscreen)
{
	setfullscreen(fullscreen);
	Color *before = xmalloc(dc.collen * sizeof(*before));
	memcpy(before, dc.col, dc.collen * sizeof(*before));
	/* DRAW_BG deliberately avoids glyph/font loading, but uses the real path. */
	for (int selected = 0; selected <= SELECTION_COLORS_PATCH; selected++) {
		Glyph base = {.u = ' ', .fg = defaultfg, .bg = defaultbg};
		unsigned int color = defaultbg;
		#if SELECTION_COLORS_PATCH
		if (selected) base.mode = ATTR_SELECTED, color = selectionbg;
		#endif
		for (int y = 0; y < ROWS; y++) {
			xdrawglyphfontspecs(NULL, base, COLS, 0, y, DRAW_BG
				#if LIGATURES_PATCH
				, COLS
				#endif
			);
			Color want = dc.col[color];
			#if ALPHA_PATCH && ALPHA_GRADIENT_PATCH
			applyalpha(&want, fullscreen ? 1.0f :
				grad_alpha * (win.h - y * win.ch) / win.h + stat_alpha);
			#endif
			assert(pixel(3, 2 + y * win.ch) == premulpixel(&want));
			assert(pixel(0, 2 + y * win.ch) == premulpixel(&dc.col[defaultbg]));
			assert(pixel(win.w - 1, 2 + y * win.ch) == premulpixel(&dc.col[defaultbg]));
		}
		assert(pixel(3, 0) == premulpixel(&dc.col[defaultbg]));
		assert(pixel(3, win.h - 1) == premulpixel(&dc.col[defaultbg]));
		for (int i = 0; i < dc.collen; i++) samecolor(before[i], dc.col[i]);
	}
	free(before);
	assert(cairo_status(xw.draw) == CAIRO_STATUS_SUCCESS);
}

int
main(void)
{
	term.row = ROWS; term.col = COLS; term.dirty = dirty;
	xw.scale = 1;
	xw.lw = win.w = 28; xw.lh = win.h = 36;
	win.cw = win.ch = 8;
	win.tw = COLS * win.cw; win.th = ROWS * win.ch;
	borderpx = 2;
	#if ANYSIZE_PATCH
	win.hborderpx = win.vborderpx = 2;
	#endif
	#if ALPHA_PATCH && ALPHA_FOCUS_HIGHLIGHT_PATCH
	alphaUnfocused = 0.29f;
	#endif
	xloadcols(); palettealpha();
	configuretests(); osccolors();
	#if ALPHA_PATCH
	optionandfocus(); osccolors();
	#endif
	xw.buf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, win.w, win.h);
	xw.draw = cairo_create(xw.buf);
	resetdrawstate();
	backgroundpixels(0); backgroundpixels(1); backgroundpixels(0);
	cairo_destroy(xw.draw); cairo_surface_destroy(xw.buf); free(dc.col);
	puts("PASS: configure/ack order, pending state, repeated/empty/maximized/zero/same-size states, full dirty, alpha/OSC/focus restoration, background/border pixels");
	return 0;
}
