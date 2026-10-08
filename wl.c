/* See LICENSE for license details. */
/*
 * Wayland backend for st. Rendering is done in software with cairo into
 * wl_shm buffers, keyboard input is translated with xkbcommon.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <malloc.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <libgen.h>
#include <linux/input-event-codes.h>

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <cairo.h>
#include <cairo-ft.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "primary-selection-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"
#include "xdg-system-bell-v1-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"

char *argv0;
#include "arg.h"
#include "st.h"
#include "win.h"
#if LIGATURES_PATCH
#include "hb.h"
#endif // LIGATURES_PATCH

#if SIXEL_PATCH
#include "sixel.h"
#endif // SIXEL_PATCH

#if UNDERCURL_PATCH
/* Undercurl slope types */
enum undercurl_slope_type {
	UNDERCURL_SLOPE_ASCENDING = 0,
	UNDERCURL_SLOPE_TOP_CAP = 1,
	UNDERCURL_SLOPE_DESCENDING = 2,
	UNDERCURL_SLOPE_BOTTOM_CAP = 3
};
#endif // UNDERCURL_PATCH

#if ANYGEOMETRY_PATCH
typedef enum {
	PixelGeometry,
	CellGeometry
} Geometry;
#endif // ANYGEOMETRY_PATCH

/* modifiers */
#define XK_ANY_MOD    UINT_MAX
#define XK_NO_MOD     0
#define XK_SWITCH_MOD (1<<13|1<<14)

/* function definitions used in config.h */
static void clipcopy(const Arg *);
static void clippaste(const Arg *);
static void numlock(const Arg *);
static void selpaste(const Arg *);
static void ttysend(const Arg *);
static void zoom(const Arg *);
static void zoomabs(const Arg *);
static void zoomreset(const Arg *);

/* wayland helpers used by patches */
static struct wl_buffer *wlcreateshmbuffer(int, int, void **, size_t *);

#include "patch/st_include.h"
#include "patch/x_include.h"

/* config.h for applying patches and the configuration. */
#include "config.h"

#if CSI_22_23_PATCH
/* size of title stack */
#define TITLESTACKSIZE 8
#endif // CSI_22_23_PATCH

/* macros */
#define IS_SET(flag)		((win.mode & (flag)) != 0)
#define TRUERED(x)		(((x) & 0xff0000) >> 8)
#define TRUEGREEN(x)		(((x) & 0xff00))
#define TRUEBLUE(x)		(((x) & 0xff) << 8)

/* mouse event passed around by the pointer handlers */
enum { MousePress, MouseRelease, MouseMotion };
typedef struct {
	int type;
	int button;
	uint state;
	int x, y;     /* buffer pixel coordinates */
	uint32_t time;
} MouseEvent;

/*
 * wl_shm backed buffer.  Frames are rendered straight into the buffer that
 * will be committed (no intermediate off-screen surface); see xstartdraw().
 */
typedef struct {
	struct wl_buffer *buffer;
	void *data;
	size_t size;
	int width, height, stride;
	int busy;		/* held by the compositor */
	int dx1, dy1, dx2, dy2;	/* area changed since the compositor last saw this buffer */
	cairo_surface_t *surface;
	cairo_t *cr;
} ShmBuffer;

static inline ushort sixd_to_16bit(int);
static int xmakeglyphfontspecs(GlyphFontSpec *, const Glyph *, int, int, int);
#if LIGATURES_PATCH && WIDE_GLYPHS_PATCH
static void xdrawglyphfontspecs(const GlyphFontSpec *, Glyph, int, int, int, int, int);
#elif LIGATURES_PATCH || WIDE_GLYPHS_PATCH
static void xdrawglyphfontspecs(const GlyphFontSpec *, Glyph, int, int, int, int);
#else
static void xdrawglyphfontspecs(const GlyphFontSpec *, Glyph, int, int, int);
#endif // WIDE_GLYPHS_PATCH | LIGATURES_PATCH
#if LIGATURES_PATCH
static inline void xresetfontsettings(uint32_t mode, Font **font, int *frcflags);
#endif // LIGATURES_PATCH
void xdrawglyph(Glyph, int, int);
static void xclear(int, int, int, int);
static void xinit(int, int);
static void cresize(int, int);
static void xresize(int, int);
static void xhints(void);
static int xloadcolor(int, const char *, Color *);
static int xloadfont(Font *, FcPattern *);
static void xloadfonts(const char *, double);
static void xunloadfont(Font *);
static void xunloadfonts(void);
static void xsetenv(void);
static int evcol(MouseEvent *);
static int evrow(MouseEvent *);

static void kpress(xkb_keycode_t);
static uint buttonmask(uint);
static void brelease(MouseEvent *);
static void bpress(MouseEvent *);
static void bmotion(MouseEvent *);
static void setsel(char *, uint32_t);
static int mouseaction(MouseEvent *, uint);
static void mousesel(MouseEvent *, int);
static void mousereport(MouseEvent *);
static char *kmap(KeySym, uint);
static int match(uint, uint);

static void drawrect(Color *, int, int, int, int);
static void drawglyphs(Color *, const GlyphFontSpec *, int, int, int, int, int);
static void resetdrawstate(void);
static void resetclip(void);
void wlfillrect(const Color *, int, int, int, int);
static void adddamage(int, int, int, int);
static inline uint32_t premulpixel(const Color *);
static void present(void);
static unsigned int glyphindex(cairo_scaled_font_t *, Rune);
static void updatesize(void);
static void pastestart(int);
static void pasteread(void);
static void sendqueue_flush(int);
static uint modstate(void);

static void run(void);
static void usage(void);

/* Globals */
Term term;
DC dc;
XWindow xw;
XSelection xsel;
TermWindow win;

#if CSI_22_23_PATCH
static int tstki; /* title stack index */
static char *titlestack[TITLESTACKSIZE]; /* title stack */
#endif // CSI_22_23_PATCH

/* Font Ring Cache */
enum {
	FRC_NORMAL,
	FRC_ITALIC,
	FRC_BOLD,
	FRC_ITALICBOLD
};

typedef struct {
	cairo_scaled_font_t *font;
	int flags;
	Rune unicodep;
} Fontcache;

/* Fontcache is an array now. A new font will be appended to the array. */
static Fontcache *frc = NULL;
static int frclen = 0;
static int frccap = 0;
static char *usedfont = NULL;
static double usedfontsize = 0;
static double defaultfontsize = 0;

#if ALPHA_PATCH
static char *opt_alpha = NULL;
#endif // ALPHA_PATCH
static char *opt_class = NULL;
static char **opt_cmd  = NULL;
static char *opt_embed = NULL;
static char *opt_font  = NULL;
static char *opt_io    = NULL;
static char *opt_line  = NULL;
static char *opt_name  = NULL;
static char *opt_title = NULL;
#if WORKINGDIR_PATCH
static char *opt_dir   = NULL;
#endif // WORKINGDIR_PATCH

#if ALPHA_PATCH && ALPHA_FOCUS_HIGHLIGHT_PATCH
static int focused = 0;
#endif // ALPHA_FOCUS_HIGHLIGHT_PATCH

static uint buttons; /* bit field of pressed buttons */
#if BLINKING_CURSOR_PATCH
static int cursorblinks = 0;
#endif // BLINKING_CURSOR_PATCH
#if VISUALBELL_1_PATCH
static int bellon = 0;    /* visual bell status */
#endif // VISUALBELL_1_PATCH
#if RELATIVEBORDER_PATCH
int borderpx;
#endif // RELATIVEBORDER_PATCH

/* wayland state */
/*
 * Two buffers are enough with a persistent off-screen surface: while the
 * compositor holds one we draw into the other.  Each buffer is
 * win.w*win.h*4 bytes, so keep this small.
 */
/*
 * One buffer suffices while the compositor releases it right after the
 * commit (Hyprland does), two when it holds the displayed one; the third
 * is only touched if both others are still held, so it normally costs no
 * RSS at all.
 */
#define NBUFFERS 3
static ShmBuffer buffers[NBUFFERS];
static ShmBuffer *curbuf;	/* buffer being drawn this frame (xstartdraw..present) */
static ShmBuffer *lastbuf;	/* most recently committed buffer: holds the full picture */
static int wlevents;          /* incremented whenever an event of interest arrived */
static int ttystarted;        /* the pty exists; cresize may call ttyresize */
static int pendingw, pendingh; /* logical size from the last xdg_toplevel configure */
#if ALPHA_PATCH
static int pendingfullscreen, isfullscreen;
#endif // ALPHA_PATCH
static uint32_t seatversion;
static uint32_t compositorversion;
static int outputscale = 1;   /* integer scale when fractional scaling is unavailable */
static uint32_t fracscale120;  /* fractional scale in 1/120 units, 0 = none */

/* keyboard repeat */
static struct {
	int32_t rate;   /* repeats per second */
	int32_t delay;  /* ms */
	xkb_keycode_t key;
	int active;
	struct timespec next;
} kbrepeat = { .rate = 25, .delay = 600 };

/* mouse wheel accumulation */
static double axisacc[2];
static int axisdiscrete[2];

/* input method (zwp_text_input_v3) */
static struct {
	int active;              /* text input has entered our surface */
	char *preedit;           /* current (applied) preedit string, NULL if none */
	int pre_begin, pre_end;  /* cursor range in preedit, byte offsets, -1 = hidden */
	int row, col, ncells;    /* where the applied preedit was last drawn */
	/* pending state, applied on done */
	char *ppreedit;
	int ppre_begin, ppre_end;
	char *pcommit;
	int spotx, spoty;        /* last cursor cell reported via xximspot */
} ime = { .pre_begin = -1, .pre_end = -1, .ppre_begin = -1, .ppre_end = -1, .row = -1 };

static void textinput_setup(void);
static void textinput_setrect(int, int);
static void drawpreedit(int, int);

/* pending paste */
static struct {
	int fd;
	int first;
	int append;
} paste = { .fd = -1 };

/* queued selection transfers to other clients */
typedef struct {
	int fd;
	char *data;
	size_t len, off;
} SendJob;
#define NSENDJOBS 8
static SendJob sendjobs[NSENDJOBS];

/* per offer data: the mime type we will request */
typedef struct {
	char *mime;
	int rank;
} OfferData;

static const char *textmimes[] = {
	"text/plain;charset=utf-8",
	"UTF8_STRING",
	"text/plain",
	"TEXT",
	"STRING",
};

#include "patch/x_include.c"

/*
 * Shared memory buffers
 */
static int
createshmfile(size_t size)
{
	int fd;

#ifdef __linux__
	fd = memfd_create("st-shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd >= 0) {
		fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_SEAL);
	}
#else
	fd = -1;
#endif
	if (fd < 0) {
		char name[64];
		int retries = 100;
		do {
			snprintf(name, sizeof name, "/st-shm-%d-%ld", getpid(), random());
			fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
			if (fd >= 0) {
				shm_unlink(name);
				break;
			}
		} while (--retries > 0 && errno == EEXIST);
		if (fd < 0)
			return -1;
	}
	if (ftruncate(fd, size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

struct wl_buffer *
wlcreateshmbuffer(int width, int height, void **datap, size_t *sizep)
{
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;
	int fd, stride = width * 4;
	size_t size = (size_t)stride * height;
	void *data;

	if ((fd = createshmfile(size)) < 0)
		return NULL;
	data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return NULL;
	}
#ifdef MADV_HUGEPAGE
	/*
	 * The whole buffer is rewritten on every full-screen update; huge
	 * pages make those stores noticeably cheaper (fewer TLB misses).
	 * Purely advisory - ignored where shmem THP is disabled.
	 */
	madvise(data, size, MADV_HUGEPAGE);
#endif
	pool = wl_shm_create_pool(xw.shm, fd, size);
	buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
			WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	if (datap)
		*datap = data;
	if (sizep)
		*sizep = size;
	return buffer;
}

static void
bufferrelease(void *data, struct wl_buffer *buffer)
{
	ShmBuffer *b = data;
	b->busy = 0;
}

static const struct wl_buffer_listener bufferlistener = { bufferrelease };

/*
 * Frame pacing: a wl_surface.frame callback is requested with every commit
 * and no new frame is drawn until the compositor has signalled that it
 * consumed the previous one.  Meanwhile terminal updates simply accumulate
 * (dirty lines / damage), so under heavy output we render at the display
 * refresh rate instead of once per maxlatency.  If a compositor never
 * answers (e.g. the surface is hidden) we fall back to drawing anyway after
 * FRAME_TIMEOUT ms so the terminal keeps behaving sanely.
 */
#define FRAME_TIMEOUT 100.0
static struct wl_callback *framecb;
static struct timespec framereq;
static int presentpending;	/* damage could not be committed: no free buffer */

static void
frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
	if (cb == framecb)
		framecb = NULL;
	wl_callback_destroy(cb);
}

static const struct wl_callback_listener framelistener = { frame_done };

/* milliseconds we may still wait for the pending frame callback, or -1 */
static double
framewait(const struct timespec *nowp)
{
	struct timespec now = *nowp;
	double left;

	if (!framecb)
		return -1;
	left = FRAME_TIMEOUT - TIMEDIFF(now, framereq);
	return left > 0 ? left : -1;
}

static void
destroybuffer(ShmBuffer *b)
{
	if (b->cr)
		cairo_destroy(b->cr);
	if (b->surface)
		cairo_surface_destroy(b->surface);
	if (b->buffer)
		wl_buffer_destroy(b->buffer);
	if (b->data)
		munmap(b->data, b->size);
	if (lastbuf == b)
		lastbuf = NULL;
	if (curbuf == b)
		curbuf = NULL;
	memset(b, 0, sizeof(*b));
}

/*
 * Find a buffer of the given size that the compositor is not holding.
 * The best choice is lastbuf itself (compositors that copy on commit
 * release it at once): it already holds the complete picture, so nothing
 * needs to be copied.  Otherwise any other free buffer will do; the parts
 * of it that went stale are refreshed from lastbuf in startframe().
 */
static ShmBuffer *
getbuffer(int width, int height)
{
	ShmBuffer *b, *cand = NULL;
	uint32_t *p, pix;
	size_t n;
	int i;

	for (i = 0; i < NBUFFERS; i++) {
		b = &buffers[i];
		/* buffers of a stale size are useless: free them right away */
		if (!b->busy && b->buffer &&
		    (b->width != width || b->height != height))
			destroybuffer(b);
	}
	if (lastbuf && !lastbuf->busy) {
		cand = lastbuf;
	} else {
		for (i = 0; i < NBUFFERS; i++) {
			b = &buffers[i];
			if (b->busy)
				continue;
			cand = b;
			if (b->buffer)
				break; /* allocated and free: prefer it */
		}
	}
	if (!cand)
		return NULL;
	b = cand;
	if (!b->buffer) {
		b->buffer = wlcreateshmbuffer(width, height, &b->data, &b->size);
		if (!b->buffer)
			return NULL;
		b->width = width;
		b->height = height;
		b->stride = width * 4;
		b->dx1 = b->dy1 = 0;
		b->dx2 = width;
		b->dy2 = height;
		wl_buffer_add_listener(b->buffer, &bufferlistener, b);
		b->surface = cairo_image_surface_create_for_data(b->data,
				CAIRO_FORMAT_ARGB32, width, height, b->stride);
		b->cr = cairo_create(b->surface);
		cairo_set_antialias(b->cr, CAIRO_ANTIALIAS_NONE);
		cairo_set_operator(b->cr, CAIRO_OPERATOR_OVER);
		/*
		 * Start from the default background so that nothing can show
		 * through if some area is neither copied nor drawn.
		 */
		pix = premulpixel(&dc.col[defaultbg]);
		p = b->data;
		for (n = (size_t)width * height; n > 0; n--)
			*p++ = pix;
	}
	return b;
}

/* record that a region of the window changed */
void
adddamage(int x, int y, int w, int h)
{
	int i, x2 = x + w, y2 = y + h;

	if (w <= 0 || h <= 0)
		return;
	LIMIT(x, 0, win.w);
	LIMIT(y, 0, win.h);
	LIMIT(x2, 0, win.w);
	LIMIT(y2, 0, win.h);
	if (x2 <= x || y2 <= y)
		return;
	for (i = 0; i < NBUFFERS; i++) {
		ShmBuffer *b = &buffers[i];
		if (!b->buffer)
			continue;
		if (b->dx1 >= b->dx2 || b->dy1 >= b->dy2) {
			b->dx1 = x, b->dy1 = y, b->dx2 = x2, b->dy2 = y2;
		} else {
			b->dx1 = MIN(b->dx1, x);
			b->dy1 = MIN(b->dy1, y);
			b->dx2 = MAX(b->dx2, x2);
			b->dy2 = MAX(b->dy2, y2);
		}
		/*
		 * The window may have been resized several times since this
		 * buffer was last used; never let its damage exceed its own
		 * dimensions (it is recreated if the size differs anyway).
		 */
		b->dx2 = MIN(b->dx2, b->width);
		b->dy2 = MIN(b->dy2, b->height);
	}
}

/*
 * Is pixel row py going to be fully redrawn by this frame?  drawregion()
 * redraws every dirty terminal line across the whole window width,
 * including the side borders; the top/bottom borders are cleared together
 * with the first/last line.
 */
static int
pixelrowdirty(int py)
{
	#if ANYSIZE_PATCH
	int top = win.vborderpx;
	#else
	int top = borderpx;
	#endif // ANYSIZE_PATCH
	int row;

	if (!term.dirty || term.row <= 0 || win.ch <= 0)
		return 0;
	if (py < top)
		row = 0;
	else if (py >= top + term.row * win.ch)
		row = term.row - 1;
	else
		row = (py - top) / win.ch;
	return term.dirty[row];
}

/*
 * Begin a frame: pick a free buffer and bring the parts of it that changed
 * since it was last drawn up to date from the previous frame - except for
 * the pixel rows that are going to be redrawn anyway.  A full-screen
 * refresh therefore copies nothing; typing copies a few cells.
 */
static int
startframe(void)
{
	ShmBuffer *b;
	int x1, y1, x2, y2, py, rowbytes;

	if (curbuf)
		return 1; /* frame already in progress */
	if (!xw.configured)
		return 0; /* no buffer may be committed before the first configure */

	b = getbuffer(win.w, win.h);
	if (!b) {
		presentpending = 1; /* retried once a buffer is released */
		return 0;
	}
	presentpending = 0;

	if (lastbuf && lastbuf != b && b->dx1 < b->dx2 && b->dy1 < b->dy2) {
		x1 = b->dx1;
		y1 = b->dy1;
		x2 = MIN(b->dx2, MIN(b->width, lastbuf->width));
		y2 = MIN(b->dy2, MIN(b->height, lastbuf->height));
		rowbytes = (x2 - x1) * 4;
		if (rowbytes > 0) {
			cairo_surface_flush(lastbuf->surface);
			cairo_surface_flush(b->surface);
			for (py = y1; py < y2; py++) {
				if (pixelrowdirty(py))
					continue;
				memcpy((unsigned char *)b->data + (size_t)py * b->stride + x1 * 4,
				       (unsigned char *)lastbuf->data + (size_t)py * lastbuf->stride + x1 * 4,
				       rowbytes);
			}
			cairo_surface_mark_dirty(b->surface);
		}
	}
	/*
	 * b->damage is kept: it is what the compositor has not seen in this
	 * buffer yet, and this frame's drawing accumulates on top of it.
	 */

	curbuf = b;
	xw.buf = b->surface;
	xw.draw = b->cr;
	cairo_reset_clip(xw.draw);
	resetdrawstate();
	#if BOXDRAW_PATCH
	boxdraw_xinit(xw.draw);
	#endif // BOXDRAW_PATCH
	return 1;
}

/* commit the buffer drawn this frame */
void
present(void)
{
	ShmBuffer *b = curbuf;

	if (!b)
		return;
	curbuf = NULL;
	xw.buf = NULL;
	xw.draw = NULL;

	cairo_surface_flush(b->surface);
	wl_surface_attach(xw.win, b->buffer, 0, 0);
	if (b->dx1 < b->dx2 && b->dy1 < b->dy2) {
		wl_surface_damage_buffer(xw.win, b->dx1, b->dy1,
				b->dx2 - b->dx1, b->dy2 - b->dy1);
	}
	b->dx1 = b->dx2 = b->dy1 = b->dy2 = 0;
	b->busy = 1;
	lastbuf = b;
	if (xw.viewport) {
		wp_viewport_set_destination(xw.viewport, xw.lw, xw.lh);
	} else if (compositorversion >= 3) {
		wl_surface_set_buffer_scale(xw.win, outputscale);
	}
	if (framecb)
		wl_callback_destroy(framecb);
	framecb = wl_surface_frame(xw.win);
	wl_callback_add_listener(framecb, &framelistener, NULL);
	clock_gettime(CLOCK_MONOTONIC, &framereq);
	wl_surface_commit(xw.win);
	wl_display_flush(xw.dpy);
}

/*
 * Drawing primitives
 *
 * The cairo context carries a lot of per-call overhead (gstate, pattern
 * allocation, clip objects, scaled font lookup).  A terminal draws
 * thousands of tiny rectangles and glyph runs per frame, so:
 *  - solid rectangles are written straight into the image surface memory;
 *  - the clip rectangle, source colour and scaled font set on the context
 *    are remembered and only changed when they actually differ.
 * Everything that uses cairo_save()/cairo_restore() around its own state
 * changes leaves the tracked state intact.  Code that changes the clip
 * without save/restore must call resetclip() first.
 */
static struct {
	int clipx, clipy, clipw, cliph;	/* current clip, clipw == 0: none */
	uint32_t src;			/* current solid source, 0xAARRGGBB */
	int srcvalid;
	cairo_scaled_font_t *font;
} ds;

static void
resetdrawstate(void)
{
	memset(&ds, 0, sizeof(ds));
}

/* set a solid source without touching the tracked state (for use
 * inside cairo_save()/cairo_restore() pairs) */
static inline void
setsourceraw(const Color *c)
{
	cairo_set_source_rgba(xw.draw,
			c->color.red / 65535.0,
			c->color.green / 65535.0,
			c->color.blue / 65535.0,
			c->color.alpha / 65535.0);
}

static inline void
setsource(Color *c)
{
	if (ds.srcvalid && ds.src == c->pixel)
		return;
	setsourceraw(c);
	ds.src = c->pixel;
	ds.srcvalid = 1;
}

static void
setclip(int x, int y, int w, int h)
{
	if (ds.clipw && ds.clipx == x && ds.clipy == y &&
	    ds.clipw == w && ds.cliph == h)
		return;
	cairo_reset_clip(xw.draw);
	cairo_rectangle(xw.draw, x, y, w, h);
	cairo_clip(xw.draw);
	ds.clipx = x, ds.clipy = y, ds.clipw = w, ds.cliph = h;
}

static void
resetclip(void)
{
	if (!ds.clipw)
		return;
	cairo_reset_clip(xw.draw);
	ds.clipw = 0;
}

/* premultiplied ARGB32 pixel for a colour, rounded the way cairo does it */
static inline uint32_t
premulpixel(const Color *c)
{
	uint32_t a = c->color.alpha;
	uint32_t r, g, b;

	if (a == 0xffff)
		return 0xff000000u | (c->color.red >> 8) << 16 |
		       (c->color.green >> 8) << 8 | (c->color.blue >> 8);
	r = (c->color.red * a + 32767) / 65535;
	g = (c->color.green * a + 32767) / 65535;
	b = (c->color.blue * a + 32767) / 65535;
	return (a >> 8) << 24 | (r >> 8) << 16 | (g >> 8) << 8 | (b >> 8);
}

/*
 * Fill a rectangle with a solid colour, replacing what was there (like
 * XftDrawRect).  Writes the pixels directly, bypassing cairo.
 */
void
wlfillrect(const Color *c, int x, int y, int w, int h)
{
	unsigned char *data;
	uint32_t *row, pix;
	int stride, sw, sh, x2 = x + w, y2 = y + h, i;

	if (!xw.buf)
		return;
	sw = cairo_image_surface_get_width(xw.buf);
	sh = cairo_image_surface_get_height(xw.buf);
	x = MAX(x, 0);
	y = MAX(y, 0);
	x2 = MIN(x2, sw);
	y2 = MIN(y2, sh);
	if (x2 <= x || y2 <= y)
		return;

	cairo_surface_flush(xw.buf);
	data = cairo_image_surface_get_data(xw.buf);
	stride = cairo_image_surface_get_stride(xw.buf);
	pix = premulpixel(c);
	w = x2 - x;
	h = y2 - y;
	for (i = y; i < y2; i++) {
		int j;
		row = (uint32_t *)(data + (size_t)i * stride) + x;
		for (j = 0; j < w; j++)
			row[j] = pix;
	}
	cairo_surface_mark_dirty_rectangle(xw.buf, x, y, w, h);
}

void
drawrect(Color *c, int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0)
		return;
	wlfillrect(c, x, y, w, h);
	adddamage(x, y, w, h);
}

static cairo_glyph_t *glyphbuf;
static int glyphbufcap;

/* draw glyph runs, clipped to the given rectangle */
void
drawglyphs(Color *fg, const GlyphFontSpec *specs, int len,
           int cx, int cy, int cw, int ch)
{
	int i, n;

	if (len <= 0)
		return;
	if (glyphbufcap < len) {
		glyphbufcap = len + 64;
		glyphbuf = xrealloc(glyphbuf, glyphbufcap * sizeof(cairo_glyph_t));
	}

	setclip(cx, cy, cw, ch);
	setsource(fg);
	for (i = 0; i < len; i += n) {
		cairo_scaled_font_t *font = specs[i].font;
		for (n = 0; i + n < len && specs[i + n].font == font; n++) {
			glyphbuf[n].index = specs[i + n].glyph;
			glyphbuf[n].x = specs[i + n].x;
			glyphbuf[n].y = specs[i + n].y;
		}
		if (ds.font != font) {
			cairo_set_scaled_font(xw.draw, font);
			ds.font = font;
		}
		cairo_show_glyphs(xw.draw, glyphbuf, n);
	}
}

/*
 * Glyph index lookup with a small direct mapped cache (XftCharIndex
 * equivalent).
 */
#define GLYPHCACHE 4096
static struct {
	cairo_scaled_font_t *font;
	Rune u;
	unsigned int idx;
} glyphcache[GLYPHCACHE];

unsigned int
glyphindex(cairo_scaled_font_t *sf, Rune u)
{
	unsigned int h = ((uintptr_t)sf >> 4 ^ u * 2654435761u) % GLYPHCACHE;
	FT_Face face;
	unsigned int idx;

	if (glyphcache[h].font == sf && glyphcache[h].u == u && sf != NULL)
		return glyphcache[h].idx;

	face = cairo_ft_scaled_font_lock_face(sf);
	idx = face ? FT_Get_Char_Index(face, u) : 0;
	cairo_ft_scaled_font_unlock_face(sf);

	glyphcache[h].font = sf;
	glyphcache[h].u = u;
	glyphcache[h].idx = idx;
	return idx;
}

static void
clearglyphcache(void)
{
	memset(glyphcache, 0, sizeof(glyphcache));
}

/*
 * Selections / clipboard
 */
static void
sendjob_add(int fd, const char *text)
{
	int i;

	if (!text) {
		close(fd);
		return;
	}
	for (i = 0; i < NSENDJOBS; i++) {
		if (sendjobs[i].fd < 0 || sendjobs[i].data == NULL) {
			sendjobs[i].fd = fd;
			sendjobs[i].data = xstrdup(text);
			sendjobs[i].len = strlen(text);
			sendjobs[i].off = 0;
			fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
			/* try to write straight away */
			sendqueue_flush(fd);
			return;
		}
	}
	/* too many pending transfers */
	close(fd);
}

static void
sendjob_done(SendJob *j)
{
	close(j->fd);
	free(j->data);
	j->fd = -1;
	j->data = NULL;
	j->len = j->off = 0;
}

/* write as much as possible for the job with the given fd (or all if fd < 0) */
void
sendqueue_flush(int fd)
{
	int i;
	ssize_t n;

	for (i = 0; i < NSENDJOBS; i++) {
		SendJob *j = &sendjobs[i];
		if (j->data == NULL || (fd >= 0 && j->fd != fd))
			continue;
		while (j->off < j->len) {
			n = write(j->fd, j->data + j->off, j->len - j->off);
			if (n < 0) {
				if (errno == EAGAIN || errno == EINTR)
					break;
				sendjob_done(j);
				break;
			}
			j->off += n;
		}
		if (j->data && j->off >= j->len)
			sendjob_done(j);
	}
}

static void
datasource_target(void *data, struct wl_data_source *source, const char *mime)
{
}

static void
datasource_send(void *data, struct wl_data_source *source, const char *mime, int32_t fd)
{
	sendjob_add(fd, xsel.clipboard);
}

static void
datasource_cancelled(void *data, struct wl_data_source *source)
{
	if (xsel.clipsource == source)
		xsel.clipsource = NULL;
	wl_data_source_destroy(source);
}

static void
datasource_dnd_drop_performed(void *data, struct wl_data_source *source)
{
}

static void
datasource_dnd_finished(void *data, struct wl_data_source *source)
{
}

static void
datasource_action(void *data, struct wl_data_source *source, uint32_t action)
{
}

static const struct wl_data_source_listener datasourcelistener = {
	datasource_target,
	datasource_send,
	datasource_cancelled,
	datasource_dnd_drop_performed,
	datasource_dnd_finished,
	datasource_action,
};

static void
primsource_send(void *data, struct zwp_primary_selection_source_v1 *source,
		const char *mime, int32_t fd)
{
	sendjob_add(fd, xsel.primary);
}

static void
primsource_cancelled(void *data, struct zwp_primary_selection_source_v1 *source)
{
	if (xsel.primsource == source)
		xsel.primsource = NULL;
	zwp_primary_selection_source_v1_destroy(source);
}

static const struct zwp_primary_selection_source_v1_listener primsourcelistener = {
	primsource_send,
	primsource_cancelled,
};

static void
offerdata_addmime(OfferData *od, const char *mime)
{
	int i;

	for (i = 0; i < LEN(textmimes); i++) {
		if (strcmp(mime, textmimes[i]) == 0) {
			if (!od->mime || i < od->rank) {
				free(od->mime);
				od->mime = xstrdup(mime);
				od->rank = i;
			}
			return;
		}
	}
}

static void
offerdata_free(OfferData *od)
{
	if (!od)
		return;
	free(od->mime);
	free(od);
}

static void
dataoffer_offer(void *data, struct wl_data_offer *offer, const char *mime)
{
	offerdata_addmime(data, mime);
}

static void
dataoffer_source_actions(void *data, struct wl_data_offer *offer, uint32_t actions)
{
}

static void
dataoffer_action(void *data, struct wl_data_offer *offer, uint32_t action)
{
}

static const struct wl_data_offer_listener dataofferlistener = {
	dataoffer_offer,
	dataoffer_source_actions,
	dataoffer_action,
};

static void
datadevice_data_offer(void *data, struct wl_data_device *dev, struct wl_data_offer *offer)
{
	OfferData *od = xmalloc(sizeof(OfferData));
	od->mime = NULL;
	od->rank = INT_MAX;
	wl_data_offer_add_listener(offer, &dataofferlistener, od);
}

static void
datadevice_enter(void *data, struct wl_data_device *dev, uint32_t serial,
		struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y,
		struct wl_data_offer *offer)
{
	/* drag and drop is not supported: refuse everything */
	if (offer)
		wl_data_offer_accept(offer, serial, NULL);
}

static void
datadevice_leave(void *data, struct wl_data_device *dev)
{
}

static void
datadevice_motion(void *data, struct wl_data_device *dev, uint32_t time,
		wl_fixed_t x, wl_fixed_t y)
{
}

static void
datadevice_drop(void *data, struct wl_data_device *dev)
{
}

static void
datadevice_selection(void *data, struct wl_data_device *dev, struct wl_data_offer *offer)
{
	if (xsel.clipoffer) {
		offerdata_free(wl_data_offer_get_user_data(xsel.clipoffer));
		wl_data_offer_destroy(xsel.clipoffer);
	}
	xsel.clipoffer = offer;
}

static const struct wl_data_device_listener datadevicelistener = {
	datadevice_data_offer,
	datadevice_enter,
	datadevice_leave,
	datadevice_motion,
	datadevice_drop,
	datadevice_selection,
};

static void
primoffer_offer(void *data, struct zwp_primary_selection_offer_v1 *offer, const char *mime)
{
	offerdata_addmime(data, mime);
}

static const struct zwp_primary_selection_offer_v1_listener primofferlistener = {
	primoffer_offer,
};

static void
primdevice_data_offer(void *data, struct zwp_primary_selection_device_v1 *dev,
		struct zwp_primary_selection_offer_v1 *offer)
{
	OfferData *od = xmalloc(sizeof(OfferData));
	od->mime = NULL;
	od->rank = INT_MAX;
	zwp_primary_selection_offer_v1_add_listener(offer, &primofferlistener, od);
}

static void
primdevice_selection(void *data, struct zwp_primary_selection_device_v1 *dev,
		struct zwp_primary_selection_offer_v1 *offer)
{
	if (xsel.primoffer) {
		offerdata_free(zwp_primary_selection_offer_v1_get_user_data(xsel.primoffer));
		zwp_primary_selection_offer_v1_destroy(xsel.primoffer);
	}
	xsel.primoffer = offer;
}

static const struct zwp_primary_selection_device_v1_listener primdevicelistener = {
	primdevice_data_offer,
	primdevice_selection,
};

/* start receiving the clipboard (which == 0) or primary (which == 1) selection */
void
pastestart(int which)
{
	int fds[2];
	OfferData *od;

	if (paste.fd >= 0)
		return; /* a paste is already in progress */

	if (which == 0) {
		if (!xw.datadev || !xsel.clipoffer)
			return;
		od = wl_data_offer_get_user_data(xsel.clipoffer);
	} else {
		if (!xw.primseldev || !xsel.primoffer)
			return;
		od = zwp_primary_selection_offer_v1_get_user_data(xsel.primoffer);
	}
	if (!od || !od->mime)
		return;

	if (pipe2(fds, O_CLOEXEC) < 0)
		return;

	if (which == 0)
		wl_data_offer_receive(xsel.clipoffer, od->mime, fds[1]);
	else
		zwp_primary_selection_offer_v1_receive(xsel.primoffer, od->mime, fds[1]);
	close(fds[1]);
	wl_display_flush(xw.dpy);

	fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
	paste.fd = fds[0];
	paste.first = 1;
	paste.append = 0;
}

/* called from the main loop when the paste pipe is readable */
void
pasteread(void)
{
	static char buf[BUFSIZ];
	char *repl, *last;
	ssize_t n;

	n = read(paste.fd, buf, sizeof(buf));
	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	if (n <= 0) {
		/* end of transfer */
		#if KEYBOARDSELECT_PATCH && REFLOW_PATCH
		if (!(IS_SET(MODE_KBDSELECT) && kbds_issearchmode()))
		#endif // KEYBOARDSELECT_PATCH
		if (IS_SET(MODE_BRCKTPASTE) && !paste.first)
			ttywrite("\033[201~", 6, 0);
		close(paste.fd);
		paste.fd = -1;
		return;
	}

	#if KEYBOARDSELECT_PATCH && REFLOW_PATCH
	if (IS_SET(MODE_KBDSELECT) && kbds_issearchmode()) {
		kbds_pasteintosearch(buf, n, paste.append++);
		paste.first = 0;
		return;
	}
	#endif // KEYBOARDSELECT_PATCH

	/*
	 * As seen in getsel:
	 * Line endings are inconsistent in the terminal and GUI world
	 * copy and pasting. When receiving some selection data,
	 * replace all '\n' with '\r'.
	 * FIXME: Fix the computer world.
	 */
	repl = buf;
	last = buf + n;
	while ((repl = memchr(repl, '\n', last - repl))) {
		*repl++ = '\r';
	}

	if (IS_SET(MODE_BRCKTPASTE) && paste.first)
		ttywrite("\033[200~", 6, 0);
	ttywrite(buf, n, 1);
	paste.first = 0;
}

void
clipcopy(const Arg *dummy)
{
	free(xsel.clipboard);
	xsel.clipboard = NULL;

	if (xsel.primary != NULL) {
		xsel.clipboard = xstrdup(xsel.primary);
		if (!xw.datadevmgr || !xw.datadev)
			return;
		if (xsel.clipsource)
			wl_data_source_destroy(xsel.clipsource);
		xsel.clipsource = wl_data_device_manager_create_data_source(xw.datadevmgr);
		wl_data_source_add_listener(xsel.clipsource, &datasourcelistener, NULL);
		for (int i = 0; i < LEN(textmimes); i++)
			wl_data_source_offer(xsel.clipsource, textmimes[i]);
		wl_data_device_set_selection(xw.datadev, xsel.clipsource, xw.serial);
	}
}

void
clippaste(const Arg *dummy)
{
	#if KEYBOARDSELECT_PATCH && REFLOW_PATCH
	if (IS_SET(MODE_KBDSELECT) && !kbds_issearchmode())
		return;
	#endif // KEYBOARDSELECT_PATCH

	pastestart(0);
}

void
numlock(const Arg *dummy)
{
	win.mode ^= MODE_NUMLOCK;
}

void
selpaste(const Arg *dummy)
{
	#if KEYBOARDSELECT_PATCH && REFLOW_PATCH
	if (IS_SET(MODE_KBDSELECT) && !kbds_issearchmode())
		return;
	#endif // KEYBOARDSELECT_PATCH

	pastestart(1);
}

void
ttysend(const Arg *arg)
{
	ttywrite(arg->s, strlen(arg->s), 1);
}

void
zoom(const Arg *arg)
{
	Arg larg;

	larg.f = usedfontsize + arg->f;
	#if SIXEL_PATCH
	if (larg.f >= 1.0)
		zoomabs(&larg);
	#else
	zoomabs(&larg);
	#endif // SIXEL_PATCH
}

void
zoomabs(const Arg *arg)
{
	#if SIXEL_PATCH
	int i;
	ImageList *im;
	#endif // SIXEL_PATCH

	xunloadfonts();
	xloadfonts(usedfont, arg->f);

	#if SIXEL_PATCH
	/* delete old pixmaps so that xfinishdraw() can create new scaled ones */
	for (im = term.images, i = 0; i < 2; i++, im = term.images_alt) {
		for (; im; im = im->next) {
			if (im->pixmap)
				cairo_surface_destroy((cairo_surface_t *)im->pixmap);
			im->pixmap = NULL;
			im->clipmask = NULL;
		}
	}
	#endif // SIXEL_PATCH

	cresize(0, 0);
	redraw();
	xhints();
}

void
zoomreset(const Arg *arg)
{
	Arg larg;

	if (defaultfontsize > 0) {
		larg.f = defaultfontsize;
		zoomabs(&larg);
	}
}

int
evcol(MouseEvent *e)
{
	#if ANYSIZE_PATCH
	int x = e->x - win.hborderpx;
	#else
	int x = e->x - borderpx;
	#endif // ANYSIZE_PATCH
	LIMIT(x, 0, win.tw - 1);
	return x / win.cw;
}

int
evrow(MouseEvent *e)
{
	#if ANYSIZE_PATCH
	int y = e->y - win.vborderpx;
	#else
	int y = e->y - borderpx;
	#endif // ANYSIZE_PATCH
	LIMIT(y, 0, win.th - 1);
	return y / win.ch;
}

uint
buttonmask(uint button)
{
	return button == Button1 ? Button1Mask
		: button == Button2 ? Button2Mask
		: button == Button3 ? Button3Mask
		: button == Button4 ? Button4Mask
		: button == Button5 ? Button5Mask
		: 0;
}

int
mouseaction(MouseEvent *e, uint release)
{
	MouseShortcut *ms;
	int screen = tisaltscr() ? S_ALT : S_PRI;

	/* ignore Button<N>mask for Button<N> - it's set on release */
	uint state = e->state & ~buttonmask(e->button);

	for (ms = mshortcuts; ms < mshortcuts + LEN(mshortcuts); ms++) {
		if (ms->release == release &&
				ms->button == e->button &&
				(!ms->screen || (ms->screen == screen)) &&
				(match(ms->mod, state) ||  /* exact or forced */
				 match(ms->mod, state & ~forcemousemod))) {
			ms->func(&(ms->arg));
			return 1;
		}
	}

	return 0;
}

void
mousesel(MouseEvent *e, int done)
{
	int type, seltype = SEL_REGULAR;
	uint state = e->state & ~(Button1Mask | forcemousemod);

	#if KEYBOARDSELECT_PATCH && REFLOW_PATCH
	if (kbds_isselectmode())
		return;
	#endif // KEYBOARDSELECT_PATCH

	for (type = 1; type < LEN(selmasks); ++type) {
		if (match(selmasks[type], state)) {
			seltype = type;
			break;
		}
	}
	selextend(evcol(e), evrow(e), seltype, done);
	if (done)
		setsel(getsel(), e->time);
}

void
mousereport(MouseEvent *e)
{
	int len, btn, code;
	int x = evcol(e), y = evrow(e);
	int state = e->state;
	char buf[40];
	static int ox, oy;

	if (e->type == MouseMotion) {
		if (x == ox && y == oy)
			return;
		if (!IS_SET(MODE_MOUSEMOTION) && !IS_SET(MODE_MOUSEMANY))
			return;
		/* MODE_MOUSEMOTION: no reporting if no button is pressed */
		if (IS_SET(MODE_MOUSEMOTION) && buttons == 0)
			return;

		/* Set btn to lowest-numbered pressed button, or 12 if no
		 * buttons are pressed. */
		for (btn = 1; btn <= 11 && !(buttons & (1<<(btn-1))); btn++)
			;
		code = 32;
	} else {
		btn = e->button;
		/* Only buttons 1 through 11 can be encoded */
		if (btn < 1 || btn > 11)
			return;
		if (e->type == MouseRelease) {
			/* MODE_MOUSEX10: no button release reporting */
			if (IS_SET(MODE_MOUSEX10))
				return;
			/* Don't send release events for the scroll wheel */
			if (btn == 4 || btn == 5)
				return;
		}
		code = 0;
	}

	ox = x;
	oy = y;

	/* Encode btn into code. If no button is pressed for a motion event in
	 * MODE_MOUSEMANY, then encode it as a release. */
	if ((!IS_SET(MODE_MOUSESGR) && e->type == MouseRelease) || btn == 12)
		code += 3;
	else if (btn >= 8)
		code += 128 + btn - 8;
	else if (btn >= 4)
		code += 64 + btn - 4;
	else
		code += btn - 1;

	if (!IS_SET(MODE_MOUSEX10)) {
		code += ((state & ShiftMask  ) ?  4 : 0)
		      + ((state & Mod1Mask   ) ?  8 : 0) /* meta key: alt */
		      + ((state & ControlMask) ? 16 : 0);
	}

	if (IS_SET(MODE_MOUSESGR)) {
		len = snprintf(buf, sizeof(buf), "\033[<%d;%d;%d%c",
				code, x+1, y+1,
				e->type == MouseRelease ? 'm' : 'M');
	} else if (x < 223 && y < 223) {
		len = snprintf(buf, sizeof(buf), "\033[M%c%c%c",
				32+code, 32+x+1, 32+y+1);
	} else {
		return;
	}

	ttywrite(buf, len, 0);
}

void
bpress(MouseEvent *e)
{
	int btn = e->button;
	struct timespec now;
	int snap;

	if (1 <= btn && btn <= 11)
		buttons |= 1 << (btn-1);

	if (IS_SET(MODE_MOUSE) && !(e->state & forcemousemod)) {
		mousereport(e);
		return;
	}

	if (mouseaction(e, 0))
		return;

	if (btn == Button1) {
		/*
		 * If the user clicks below predefined timeouts specific
		 * snapping behaviour is exposed.
		 */
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (TIMEDIFF(now, xsel.tclick2) <= tripleclicktimeout) {
			snap = SNAP_LINE;
		} else if (TIMEDIFF(now, xsel.tclick1) <= doubleclicktimeout) {
			snap = SNAP_WORD;
		} else {
			snap = 0;
		}
		xsel.tclick2 = xsel.tclick1;
		xsel.tclick1 = now;

		#if KEYBOARDSELECT_PATCH && REFLOW_PATCH
		if (kbds_isselectmode())
			return;
		#endif // KEYBOARDSELECT_PATCH

		selstart(evcol(e), evrow(e), snap);

		#if OPENURLONCLICK_PATCH
		clearurl();
		url_click = 1;
		#endif // OPENURLONCLICK_PATCH
	}
}

void
xclipcopy(void)
{
	clipcopy(NULL);
}

void
setsel(char *str, uint32_t t)
{
	if (!str)
		return;

	free(xsel.primary);
	xsel.primary = str;

	if (xw.primselmgr && xw.primseldev) {
		if (xsel.primsource)
			zwp_primary_selection_source_v1_destroy(xsel.primsource);
		xsel.primsource = zwp_primary_selection_device_manager_v1_create_source(xw.primselmgr);
		zwp_primary_selection_source_v1_add_listener(xsel.primsource, &primsourcelistener, NULL);
		for (int i = 0; i < LEN(textmimes); i++)
			zwp_primary_selection_source_v1_offer(xsel.primsource, textmimes[i]);
		zwp_primary_selection_device_v1_set_selection(xw.primseldev, xsel.primsource, xw.serial);
	}

	#if CLIPBOARD_PATCH
	clipcopy(NULL);
	#endif // CLIPBOARD_PATCH
}

void
xsetsel(char *str)
{
	setsel(str, xw.serial);
}

void
brelease(MouseEvent *e)
{
	int btn = e->button;

	if (1 <= btn && btn <= 11)
		buttons &= ~(1 << (btn-1));

	if (IS_SET(MODE_MOUSE) && !(e->state & forcemousemod)) {
		mousereport(e);
		return;
	}

	if (mouseaction(e, 1))
		return;

	if (btn == Button1) {
		mousesel(e, 1);
		#if OPENURLONCLICK_PATCH
		if (url_click && e->state & url_opener_modkey)
			openUrlOnClick(evcol(e), evrow(e), url_opener);
		#endif // OPENURLONCLICK_PATCH
	}

	#if RIGHTCLICKTOPLUMB_PATCH
	else if (btn == Button3)
		plumb(xsel.primary);
	#endif // RIGHTCLICKTOPLUMB_PATCH
}

void
bmotion(MouseEvent *e)
{
	#if HIDECURSOR_PATCH
	if (!xw.pointerisvisible) {
		wlsetpointer(POINTER_TEXT);
		xw.pointerisvisible = 1;
		if (!IS_SET(MODE_MOUSEMANY))
			xsetpointermotion(0);
	}
	#endif // HIDECURSOR_PATCH
	#if OPENURLONCLICK_PATCH
	if (!IS_SET(MODE_MOUSE)) {
		if (!(e->state & Button1Mask) && detecturl(evcol(e), evrow(e), 1))
			wlsetpointer(POINTER_LINK);
		else
			wlsetpointer(POINTER_TEXT);
	}
	url_click = 0;
	#endif // OPENURLONCLICK_PATCH

	if (IS_SET(MODE_MOUSE) && !(e->state & forcemousemod)) {
		mousereport(e);
		return;
	}

	mousesel(e, 0);
}

void
cresize(int width, int height)
{
	int col, row;

	if (width != 0)
		win.w = width;
	if (height != 0)
		win.h = height;

	col = (win.w - 2 * borderpx) / win.cw;
	row = (win.h - 2 * borderpx) / win.ch;
	col = MAX(2, col);
	row = MAX(1, row);

	#if ANYSIZE_PATCH
	win.hborderpx = (win.w - col * win.cw) / 2;
	win.vborderpx = (win.h - row * win.ch) / 2;
	#endif // ANYSIZE_PATCH

	tresize(col, row);
	xresize(col, row);
	if (ttystarted)
		ttyresize(win.tw, win.th);
	/*
	 * tresize() (reflow) frees thousands of scrollback lines and a large
	 * temporary buffer; hand the resulting holes back to the kernel so RSS
	 * does not ratchet upwards with every resize.
	 */
	malloc_trim(0);
}

void
xresize(int col, int row)
{
	win.tw = col * win.cw;
	win.th = row * win.ch;

	/*
	 * Buffers of the old size are recreated (fully damaged, filled with
	 * the background) by getbuffer() on the next frame; every line is
	 * redrawn because tresize() marked the whole terminal dirty.
	 */
	tfulldirt();

	/* resize to new width */
	#if LIGATURES_PATCH
	xw.specbuf = xrealloc(xw.specbuf, col * sizeof(GlyphFontSpec) * 4);
	xw.specseq = xrealloc(xw.specseq, col * sizeof(GlyphFontSeq));
	#else
	xw.specbuf = xrealloc(xw.specbuf, col * sizeof(GlyphFontSpec));
	#endif // LIGATURES_PATCH
}

ushort
sixd_to_16bit(int x)
{
	return x == 0 ? 0 : 0x3737 + 0x2828 * x;
}

/*
 * Colour name parsing (subset of XParseColor / X11 rgb.txt)
 */
static const struct {
	const char *name;
	unsigned char r, g, b;
} colornames[] = {
	{ "black",       0,   0,   0 },
	{ "white",     255, 255, 255 },
	{ "red",       255,   0,   0 },
	{ "red1",      255,   0,   0 },
	{ "red2",      238,   0,   0 },
	{ "red3",      205,   0,   0 },
	{ "red4",      139,   0,   0 },
	{ "green",       0, 255,   0 },
	{ "green1",      0, 255,   0 },
	{ "green2",      0, 238,   0 },
	{ "green3",      0, 205,   0 },
	{ "green4",      0, 139,   0 },
	{ "blue",        0,   0, 255 },
	{ "blue1",       0,   0, 255 },
	{ "blue2",       0,   0, 238 },
	{ "blue3",       0,   0, 205 },
	{ "blue4",       0,   0, 139 },
	{ "yellow",    255, 255,   0 },
	{ "yellow1",   255, 255,   0 },
	{ "yellow2",   238, 238,   0 },
	{ "yellow3",   205, 205,   0 },
	{ "yellow4",   139, 139,   0 },
	{ "magenta",   255,   0, 255 },
	{ "magenta1",  255,   0, 255 },
	{ "magenta2",  238,   0, 238 },
	{ "magenta3",  205,   0, 205 },
	{ "magenta4",  139,   0, 139 },
	{ "cyan",        0, 255, 255 },
	{ "cyan1",       0, 255, 255 },
	{ "cyan2",       0, 238, 238 },
	{ "cyan3",       0, 205, 205 },
	{ "cyan4",       0, 139, 139 },
	{ "gray",      190, 190, 190 },
	{ "grey",      190, 190, 190 },
	{ "orange",    255, 165,   0 },
	{ "purple",    160,  32, 240 },
	{ "brown",     165,  42,  42 },
	{ "pink",      255, 192, 203 },
	{ "navy",        0,   0, 128 },
	{ "maroon",    176,  48,  96 },
	{ "olive",     128, 128,   0 },
	{ "teal",        0, 128, 128 },
	{ "silver",    192, 192, 192 },
	{ "gold",      255, 215,   0 },
	{ "lightgray", 211, 211, 211 },
	{ "lightgrey", 211, 211, 211 },
	{ "darkgray",  169, 169, 169 },
	{ "darkgrey",  169, 169, 169 },
	{ "lightblue", 173, 216, 230 },
	{ "darkblue",    0,   0, 139 },
	{ "darkgreen",   0, 100,   0 },
	{ "darkred",   139,   0,   0 },
};

static int
hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* parse n hex digits into a 16 bit channel (left aligned like X11) */
static int
hexchannel(const char *s, int n, unsigned short *out)
{
	unsigned int v = 0;
	int i, d;

	if (n < 1 || n > 4)
		return 0;
	for (i = 0; i < n; i++) {
		if ((d = hexval(s[i])) < 0)
			return 0;
		v = v << 4 | d;
	}
	*out = v << (16 - 4 * n);
	return 1;
}

static int
parsecolor(const char *name, RenderColor *c)
{
	char buf[64], *p, *q;
	size_t len, i, n;
	int pct;

	c->alpha = 0xffff;

	/* strip spaces, lower case */
	for (p = buf, q = (char *)name; *q && p < buf + sizeof(buf) - 1; q++) {
		if (*q == ' ' || *q == '\t')
			continue;
		*p++ = (*q >= 'A' && *q <= 'Z') ? *q + 32 : *q;
	}
	*p = '\0';
	len = strlen(buf);

	if (buf[0] == '#') {
		n = (len - 1) / 3;
		if (n < 1 || n > 4 || (len - 1) % 3 != 0)
			return 0;
		return hexchannel(buf + 1, n, &c->red) &&
		       hexchannel(buf + 1 + n, n, &c->green) &&
		       hexchannel(buf + 1 + 2 * n, n, &c->blue);
	}
	if (strncmp(buf, "rgb:", 4) == 0) {
		unsigned short *ch[3] = { &c->red, &c->green, &c->blue };
		p = buf + 4;
		for (i = 0; i < 3; i++) {
			q = strchr(p, '/');
			n = q ? (size_t)(q - p) : strlen(p);
			if ((i < 2 && !q) || (i == 2 && q))
				return 0;
			if (!hexchannel(p, n, ch[i]))
				return 0;
			p = q + 1;
		}
		return 1;
	}
	if ((strncmp(buf, "gray", 4) == 0 || strncmp(buf, "grey", 4) == 0) &&
	    buf[4] >= '0' && buf[4] <= '9') {
		pct = atoi(buf + 4);
		if (pct < 0 || pct > 100)
			return 0;
		c->red = c->green = c->blue = (int)(pct * 255 / 100.0 + 0.5) * 0x101;
		return 1;
	}
	for (i = 0; i < LEN(colornames); i++) {
		if (strcmp(buf, colornames[i].name) == 0) {
			c->red = colornames[i].r * 0x101;
			c->green = colornames[i].g * 0x101;
			c->blue = colornames[i].b * 0x101;
			return 1;
		}
	}
	return 0;
}

static void
setpixel(Color *c)
{
	c->pixel = (unsigned long)(c->color.alpha >> 8) << 24 |
	           (c->color.red >> 8) << 16 |
	           (c->color.green >> 8) << 8 |
	           (c->color.blue >> 8);
}

int
xloadcolor(int i, const char *name, Color *ncolor)
{
	RenderColor color = { .alpha = 0xffff };

	if (!name) {
		if (BETWEEN(i, 16, 255)) { /* 256 color */
			if (i < 6*6*6+16) { /* same colors as xterm */
				color.red   = sixd_to_16bit( ((i-16)/36)%6 );
				color.green = sixd_to_16bit( ((i-16)/6) %6 );
				color.blue  = sixd_to_16bit( ((i-16)/1) %6 );
			} else { /* greyscale */
				color.red = 0x0808 + 0x0a0a * (i - (6*6*6+16));
				color.green = color.blue = color.red;
			}
			ncolor->color = color;
			setpixel(ncolor);
			return 1;
		} else
			name = colorname[i];
	}

	if (!name || !parsecolor(name, &color))
		return 0;
	ncolor->color = color;
	setpixel(ncolor);
	return 1;
}

#if ALPHA_PATCH
static void
applyalpha(Color *c, float a)
{
	c->color.alpha = (unsigned short)(0xffff * a);
	setpixel(c);
}

static float
effectivealpha(void)
{
	if (opt_alpha)
		alpha = strtof(opt_alpha, NULL);
	/* Fullscreen overrides rendering, never the user's configured opacity. */
	if (isfullscreen)
		return 1.0f;
	#if ALPHA_FOCUS_HIGHLIGHT_PATCH
	return focused ? alpha : alphaUnfocused;
	#else
	return alpha;
	#endif // ALPHA_FOCUS_HIGHLIGHT_PATCH
}

void
xloadalpha(void)
{
	float a = effectivealpha();

	applyalpha(&dc.col[defaultbg], a);
	#if SELECTION_COLORS_PATCH && SELECTIONBG_ALPHA_PATCH
	applyalpha(&dc.col[selectionbg], a);
	#endif // SELECTION_COLORS_PATCH && SELECTIONBG_ALPHA_PATCH
}
#endif // ALPHA_PATCH

#if ALPHA_PATCH && ALPHA_FOCUS_HIGHLIGHT_PATCH
void
xloadcols(void)
{
	static int loaded;

	if (!loaded) {
		dc.collen = 1 + (defaultbg = MAX(LEN(colorname), 256));
		dc.col = xmalloc((dc.collen) * sizeof(Color));
	}

	for (int i = 0; i+1 < dc.collen; ++i)
		if (!xloadcolor(i, NULL, &dc.col[i])) {
			if (colorname[i])
				die("could not allocate color '%s'\n", colorname[i]);
			else
				die("could not allocate color %d\n", i);
		}
	if (dc.collen) // cannot die, as the color is already loaded.
		xloadcolor(focused ? bg : bgUnfocused, NULL, &dc.col[defaultbg]);

	xloadalpha();
	loaded = 1;
}
#else
void
xloadcols(void)
{
	int i;
	static int loaded;

	if (!loaded) {
		dc.collen = MAX(LEN(colorname), 256);
		dc.col = xmalloc(dc.collen * sizeof(Color));
	}

	for (i = 0; i < dc.collen; i++)
		if (!xloadcolor(i, NULL, &dc.col[i])) {
			if (colorname[i])
				die("could not allocate color '%s'\n", colorname[i]);
			else
				die("could not allocate color %d\n", i);
		}
	#if ALPHA_PATCH
	xloadalpha();
	#endif // ALPHA_PATCH
	loaded = 1;
}
#endif // ALPHA_FOCUS_HIGHLIGHT_PATCH

int
xgetcolor(int x, unsigned char *r, unsigned char *g, unsigned char *b)
{
	if (!BETWEEN(x, 0, dc.collen - 1))
		return 1;

	*r = dc.col[x].color.red >> 8;
	*g = dc.col[x].color.green >> 8;
	*b = dc.col[x].color.blue >> 8;

	return 0;
}

int
xsetcolorname(int x, const char *name)
{
	Color ncolor;

	if (!BETWEEN(x, 0, dc.collen - 1))
		return 1;

	if (!xloadcolor(x, name, &ncolor))
		return 1;

	dc.col[x] = ncolor;

	#if ALPHA_PATCH
	if (x == defaultbg
	    #if SELECTION_COLORS_PATCH && SELECTIONBG_ALPHA_PATCH
	    || x == selectionbg
	    #endif // SELECTION_COLORS_PATCH && SELECTIONBG_ALPHA_PATCH
	)
		xloadalpha();
	#endif // ALPHA_PATCH
	return 0;
}

/*
 * Absolute coordinates.
 */
void
xclear(int x1, int y1, int x2, int y2)
{
	#if INVERT_PATCH
	Color c;
	c = dc.col[IS_SET(MODE_REVERSE)? defaultfg : defaultbg];
	if (invertcolors) {
		c = invertedcolor(&c);
	}
	drawrect(&c, x1, y1, x2-x1, y2-y1);
	#else
	drawrect(&dc.col[IS_SET(MODE_REVERSE)? defaultfg : defaultbg],
			x1, y1, x2-x1, y2-y1);
	#endif // INVERT_PATCH
}

void
xclearwin(void)
{
	xclear(0, 0, win.w, win.h);
}

void
xhints(void)
{
	int minw, minh;

	if (!xw.toplevel)
		return;

	/* minimum size in logical pixels */
	minw = ceil((win.cw + 2 * borderpx) / xw.scale);
	minh = ceil((win.ch + 2 * borderpx) / xw.scale);
	if (xw.isfixed) {
		xdg_toplevel_set_min_size(xw.toplevel, xw.lw, xw.lh);
		xdg_toplevel_set_max_size(xw.toplevel, xw.lw, xw.lh);
	} else {
		xdg_toplevel_set_min_size(xw.toplevel, minw, minh);
		xdg_toplevel_set_max_size(xw.toplevel, 0, 0);
	}
}

/*
 * Fonts
 */

/* defaults Xft would have applied to the pattern */
static void
xdefaultsubstitute(FcPattern *pattern)
{
	double d;
	FcBool b;
	int i;

	if (FcPatternGetDouble(pattern, FC_DPI, 0, &d) != FcResultMatch)
		FcPatternAddDouble(pattern, FC_DPI, 96.0);
	if (FcPatternGetBool(pattern, FC_ANTIALIAS, 0, &b) != FcResultMatch)
		FcPatternAddBool(pattern, FC_ANTIALIAS, FcTrue);
	if (FcPatternGetBool(pattern, FC_HINTING, 0, &b) != FcResultMatch)
		FcPatternAddBool(pattern, FC_HINTING, FcTrue);
	if (FcPatternGetInteger(pattern, FC_HINT_STYLE, 0, &i) != FcResultMatch)
		FcPatternAddInteger(pattern, FC_HINT_STYLE, FC_HINT_SLIGHT);
	if (FcPatternGetInteger(pattern, FC_RGBA, 0, &i) != FcResultMatch)
		FcPatternAddInteger(pattern, FC_RGBA, FC_RGBA_UNKNOWN);
}

/* create a cairo scaled font for a fontconfig match at the given pixel size */
static cairo_scaled_font_t *
opencairofont(FcPattern *match, double pixelsize)
{
	cairo_font_face_t *face;
	cairo_matrix_t fm, ctm;
	cairo_font_options_t *fo;
	cairo_scaled_font_t *sf;
	FcBool aa = FcTrue;
	int rgba = FC_RGBA_UNKNOWN, hs = FC_HINT_SLIGHT;

	face = cairo_ft_font_face_create_for_pattern(match);
	if (cairo_font_face_status(face) != CAIRO_STATUS_SUCCESS) {
		cairo_font_face_destroy(face);
		return NULL;
	}

	cairo_matrix_init_scale(&fm, pixelsize, pixelsize);
	cairo_matrix_init_identity(&ctm);

	fo = cairo_font_options_create();
	cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_ON);
	FcPatternGetBool(match, FC_ANTIALIAS, 0, &aa);
	FcPatternGetInteger(match, FC_RGBA, 0, &rgba);
	FcPatternGetInteger(match, FC_HINT_STYLE, 0, &hs);
	if (!aa)
		cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_NONE);
	else if (rgba == FC_RGBA_RGB || rgba == FC_RGBA_BGR ||
	         rgba == FC_RGBA_VRGB || rgba == FC_RGBA_VBGR)
		cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_SUBPIXEL);
	else
		cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_GRAY);
	switch (hs) {
	case FC_HINT_NONE:   cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_NONE); break;
	case FC_HINT_SLIGHT: cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_SLIGHT); break;
	case FC_HINT_MEDIUM: cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_MEDIUM); break;
	case FC_HINT_FULL:   cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_FULL); break;
	}

	sf = cairo_scaled_font_create(face, &fm, &ctm, fo);
	cairo_font_options_destroy(fo);
	cairo_font_face_destroy(face);
	if (cairo_scaled_font_status(sf) != CAIRO_STATUS_SUCCESS) {
		cairo_scaled_font_destroy(sf);
		return NULL;
	}
	return sf;
}

/* pixel size a pattern asks for, scaled for the output */
static double
patternpixelsize(FcPattern *pattern)
{
	double size;

	if (FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &size) != FcResultMatch)
		size = 12;
	return size;
}

int
xloadfont(Font *f, FcPattern *pattern)
{
	FcPattern *configured;
	FcPattern *match;
	FcResult result;
	cairo_font_extents_t fext;
	cairo_text_extents_t text;
	double pixelsize;
	int wantattr, haveattr;

	/*
	 * Manually configure instead of calling XftMatchFont
	 * so that we can use the configured pattern for
	 * "missing glyph" lookups.
	 */
	configured = FcPatternDuplicate(pattern);
	if (!configured)
		return 1;

	/* apply the output scale to the requested pixel size */
	pixelsize = patternpixelsize(configured) * xw.scale;
	FcPatternDel(configured, FC_PIXEL_SIZE);
	FcPatternDel(configured, FC_SIZE);
	FcPatternAddDouble(configured, FC_PIXEL_SIZE, pixelsize);

	FcConfigSubstitute(NULL, configured, FcMatchPattern);
	xdefaultsubstitute(configured);
	FcDefaultSubstitute(configured);

	match = FcFontMatch(NULL, configured, &result);
	if (!match) {
		FcPatternDestroy(configured);
		return 1;
	}

	FcPatternGetDouble(match, FC_PIXEL_SIZE, 0, &pixelsize);
	if (!(f->match = opencairofont(match, pixelsize))) {
		FcPatternDestroy(configured);
		FcPatternDestroy(match);
		return 1;
	}

	if ((FcPatternGetInteger(pattern, "slant", 0, &wantattr) ==
	    FcResultMatch)) {
		/*
		 * Check if fontconfig was unable to find a font with the
		 * appropriate slant but gave us one anyway. Try to mitigate.
		 */
		if ((FcPatternGetInteger(match, "slant", 0,
		    &haveattr) != FcResultMatch) || haveattr < wantattr) {
			f->badslant = 1;
			fputs("font slant does not match\n", stderr);
		}
	}

	if ((FcPatternGetInteger(pattern, "weight", 0, &wantattr) ==
	    FcResultMatch)) {
		if ((FcPatternGetInteger(match, "weight", 0,
		    &haveattr) != FcResultMatch) || haveattr != wantattr) {
			f->badweight = 1;
			fputs("font weight does not match\n", stderr);
		}
	}
	FcPatternDestroy(match);

	cairo_scaled_font_extents(f->match, &fext);
	cairo_scaled_font_text_extents(f->match, ascii_printable, &text);

	f->set = NULL;
	f->pattern = configured;

	f->ascent = lround(fext.ascent);
	f->descent = lround(fext.descent);
	f->lbearing = 0;
	f->rbearing = lround(fext.max_x_advance);

	f->height = f->ascent + f->descent;
	#if WIDE_GLYPH_SPACING_PATCH
	f->width = ceil((text.x_advance > 18 * strlen(ascii_printable) ? text.x_advance / 3 : text.x_advance) / strlen(ascii_printable));
	#else
	f->width = ceil(text.x_advance / strlen(ascii_printable));
	#endif // WIDE_GLYPH_SPACING_PATCH

	return 0;
}

void
xloadfonts(const char *fontstr, double fontsize)
{
	FcPattern *pattern;
	double fontval, dpi;

	pattern = FcNameParse((const FcChar8 *)fontstr);

	if (!pattern)
		die("can't open font %s\n", fontstr);

	if (fontsize > 1) {
		FcPatternDel(pattern, FC_PIXEL_SIZE);
		FcPatternDel(pattern, FC_SIZE);
		FcPatternAddDouble(pattern, FC_PIXEL_SIZE, (double)fontsize);
		usedfontsize = fontsize;
	} else {
		if (FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &fontval) ==
				FcResultMatch) {
			usedfontsize = fontval;
		} else if (FcPatternGetDouble(pattern, FC_SIZE, 0, &fontval) ==
				FcResultMatch) {
			/* convert points to pixels the way fontconfig would */
			if (FcPatternGetDouble(pattern, FC_DPI, 0, &dpi) != FcResultMatch)
				dpi = 96.0;
			usedfontsize = fontval * dpi / 72.0;
			FcPatternDel(pattern, FC_SIZE);
			FcPatternAddDouble(pattern, FC_PIXEL_SIZE, usedfontsize);
		} else {
			/*
			 * Default font size is 12, if none given. This is to
			 * have a known usedfontsize value.
			 */
			FcPatternAddDouble(pattern, FC_PIXEL_SIZE, 12);
			usedfontsize = 12;
		}
		defaultfontsize = usedfontsize;
	}

	if (xloadfont(&dc.font, pattern))
		die("can't open font %s\n", fontstr);

	/* Setting character width and height. */
	win.cw = ceilf(dc.font.width * cwscale);
	win.ch = ceilf(dc.font.height * chscale);
	#if VERTCENTER_PATCH
	win.cyo = ceilf(dc.font.height * (chscale - 1) / 2);
	#endif // VERTCENTER_PATCH

	#if RELATIVEBORDER_PATCH
	borderpx = (int) ceilf(((float)borderperc / 100) * win.cw);
	#endif // RELATIVEBORDER_PATCH
	FcPatternDel(pattern, FC_SLANT);
	#if !DISABLE_ITALIC_FONTS_PATCH
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ITALIC);
	#endif // DISABLE_ITALIC_FONTS_PATCH
	if (xloadfont(&dc.ifont, pattern))
		die("can't open font %s\n", fontstr);

	FcPatternDel(pattern, FC_WEIGHT);
	#if !DISABLE_BOLD_FONTS_PATCH
	FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_BOLD);
	#endif // DISABLE_BOLD_FONTS_PATCH
	if (xloadfont(&dc.ibfont, pattern))
		die("can't open font %s\n", fontstr);

	FcPatternDel(pattern, FC_SLANT);
	#if !DISABLE_ROMAN_FONTS_PATCH
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
	#endif // DISABLE_ROMAN_FONTS_PATCH
	if (xloadfont(&dc.bfont, pattern))
		die("can't open font %s\n", fontstr);

	FcPatternDestroy(pattern);

	#if FONT2_PATCH
	/* Seed fallback priority on startup and every zoom/output-scale reload. */
	xloadsparefonts();
	#endif // FONT2_PATCH
}

void
xunloadfont(Font *f)
{
	cairo_scaled_font_destroy(f->match);
	FcPatternDestroy(f->pattern);
	if (f->set)
		FcFontSetDestroy(f->set);
	f->match = NULL;
	f->pattern = NULL;
	f->set = NULL;
}

void
xunloadfonts(void)
{
	#if LIGATURES_PATCH
	/* Clear Harfbuzz font cache. */
	hbunloadfonts();
	#endif // LIGATURES_PATCH

	/* Free the loaded fonts in the font cache.  */
	while (frclen > 0)
		cairo_scaled_font_destroy(frc[--frclen].font);

	xunloadfont(&dc.font);
	xunloadfont(&dc.bfont);
	xunloadfont(&dc.ifont);
	xunloadfont(&dc.ibfont);
	clearglyphcache();
}

/*
 * Wayland listeners
 */
static void
noop(void)
{
}

/* xdg_wm_base */
static void
wmbase_ping(void *data, struct xdg_wm_base *wmbase, uint32_t serial)
{
	xdg_wm_base_pong(wmbase, serial);
}

static const struct xdg_wm_base_listener wmbaselistener = { wmbase_ping };

/* xdg_surface */
static void
xdgsurface_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
	xdg_surface_ack_configure(surface, serial);

	#if ALPHA_PATCH
	if (isfullscreen != pendingfullscreen) {
		isfullscreen = pendingfullscreen;
		/* Preserve OSC colors and repaint even for a state-only configure. */
		xloadalpha();
		tfulldirt();
	}
	#endif // ALPHA_PATCH

	if (pendingw > 0 && pendingh > 0 &&
	    (pendingw != xw.lw || pendingh != xw.lh)) {
		xw.lw = pendingw;
		xw.lh = pendingh;
	}
	if (!xw.configured) {
		xw.configured = 1;
		win.mode |= MODE_VISIBLE;
	}
	updatesize();
	wlevents++;
}

static const struct xdg_surface_listener xdgsurfacelistener = { xdgsurface_configure };

/* xdg_toplevel */
static void
toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t w, int32_t h,
		struct wl_array *states)
{
	pendingw = w;
	pendingh = h;
	#if ALPHA_PATCH
	uint32_t *state;

	pendingfullscreen = 0;
	wl_array_for_each(state, states)
		if (*state == XDG_TOPLEVEL_STATE_FULLSCREEN)
			pendingfullscreen = 1;
	#endif // ALPHA_PATCH
}

static void
toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
	ttyhangup();
	exit(0);
}

static void
toplevel_configure_bounds(void *data, struct xdg_toplevel *toplevel, int32_t w, int32_t h)
{
}

static void
toplevel_wm_capabilities(void *data, struct xdg_toplevel *toplevel, struct wl_array *caps)
{
}

static const struct xdg_toplevel_listener toplevellistener = {
	toplevel_configure,
	toplevel_close,
	toplevel_configure_bounds,
	toplevel_wm_capabilities,
};

/* xdg_toplevel_decoration */
static void
decoration_configure(void *data, struct zxdg_toplevel_decoration_v1 *deco, uint32_t mode)
{
}

static const struct zxdg_toplevel_decoration_v1_listener decorationlistener = {
	decoration_configure
};

/* wl_surface */
static void
surface_enter(void *data, struct wl_surface *surface, struct wl_output *output)
{
}

static void
surface_leave(void *data, struct wl_surface *surface, struct wl_output *output)
{
}

static void
setscale(double scale)
{
	if (scale <= 0 || fabs(scale - xw.scale) < 1e-6)
		return;
	xw.scale = scale;

	/* reload fonts at the new pixel size */
	if (dc.font.match) {
		xunloadfonts();
		xloadfonts(usedfont, usedfontsize);
		#if SIXEL_PATCH
		for (int i = 0; i < 2; i++) {
			for (ImageList *im = i ? term.images_alt : term.images; im; im = im->next) {
				if (im->pixmap)
					cairo_surface_destroy((cairo_surface_t *)im->pixmap);
				im->pixmap = NULL;
			}
		}
		#endif // SIXEL_PATCH
		xhints();
	}
	if (xw.configured)
		updatesize();
	wlevents++;
}

static void
surface_preferred_buffer_scale(void *data, struct wl_surface *surface, int32_t factor)
{
	outputscale = MAX(factor, 1);
	if (!xw.fracscale)
		setscale(outputscale);
}

static void
surface_preferred_buffer_transform(void *data, struct wl_surface *surface, uint32_t transform)
{
}

static const struct wl_surface_listener surfacelistener = {
	surface_enter,
	surface_leave,
	surface_preferred_buffer_scale,
	surface_preferred_buffer_transform,
};

/* wp_fractional_scale_v1 */
static void
fracscale_preferred(void *data, struct wp_fractional_scale_v1 *fs, uint32_t scale)
{
	fracscale120 = scale;
	setscale(scale / 120.0);
}

static const struct wp_fractional_scale_v1_listener fracscalelistener = {
	fracscale_preferred
};

/* recompute the buffer size from the logical size and scale */
void
updatesize(void)
{
	int pw, ph;

	if (xw.lw <= 0 || xw.lh <= 0)
		return;
	pw = lround(xw.lw * xw.scale);
	ph = lround(xw.lh * xw.scale);
	if (pw == win.w && ph == win.h)
		return;
	if (ttystarted) {
		cresize(pw, ph);
	} else {
		win.w = pw;
		win.h = ph;
	}
	if (xw.viewport)
		wp_viewport_set_destination(xw.viewport, xw.lw, xw.lh);
}

/*
 * Keyboard
 */
static void
keyboard_keymap(void *data, struct wl_keyboard *kbd, uint32_t format, int32_t fd, uint32_t size)
{
	struct xkb_keymap *keymap;
	struct xkb_state *state;
	char *map;

	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED)
		return;
	keymap = xkb_keymap_new_from_string(xw.xkbctx, map,
			XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	if (!keymap)
		return;
	state = xkb_state_new(keymap);
	if (!state) {
		xkb_keymap_unref(keymap);
		return;
	}
	if (xw.xkbstate)
		xkb_state_unref(xw.xkbstate);
	if (xw.keymap)
		xkb_keymap_unref(xw.keymap);
	xw.keymap = keymap;
	xw.xkbstate = state;
}

static void
focusin(void)
{
	win.mode |= MODE_FOCUSED;
	if (IS_SET(MODE_FOCUS))
		ttywrite("\033[I", 3, 0);
	#if ALPHA_PATCH && ALPHA_FOCUS_HIGHLIGHT_PATCH
	if (!focused) {
		focused = 1;
		xloadcols();
		tfulldirt();
	}
	#endif // ALPHA_FOCUS_HIGHLIGHT_PATCH
	wlevents++;
}

static void
focusout(void)
{
	win.mode &= ~MODE_FOCUSED;
	if (IS_SET(MODE_FOCUS))
		ttywrite("\033[O", 3, 0);
	#if ALPHA_PATCH && ALPHA_FOCUS_HIGHLIGHT_PATCH
	if (focused) {
		focused = 0;
		xloadcols();
		tfulldirt();
	}
	#endif // ALPHA_FOCUS_HIGHLIGHT_PATCH
	kbrepeat.active = 0;
	wlevents++;
}

static void
keyboard_enter(void *data, struct wl_keyboard *kbd, uint32_t serial,
		struct wl_surface *surface, struct wl_array *keys)
{
	xw.serial = serial;
	focusin();
}

static void
keyboard_leave(void *data, struct wl_keyboard *kbd, uint32_t serial, struct wl_surface *surface)
{
	xw.serial = serial;
	focusout();
}

static void
keyboard_key(void *data, struct wl_keyboard *kbd, uint32_t serial, uint32_t time,
		uint32_t key, uint32_t state)
{
	xkb_keycode_t keycode = key + 8;

	xw.serial = serial;
	wlevents++;

	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		kpress(keycode);
		if (xw.keymap && kbrepeat.rate > 0 &&
		    xkb_keymap_key_repeats(xw.keymap, keycode)) {
			kbrepeat.active = 1;
			kbrepeat.key = keycode;
			clock_gettime(CLOCK_MONOTONIC, &kbrepeat.next);
			kbrepeat.next.tv_nsec += (long)kbrepeat.delay * 1000000L;
			while (kbrepeat.next.tv_nsec >= 1000000000L) {
				kbrepeat.next.tv_nsec -= 1000000000L;
				kbrepeat.next.tv_sec++;
			}
		}
	} else if (kbrepeat.active && kbrepeat.key == keycode) {
		kbrepeat.active = 0;
	}
}

static void
keyboard_modifiers(void *data, struct wl_keyboard *kbd, uint32_t serial,
		uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
	if (!xw.xkbstate)
		return;
	xkb_state_update_mask(xw.xkbstate, depressed, latched, locked, 0, 0, group);
}

static void
keyboard_repeat_info(void *data, struct wl_keyboard *kbd, int32_t rate, int32_t delay)
{
	kbrepeat.rate = rate;
	kbrepeat.delay = delay;
}

static const struct wl_keyboard_listener keyboardlistener = {
	keyboard_keymap,
	keyboard_enter,
	keyboard_leave,
	keyboard_key,
	keyboard_modifiers,
	keyboard_repeat_info,
};

/* current modifier state as X11 style mask */
uint
modstate(void)
{
	uint state = 0;

	if (!xw.xkbstate)
		return 0;
	#define MOD(name, mask) \
		if (xkb_state_mod_name_is_active(xw.xkbstate, name, XKB_STATE_MODS_EFFECTIVE) > 0) \
			state |= mask;
	MOD(XKB_MOD_NAME_SHIFT, ShiftMask);
	MOD(XKB_MOD_NAME_CAPS, LockMask);
	MOD(XKB_MOD_NAME_CTRL, ControlMask);
	MOD(XKB_MOD_NAME_ALT, Mod1Mask);
	MOD(XKB_MOD_NAME_NUM, Mod2Mask);
	MOD("Mod3", Mod3Mask);
	MOD(XKB_MOD_NAME_LOGO, Mod4Mask);
	MOD("Mod5", Mod5Mask);
	#undef MOD
	return state;
}

/*
 * Pointer
 */
static void
pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
		struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
	xw.pointerserial = serial;
	xw.serial = serial;
	xw.ptrx = wl_fixed_to_double(sx) * xw.scale;
	xw.ptry = wl_fixed_to_double(sy) * xw.scale;
	/* the cursor image must be set on every enter */
	xw.pointershape = -1;
	wlsetpointer(IS_SET(MODE_MOUSE) ? POINTER_TEXT : POINTER_TEXT);
}

static void
pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface)
{
	xw.serial = serial;
}

static void
pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
	MouseEvent e;

	xw.ptrx = wl_fixed_to_double(sx) * xw.scale;
	xw.ptry = wl_fixed_to_double(sy) * xw.scale;

	e.type = MouseMotion;
	e.button = 0;
	e.state = modstate() | buttons << 8;
	e.x = xw.ptrx;
	e.y = xw.ptry;
	e.time = time;
	bmotion(&e);
	wlevents++;
}

static int
linuxbutton(uint32_t button)
{
	switch (button) {
	case BTN_LEFT:    return Button1;
	case BTN_MIDDLE:  return Button2;
	case BTN_RIGHT:   return Button3;
	case BTN_SIDE:    return 8;
	case BTN_EXTRA:   return 9;
	case BTN_FORWARD: return 10;
	case BTN_BACK:    return 11;
	}
	return 0;
}

static void
buttonevent(int type, int button, uint32_t time)
{
	MouseEvent e;

	e.type = type;
	e.button = button;
	e.state = modstate() | buttons << 8;
	e.x = xw.ptrx;
	e.y = xw.ptry;
	e.time = time;
	if (type == MousePress)
		bpress(&e);
	else
		brelease(&e);
	wlevents++;
}

static void
pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time,
		uint32_t button, uint32_t state)
{
	int btn = linuxbutton(button);

	xw.serial = serial;
	xw.pointerserial = serial;
	if (!btn)
		return;
	buttonevent(state == WL_POINTER_BUTTON_STATE_PRESSED ? MousePress : MouseRelease,
			btn, time);
}

/* emit wheel "clicks": axis 0 = vertical, 1 = horizontal */
static void
wheelclicks(int axis, int n, uint32_t time)
{
	int btn, i;

	if (n == 0)
		return;
	if (axis == 0)
		btn = n < 0 ? Button4 : Button5;
	else
		btn = n < 0 ? 6 : 7;
	for (i = 0; i < abs(n); i++) {
		buttonevent(MousePress, btn, time);
		buttonevent(MouseRelease, btn, time);
	}
}

static void
pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value)
{
	int a = axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? 0 : 1;
	int n;

	if (axisdiscrete[a]) /* discrete steps already handled for this frame */
		return;
	axisacc[a] += wl_fixed_to_double(value);
	n = (int)(axisacc[a] / 10.0);
	if (n != 0) {
		axisacc[a] -= n * 10.0;
		wheelclicks(a, n, time);
	}
}

static void
pointer_frame(void *data, struct wl_pointer *pointer)
{
	axisdiscrete[0] = axisdiscrete[1] = 0;
}

static void
pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source)
{
}

static void
pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis)
{
	int a = axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? 0 : 1;
	axisacc[a] = 0;
}

static void
pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete)
{
	int a = axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? 0 : 1;
	axisdiscrete[a] = 1;
	wheelclicks(a, discrete, 0);
}

static void
pointer_axis_value120(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t value120)
{
	static int acc[2];
	int a = axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? 0 : 1;
	int n;

	axisdiscrete[a] = 1;
	acc[a] += value120;
	n = acc[a] / 120;
	if (n != 0) {
		acc[a] -= n * 120;
		wheelclicks(a, n, 0);
	}
}

static void
pointer_axis_relative_direction(void *data, struct wl_pointer *pointer, uint32_t axis, uint32_t dir)
{
}

static const struct wl_pointer_listener pointerlistener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
	.axis_value120 = pointer_axis_value120,
	.axis_relative_direction = pointer_axis_relative_direction,
};

void
wlsetpointer(int shape)
{
	struct wl_cursor *cursor = NULL;
	struct wl_cursor_image *image;
	struct wl_buffer *buffer;
	const char *names[3] = { NULL, NULL, NULL };
	int i, size;

	if (!xw.pointer)
		return;
	if (shape == xw.pointershape)
		return;
	xw.pointershape = shape;

	if (shape == POINTER_HIDDEN) {
		wl_pointer_set_cursor(xw.pointer, xw.pointerserial, NULL, 0, 0);
		return;
	}

	if (xw.cshape) {
		uint32_t s;
		switch (shape) {
		case POINTER_LINK: s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER; break;
		case POINTER_TEXT:
			if (!strcmp(mouseshape, "text") || !strcmp(mouseshape, "xterm"))
				s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
			else if (!strcmp(mouseshape, "crosshair"))
				s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR;
			else if (!strcmp(mouseshape, "pointer") || !strcmp(mouseshape, "hand2"))
				s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
			else
				s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
			break;
		default: s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT; break;
		}
		wp_cursor_shape_device_v1_set_shape(xw.cshape, xw.pointerserial, s);
		return;
	}

	/* fall back to a cursor theme */
	size = 24 * outputscale;
	if (!xw.cursortheme) {
		const char *theme = getenv("XCURSOR_THEME");
		const char *sz = getenv("XCURSOR_SIZE");
		if (sz && atoi(sz) > 0)
			size = atoi(sz) * outputscale;
		xw.cursortheme = wl_cursor_theme_load(theme, size, xw.shm);
		if (!xw.cursortheme)
			return;
	}
	if (!xw.cursorsurface)
		xw.cursorsurface = wl_compositor_create_surface(xw.compositor);

	switch (shape) {
	case POINTER_LINK: names[0] = "pointer"; names[1] = "hand2"; names[2] = "hand1"; break;
	case POINTER_TEXT: names[0] = mouseshape; names[1] = "text"; names[2] = "xterm"; break;
	default: names[0] = "default"; names[1] = "left_ptr"; break;
	}
	for (i = 0; i < 3 && names[i] && !cursor; i++)
		cursor = wl_cursor_theme_get_cursor(xw.cursortheme, names[i]);
	if (!cursor || cursor->image_count < 1)
		return;
	image = cursor->images[0];
	buffer = wl_cursor_image_get_buffer(image);
	if (!buffer)
		return;
	if (compositorversion >= 3)
		wl_surface_set_buffer_scale(xw.cursorsurface, outputscale);
	wl_surface_attach(xw.cursorsurface, buffer, 0, 0);
	wl_surface_damage(xw.cursorsurface, 0, 0, image->width, image->height);
	wl_surface_commit(xw.cursorsurface);
	wl_pointer_set_cursor(xw.pointer, xw.pointerserial, xw.cursorsurface,
			image->hotspot_x / outputscale, image->hotspot_y / outputscale);
}

/*
 * Input method: zwp_text_input_v3 (fcitx5, ibus, ...)
 */
static void
ime_dirtyrow(int row)
{
	if (BETWEEN(row, 0, term.row - 1))
		term.dirty[row] = 1;
}

static void
ime_clearpreedit(void)
{
	if (ime.preedit) {
		free(ime.preedit);
		ime.preedit = NULL;
		ime_dirtyrow(ime.row);
	}
	ime.pre_begin = ime.pre_end = -1;
	ime.row = -1;
}

static void
textinput_enter(void *data, struct zwp_text_input_v3 *ti, struct wl_surface *surface)
{
	ime.active = 1;
	zwp_text_input_v3_enable(ti);
	zwp_text_input_v3_set_content_type(ti,
			ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
			ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_TERMINAL);
	textinput_setrect(ime.spotx, ime.spoty);
	zwp_text_input_v3_commit(ti);
}

static void
textinput_leave(void *data, struct zwp_text_input_v3 *ti, struct wl_surface *surface)
{
	ime.active = 0;
	zwp_text_input_v3_disable(ti);
	zwp_text_input_v3_commit(ti);
	free(ime.ppreedit); ime.ppreedit = NULL;
	free(ime.pcommit); ime.pcommit = NULL;
	ime_clearpreedit();
	wlevents++;
}

static void
textinput_preedit(void *data, struct zwp_text_input_v3 *ti, const char *text,
		int32_t cursor_begin, int32_t cursor_end)
{
	free(ime.ppreedit);
	ime.ppreedit = (text && *text) ? xstrdup((char *)text) : NULL;
	ime.ppre_begin = cursor_begin;
	ime.ppre_end = cursor_end;
}

static void
textinput_commit(void *data, struct zwp_text_input_v3 *ti, const char *text)
{
	free(ime.pcommit);
	ime.pcommit = (text && *text) ? xstrdup((char *)text) : NULL;
}

static void
textinput_delete(void *data, struct zwp_text_input_v3 *ti, uint32_t before, uint32_t after)
{
	/* a terminal has no surrounding text; nothing to delete */
}

static void
textinput_done(void *data, struct zwp_text_input_v3 *ti, uint32_t serial)
{
	/* apply pending state atomically (order mandated by the protocol) */
	if (ime.pcommit) {
		size_t len = strlen(ime.pcommit);
		#if KEYBOARDSELECT_PATCH && REFLOW_PATCH
		if (IS_SET(MODE_KBDSELECT) && kbds_issearchmode())
			kbds_pasteintosearch(ime.pcommit, len, 0);
		else
		#endif // KEYBOARDSELECT_PATCH
		if (!IS_SET(MODE_KBDLOCK))
			ttywrite(ime.pcommit, len, 1);
		free(ime.pcommit);
		ime.pcommit = NULL;
	}

	/* preedit: pending value replaces the current one (empty clears it) */
	ime_clearpreedit();
	if (ime.ppreedit) {
		ime.preedit = ime.ppreedit;
		ime.pre_begin = ime.ppre_begin;
		ime.pre_end = ime.ppre_end;
		ime.ppreedit = NULL;
	}
	ime.ppre_begin = ime.ppre_end = -1;
	wlevents++;
}

static const struct zwp_text_input_v3_listener textinputlistener = {
	textinput_enter,
	textinput_leave,
	textinput_preedit,
	textinput_commit,
	textinput_delete,
	textinput_done,
};

static void
textinput_setup(void)
{
	if (xw.textinput || !xw.timgr || !xw.seat)
		return;
	xw.textinput = zwp_text_input_manager_v3_get_text_input(xw.timgr, xw.seat);
	zwp_text_input_v3_add_listener(xw.textinput, &textinputlistener, NULL);
}

/* tell the input method where the cursor cell is (surface-local logical px) */
static void
textinput_setrect(int x, int y)
{
	int px, py, pw, ph;

	if (!xw.textinput)
		return;
	#if ANYSIZE_PATCH
	px = win.hborderpx + x * win.cw;
	py = win.vborderpx + y * win.ch;
	#else
	px = borderpx + x * win.cw;
	py = borderpx + y * win.ch;
	#endif // ANYSIZE_PATCH
	pw = win.cw;
	ph = win.ch;
	zwp_text_input_v3_set_cursor_rectangle(xw.textinput,
			(int)(px / xw.scale), (int)(py / xw.scale),
			MAX(1, (int)(pw / xw.scale)), MAX(1, (int)(ph / xw.scale)));
}

/*
 * Draw the preedit string over the cursor row, starting at the cursor cell.
 * Characters are underlined; the cell holding the IM cursor is reversed.
 */
static void
drawpreedit(int cx, int cy)
{
	Glyph g;
	mbstate_t mbs;
	wchar_t wc;
	size_t n, len, off;
	int x, w, cursorcell = -1, byteoff;

	if (!ime.preedit || !BETWEEN(cy, 0, term.row - 1))
		return;

	memset(&g, 0, sizeof g);
	g.fg = defaultfg;
	g.bg = defaultbg;
	g.mode = ATTR_UNDERLINE;

	memset(&mbs, 0, sizeof mbs);
	len = strlen(ime.preedit);
	x = cx;
	for (off = 0; off < len && x < term.col; off += n) {
		n = mbrtowc(&wc, ime.preedit + off, len - off, &mbs);
		if (n == (size_t)-1 || n == (size_t)-2) {
			memset(&mbs, 0, sizeof mbs);
			wc = 0xFFFD;
			n = 1;
		} else if (n == 0) {
			break;
		}
		w = wcwidth(wc);
		if (w < 0)
			w = 1;
		if (w == 0)
			continue; /* combining marks: skip */
		if (x + w > term.col)
			break;
		byteoff = off;
		g.u = wc;
		g.mode = ATTR_UNDERLINE | (w == 2 ? ATTR_WIDE : 0);
		if (ime.pre_begin >= 0 && byteoff >= ime.pre_begin &&
		    (byteoff < ime.pre_end || ime.pre_begin == ime.pre_end) &&
		    cursorcell < 0) {
			g.mode |= ATTR_REVERSE;
			cursorcell = x;
		}
		xdrawglyph(g, x, cy);
		x += w;
	}
	/* IM cursor at the end of the preedit: show an empty reversed cell */
	if (cursorcell < 0 && ime.pre_begin >= 0 && x < term.col) {
		g.u = ' ';
		g.mode = ATTR_REVERSE;
		xdrawglyph(g, x, cy);
		x++;
	}
	ime.row = cy;
	ime.col = cx;
	ime.ncells = x - cx;
}

/*
 * Seat
 */
static void
seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !xw.keyboard) {
		xw.keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(xw.keyboard, &keyboardlistener, NULL);
		textinput_setup();
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && xw.keyboard) {
		wl_keyboard_release(xw.keyboard);
		xw.keyboard = NULL;
	}

	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !xw.pointer) {
		xw.pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(xw.pointer, &pointerlistener, NULL);
		if (xw.cshapemgr)
			xw.cshape = wp_cursor_shape_manager_v1_get_pointer(xw.cshapemgr, xw.pointer);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && xw.pointer) {
		if (xw.cshape) {
			wp_cursor_shape_device_v1_destroy(xw.cshape);
			xw.cshape = NULL;
		}
		wl_pointer_release(xw.pointer);
		xw.pointer = NULL;
	}
}

static void
seat_name(void *data, struct wl_seat *seat, const char *name)
{
}

static const struct wl_seat_listener seatlistener = { seat_capabilities, seat_name };

/*
 * Registry
 */
static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version)
{
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositorversion = MIN(version, 6);
		xw.compositor = wl_registry_bind(registry, name, &wl_compositor_interface, compositorversion);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		xw.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0 && !xw.seat) {
		seatversion = MIN(version, 9);
		xw.seat = wl_registry_bind(registry, name, &wl_seat_interface, seatversion);
		wl_seat_add_listener(xw.seat, &seatlistener, NULL);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		xw.wmbase = wl_registry_bind(registry, name, &xdg_wm_base_interface, MIN(version, 6));
		xdg_wm_base_add_listener(xw.wmbase, &wmbaselistener, NULL);
	} else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
		xw.datadevmgr = wl_registry_bind(registry, name, &wl_data_device_manager_interface, MIN(version, 3));
	} else if (strcmp(interface, zwp_primary_selection_device_manager_v1_interface.name) == 0) {
		xw.primselmgr = wl_registry_bind(registry, name, &zwp_primary_selection_device_manager_v1_interface, 1);
	} else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
		xw.decomgr = wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1);
	} else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
		xw.viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
	} else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
		xw.fracmgr = wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1);
	} else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
		xw.cshapemgr = wl_registry_bind(registry, name, &wp_cursor_shape_manager_v1_interface, 1);
	} else if (strcmp(interface, xdg_system_bell_v1_interface.name) == 0) {
		xw.bell = wl_registry_bind(registry, name, &xdg_system_bell_v1_interface, 1);
	} else if (strcmp(interface, zwp_text_input_manager_v3_interface.name) == 0) {
		xw.timgr = wl_registry_bind(registry, name, &zwp_text_input_manager_v3_interface, 1);
		textinput_setup();
	}
}

static void
registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registrylistener = {
	registry_global,
	registry_global_remove,
};

void
xinit(int cols, int rows)
{
	const char *appid;

	if (!(xw.dpy = wl_display_connect(NULL)))
		die("can't open display\n");

	xw.scale = 1.0;
	xw.pointershape = -1;
	xw.pointerisvisible = 1;
	for (int i = 0; i < NSENDJOBS; i++)
		sendjobs[i].fd = -1;

	xw.registry = wl_display_get_registry(xw.dpy);
	wl_registry_add_listener(xw.registry, &registrylistener, NULL);
	wl_display_roundtrip(xw.dpy);
	if (!xw.compositor || !xw.shm || !xw.wmbase)
		die("compositor lacks wl_compositor, wl_shm or xdg_wm_base\n");
	/* second roundtrip: seat capabilities, shm formats */
	wl_display_roundtrip(xw.dpy);

	xw.xkbctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!xw.xkbctx)
		die("can't create xkb context\n");

	/* font */
	if (!FcInit())
		die("could not init fontconfig.\n");

	usedfont = (opt_font == NULL)? font : opt_font;
	xloadfonts(usedfont, 0);

	/* colors */
	xloadcols();

	/* adjust fixed window geometry */
	#if ANYGEOMETRY_PATCH
	switch (geometry) {
	case CellGeometry:
		win.w = 2 * borderpx + cols * win.cw;
		win.h = 2 * borderpx + rows * win.ch;
		break;
	case PixelGeometry:
		win.w = cols;
		win.h = rows;
		cols = (win.w - 2 * borderpx) / win.cw;
		rows = (win.h - 2 * borderpx) / win.ch;
		break;
	}
	#else
	win.w = 2 * borderpx + cols * win.cw;
	win.h = 2 * borderpx + rows * win.ch;
	#endif // ANYGEOMETRY_PATCH
	xw.lw = win.w;
	xw.lh = win.h;

	/* selection devices */
	if (xw.datadevmgr && xw.seat) {
		xw.datadev = wl_data_device_manager_get_data_device(xw.datadevmgr, xw.seat);
		wl_data_device_add_listener(xw.datadev, &datadevicelistener, NULL);
	}
	if (xw.primselmgr && xw.seat) {
		xw.primseldev = zwp_primary_selection_device_manager_v1_get_device(xw.primselmgr, xw.seat);
		zwp_primary_selection_device_v1_add_listener(xw.primseldev, &primdevicelistener, NULL);
	}

	/* window */
	xw.win = wl_compositor_create_surface(xw.compositor);
	wl_surface_add_listener(xw.win, &surfacelistener, NULL);
	xw.xdgsurface = xdg_wm_base_get_xdg_surface(xw.wmbase, xw.win);
	xdg_surface_add_listener(xw.xdgsurface, &xdgsurfacelistener, NULL);
	xw.toplevel = xdg_surface_get_toplevel(xw.xdgsurface);
	xdg_toplevel_add_listener(xw.toplevel, &toplevellistener, NULL);

	appid = opt_name ? opt_name : opt_class ? opt_class : "st";
	xdg_toplevel_set_app_id(xw.toplevel, appid);

	if (xw.decomgr) {
		xw.deco = zxdg_decoration_manager_v1_get_toplevel_decoration(xw.decomgr, xw.toplevel);
		zxdg_toplevel_decoration_v1_add_listener(xw.deco, &decorationlistener, NULL);
		zxdg_toplevel_decoration_v1_set_mode(xw.deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
	if (xw.viewporter && xw.fracmgr) {
		xw.viewport = wp_viewporter_get_viewport(xw.viewporter, xw.win);
		xw.fracscale = wp_fractional_scale_manager_v1_get_fractional_scale(xw.fracmgr, xw.win);
		wp_fractional_scale_v1_add_listener(xw.fracscale, &fracscalelistener, NULL);
	}

	/* font spec buffer */
	#if LIGATURES_PATCH
	xw.specbuf = xmalloc(cols * sizeof(GlyphFontSpec) * 4);
	xw.specseq = xmalloc(cols * sizeof(GlyphFontSeq));
	#else
	xw.specbuf = xmalloc(cols * sizeof(GlyphFontSpec));
	#endif // LIGATURES_PATCH

	#if NETWMICON_PATCH
	setnetwmicon();
	#endif // NETWMICON_PATCH

	win.mode = MODE_NUMLOCK;
	resettitle();
	xhints();

	/* map the window: initial commit without a buffer, then wait for configure */
	wl_surface_commit(xw.win);
	wl_display_flush(xw.dpy);

	clock_gettime(CLOCK_MONOTONIC, &xsel.tclick1);
	clock_gettime(CLOCK_MONOTONIC, &xsel.tclick2);
	xsel.primary = NULL;
	xsel.clipboard = NULL;
}

#if LIGATURES_PATCH
void
xresetfontsettings(uint32_t mode, Font **font, int *frcflags)
{
	*font = &dc.font;
	if ((mode & ATTR_ITALIC) && (mode & ATTR_BOLD)) {
		*font = &dc.ibfont;
		*frcflags = FRC_ITALICBOLD;
	} else if (mode & ATTR_ITALIC) {
		*font = &dc.ifont;
		*frcflags = FRC_ITALIC;
	} else if (mode & ATTR_BOLD) {
		*font = &dc.bfont;
		*frcflags = FRC_BOLD;
	}
}
#endif // LIGATURES_PATCH

/* find (or load) a fallback font for rune in the font ring cache */
static int
fallbackfont(Font *font, int frcflags, Rune rune, unsigned int *glyphidx)
{
	FcResult fcres;
	FcPattern *fcpattern, *fontpattern;
	FcFontSet *fcsets[] = { NULL };
	FcCharSet *fccharset;
	double pixelsize;
	int f;

	for (f = 0; f < frclen; f++) {
		*glyphidx = glyphindex(frc[f].font, rune);
		/* Everything correct. */
		if (*glyphidx && frc[f].flags == frcflags)
			break;
		/* We got a default font for a not found glyph. */
		if (!*glyphidx && frc[f].flags == frcflags
				&& frc[f].unicodep == rune) {
			break;
		}
	}

	/* Nothing was found. Use fontconfig to find matching font. */
	if (f >= frclen) {
		if (!font->set)
			font->set = FcFontSort(0, font->pattern, 1, 0, &fcres);
		fcsets[0] = font->set;

		/*
		 * Nothing was found in the cache. Now use
		 * some dozen of Fontconfig calls to get the
		 * font for one single character.
		 *
		 * Xft and fontconfig are design failures.
		 */
		fcpattern = FcPatternDuplicate(font->pattern);
		fccharset = FcCharSetCreate();

		FcCharSetAddChar(fccharset, rune);
		FcPatternAddCharSet(fcpattern, FC_CHARSET, fccharset);
		FcPatternAddBool(fcpattern, FC_SCALABLE, 1);

		FcConfigSubstitute(0, fcpattern, FcMatchPattern);
		FcDefaultSubstitute(fcpattern);

		fontpattern = FcFontSetMatch(0, fcsets, 1, fcpattern, &fcres);
		if (!fontpattern)
			die("FcFontSetMatch failed seeking fallback font\n");

		/* Allocate memory for the new cache entry. */
		if (frclen >= frccap) {
			frccap += 16;
			frc = xrealloc(frc, frccap * sizeof(Fontcache));
		}

		if (FcPatternGetDouble(fontpattern, FC_PIXEL_SIZE, 0, &pixelsize) != FcResultMatch)
			pixelsize = patternpixelsize(font->pattern);
		frc[frclen].font = opencairofont(fontpattern, pixelsize);
		if (!frc[frclen].font)
			die("opencairofont failed seeking fallback font: %s\n",
				strerror(errno));
		frc[frclen].flags = frcflags;
		frc[frclen].unicodep = rune;

		*glyphidx = glyphindex(frc[frclen].font, rune);

		f = frclen;
		frclen++;

		FcPatternDestroy(fcpattern);
		FcPatternDestroy(fontpattern);
		FcCharSetDestroy(fccharset);
	}

	return f;
}

int
xmakeglyphfontspecs(GlyphFontSpec *specs, const Glyph *glyphs, int len, int x, int y)
{
	#if ANYSIZE_PATCH
	float winx = win.hborderpx + x * win.cw, winy = win.vborderpx + y * win.ch, xp, yp;
	#else
	float winx = borderpx + x * win.cw, winy = borderpx + y * win.ch, xp, yp;
	#endif // ANYSIZE_PATCH
	ushort mode, prevmode = USHRT_MAX;
	Font *font = &dc.font;
	int frcflags = FRC_NORMAL;
	float runewidth = win.cw * ((glyphs[0].mode & ATTR_WIDE) ? 2.0f : 1.0f);
	Rune rune;
	unsigned int glyphidx;
	int i, f, numspecs = 0;
	#if LIGATURES_PATCH
	float cluster_xp, cluster_yp;
	HbTransformData shaped;

	/* Initial values. */
	xresetfontsettings(glyphs[0].mode, &font, &frcflags);
	#if VERTCENTER_PATCH
	xp = winx, yp = winy + font->ascent + win.cyo;
	#else
	xp = winx, yp = winy + font->ascent;
	#endif // VERTCENTER_PATCH
	cluster_xp = xp; cluster_yp = yp;
	#if BOXDRAW_PATCH
	/* Attribute runs are homogeneous. Geometry is one shape per cell,
	 * independent of font coverage, GSUB ligatures, and shaping caches. */
	if (glyphs[0].mode & ATTR_BOXDRAW) {
		for (i = 0; i < len; i++) {
			if (glyphs[i].mode & ATTR_WDUMMY)
				continue;
			specs[numspecs].font = font->match;
			specs[numspecs].glyph = boxdrawindex(&glyphs[i]);
			specs[numspecs].x = xp;
			specs[numspecs].y = yp;
			numspecs++;
			xp += win.cw * ((glyphs[i].mode & ATTR_WIDE) ? 2 : 1);
		}
		return numspecs;
	}
	#endif
	/* Shape the segment. */
	hbtransform(&shaped, font->match, glyphs, 0, len);
	#endif // LIGATURES_PATCH

	#if LIGATURES_PATCH
	for (int code_idx = 0; code_idx < shaped.count; code_idx++)
	#elif VERTCENTER_PATCH
	for (i = 0, xp = winx, yp = winy + font->ascent + win.cyo; i < len; ++i)
	#else
	for (i = 0, xp = winx, yp = winy + font->ascent; i < len; ++i)
	#endif // LIGATURES_PATCH | VERTCENTER_PATCH
	{
		#if LIGATURES_PATCH
		int idx = shaped.glyphs[code_idx].cluster;

		/* Skip dummy wide-character spacing. */
		if (glyphs[idx].mode & ATTR_WDUMMY)
			continue;

		/* Advance the drawing cursor if we've moved to a new cluster */
		if (code_idx > 0 && idx != shaped.glyphs[code_idx - 1].cluster) {
			xp += runewidth;
			cluster_xp = xp;
			cluster_yp = yp;
		}

		#if BOXDRAW_PATCH
		if (glyphs[idx].mode & ATTR_BOXDRAW) {
			/* minor shoehorning: boxdraw uses only this ushort */
			specs[numspecs].font = font->match;
			specs[numspecs].glyph = boxdrawindex(&glyphs[idx]);
			specs[numspecs].x = xp;
			specs[numspecs].y = yp;
			numspecs++;
		} else if (shaped.glyphs[code_idx].codepoint != 0) {
		#else
		if (shaped.glyphs[code_idx].codepoint != 0) {
		#endif // BOXDRAW_PATCH
			/* If symbol is found, put it into the specs. */
			specs[numspecs].font = font->match;
			specs[numspecs].glyph = shaped.glyphs[code_idx].codepoint;
			specs[numspecs].x = cluster_xp + (short)(shaped.positions[code_idx].x_offset / 64.);
			specs[numspecs].y = cluster_yp - (short)(shaped.positions[code_idx].y_offset / 64.);
			cluster_xp += shaped.positions[code_idx].x_advance / 64.;
			cluster_yp += shaped.positions[code_idx].y_advance / 64.;
			numspecs++;
		} else {
			/* If it's not found, try to fetch it through the font cache. */
			rune = glyphs[idx].u;
			f = fallbackfont(font, frcflags, rune, &glyphidx);
			specs[numspecs].font = frc[f].font;
			specs[numspecs].glyph = glyphidx;
			specs[numspecs].x = (short)xp;
			specs[numspecs].y = (short)yp;
			numspecs++;
		}
		#else // !LIGATURES_PATCH
		/* Fetch rune and mode for current glyph. */
		rune = glyphs[i].u;
		mode = glyphs[i].mode;

		/* Skip dummy wide-character spacing. */
		if (mode == ATTR_WDUMMY)
			continue;

		/* Determine font for glyph if different from previous glyph. */
		if (prevmode != mode) {
			prevmode = mode;
			font = &dc.font;
			frcflags = FRC_NORMAL;
			runewidth = win.cw * ((mode & ATTR_WIDE) ? 2.0f : 1.0f);
			if ((mode & ATTR_ITALIC) && (mode & ATTR_BOLD)) {
				font = &dc.ibfont;
				frcflags = FRC_ITALICBOLD;
			} else if (mode & ATTR_ITALIC) {
				font = &dc.ifont;
				frcflags = FRC_ITALIC;
			} else if (mode & ATTR_BOLD) {
				font = &dc.bfont;
				frcflags = FRC_BOLD;
			}
			#if VERTCENTER_PATCH
			yp = winy + font->ascent + win.cyo;
			#else
			yp = winy + font->ascent;
			#endif // VERTCENTER_PATCH
		}

		#if BOXDRAW_PATCH
		if (mode & ATTR_BOXDRAW) {
			/* minor shoehorning: boxdraw uses only this ushort */
			glyphidx = boxdrawindex(&glyphs[i]);
		} else {
			/* Lookup character index with default font. */
			glyphidx = glyphindex(font->match, rune);
		}
		#else
		/* Lookup character index with default font. */
		glyphidx = glyphindex(font->match, rune);
		#endif // BOXDRAW_PATCH
		if (glyphidx) {
			specs[numspecs].font = font->match;
			specs[numspecs].glyph = glyphidx;
			specs[numspecs].x = (short)xp;
			specs[numspecs].y = (short)yp;
			xp += runewidth;
			numspecs++;
			continue;
		}

		/* Fallback on font cache, search the font cache for match. */
		f = fallbackfont(font, frcflags, rune, &glyphidx);

		specs[numspecs].font = frc[f].font;
		specs[numspecs].glyph = glyphidx;
		specs[numspecs].x = (short)xp;
		specs[numspecs].y = (short)yp;
		xp += runewidth;
		numspecs++;
		#endif // LIGATURES_PATCH
	}

	return numspecs;
}

#if UNDERCURL_PATCH
static int isSlopeRising (int x, int iPoint, int waveWidth)
{
	//    .     .     .     .
	//   / \   / \   / \   / \
	//  /   \ /   \ /   \ /   \
	// .     .     .     .     .

	// Find absolute `x` of point
	x += iPoint * (waveWidth/2);

	// Find index of absolute wave
	int absSlope = x / ((float)waveWidth/2);

	return (absSlope % 2);
}

static int getSlope (int x, int iPoint, int waveWidth)
{
	// Sizes: Caps are half width of slopes
	//    1_2       1_2       1_2      1_2
	//   /   \     /   \     /   \    /   \
	//  /     \   /     \   /     \  /     \
	// 0       3_0       3_0      3_0       3_
	// <2->    <1>         <---6---->

	// Find type of first point
	int firstType;
	x -= (x / waveWidth) * waveWidth;
	if (x < (waveWidth * (2.f/6.f)))
		firstType = UNDERCURL_SLOPE_ASCENDING;
	else if (x < (waveWidth * (3.f/6.f)))
		firstType = UNDERCURL_SLOPE_TOP_CAP;
	else if (x < (waveWidth * (5.f/6.f)))
		firstType = UNDERCURL_SLOPE_DESCENDING;
	else
		firstType = UNDERCURL_SLOPE_BOTTOM_CAP;

	// Find type of given point
	int pointType = (iPoint % 4);
	pointType += firstType;
	pointType %= 4;

	return pointType;
}

typedef struct { float x, y; } UPoint;

/* stroke a polyline with cairo; when relative is set every point but the
 * first is an offset from the previous one (XDrawLines CoordModePrevious) */
static void
drawlines(Color *c, int lw, UPoint *points, int npoints, int relative)
{
	float px, py;
	int i;

	if (npoints < 2)
		return;
	cairo_save(xw.draw);
	cairo_set_operator(xw.draw, CAIRO_OPERATOR_OVER);
	setsourceraw(c);
	cairo_set_line_width(xw.draw, lw);
	cairo_set_line_cap(xw.draw, CAIRO_LINE_CAP_BUTT);
	cairo_set_line_join(xw.draw, CAIRO_LINE_JOIN_MITER);
	cairo_set_antialias(xw.draw, CAIRO_ANTIALIAS_DEFAULT);
	px = points[0].x + 0.5f;
	py = points[0].y + 0.5f;
	cairo_move_to(xw.draw, px, py);
	for (i = 1; i < npoints; i++) {
		if (relative) {
			px += points[i].x;
			py += points[i].y;
		} else {
			px = points[i].x + 0.5f;
			py = points[i].y + 0.5f;
		}
		cairo_line_to(xw.draw, px, py);
	}
	cairo_stroke(xw.draw);
	cairo_restore(xw.draw);
}
#endif // UNDERCURL_PATCH

void
xdrawglyphfontspecs(const GlyphFontSpec *specs, Glyph base, int len, int x, int y
	#if WIDE_GLYPHS_PATCH
	,int dmode
	#endif // WIDE_GLYPHS_PATCH
	#if LIGATURES_PATCH
	, int charlen
	#endif // LIGATURES_PATCH
) {
	#if LIGATURES_PATCH
	int width = charlen * win.cw;
	#else
	int charlen = len * ((base.mode & ATTR_WIDE) ? 2 : 1);
	int width = charlen * win.cw;
	#endif // WIDE_GLYPHS_PATCH
	#if ANYSIZE_PATCH
	int winx = win.hborderpx + x * win.cw, winy = win.vborderpx + y * win.ch;
	#else
	int winx = borderpx + x * win.cw, winy = borderpx + y * win.ch;
	#endif // ANYSIZE_PATCH
	Color *fg, *bg, *temp, revfg, revbg, truefg, truebg, opaquefg;
	#if ALPHA_PATCH && ALPHA_GRADIENT_PATCH
	Color gradientbg;
	#endif // ALPHA_PATCH && ALPHA_GRADIENT_PATCH

	/* Fallback on color display for attributes not supported by the font */
	if (base.mode & ATTR_ITALIC && base.mode & ATTR_BOLD) {
		if (dc.ibfont.badslant || dc.ibfont.badweight)
			base.fg = defaultattr;
	} else if ((base.mode & ATTR_ITALIC && dc.ifont.badslant) ||
	    (base.mode & ATTR_BOLD && dc.bfont.badweight)) {
		base.fg = defaultattr;
	}

	if (IS_TRUECOL(base.fg)) {
		truefg.color.alpha = 0xffff;
		truefg.color.red = TRUERED(base.fg);
		truefg.color.green = TRUEGREEN(base.fg);
		truefg.color.blue = TRUEBLUE(base.fg);
		setpixel(&truefg);
		fg = &truefg;
	} else {
		fg = &dc.col[base.fg];
	}

	if (IS_TRUECOL(base.bg)) {
		truebg.color.alpha = 0xffff;
		truebg.color.green = TRUEGREEN(base.bg);
		truebg.color.red = TRUERED(base.bg);
		truebg.color.blue = TRUEBLUE(base.bg);
		setpixel(&truebg);
		bg = &truebg;
	} else {
		bg = &dc.col[base.bg];
	}

	#if !BOLD_IS_NOT_BRIGHT_PATCH
	/* Change basic system colors [0-7] to bright system colors [8-15] */
	if ((base.mode & ATTR_BOLD_FAINT) == ATTR_BOLD && BETWEEN(base.fg, 0, 7))
		fg = &dc.col[base.fg + 8];
	#endif // BOLD_IS_NOT_BRIGHT_PATCH

	if (IS_SET(MODE_REVERSE)) {
		if (fg == &dc.col[defaultfg]) {
			fg = &dc.col[defaultbg];
		} else {
			revfg.color.red = ~fg->color.red;
			revfg.color.green = ~fg->color.green;
			revfg.color.blue = ~fg->color.blue;
			revfg.color.alpha = fg->color.alpha;
			setpixel(&revfg);
			fg = &revfg;
		}

		if (bg == &dc.col[defaultbg]) {
			bg = &dc.col[defaultfg];
		} else {
			revbg.color.red = ~bg->color.red;
			revbg.color.green = ~bg->color.green;
			revbg.color.blue = ~bg->color.blue;
			revbg.color.alpha = bg->color.alpha;
			setpixel(&revbg);
			bg = &revbg;
		}
	}

	if ((base.mode & ATTR_BOLD_FAINT) == ATTR_FAINT) {
		revfg.color.red = fg->color.red / 2;
		revfg.color.green = fg->color.green / 2;
		revfg.color.blue = fg->color.blue / 2;
		revfg.color.alpha = fg->color.alpha;
		setpixel(&revfg);
		fg = &revfg;
	}

	if (base.mode & ATTR_REVERSE) {
		#if SPOILER_PATCH
		if (bg == fg) {
			bg = &dc.col[defaultfg];
			fg = &dc.col[defaultbg];
		} else {
			temp = fg;
			fg = bg;
			bg = temp;
		}
		#else
		temp = fg;
		fg = bg;
		bg = temp;
		#endif // SPOILER_PATCH
	}

	#if SELECTION_COLORS_PATCH
	if (base.mode & ATTR_SELECTED) {
		bg = &dc.col[selectionbg];
		if (!ignoreselfg)
			fg = &dc.col[selectionfg];
	}
	#endif // SELECTION_COLORS_PATCH

	if (base.mode & ATTR_BLINK && win.mode & MODE_BLINK)
		fg = bg;

	if (base.mode & ATTR_INVISIBLE)
		fg = bg;

	#if INVERT_PATCH
	if (invertcolors) {
		revfg = invertedcolor(fg);
		revbg = invertedcolor(bg);
		fg = &revfg;
		bg = &revbg;
	}
	#endif // INVERT_PATCH

	#if KEYBOARDSELECT_PATCH && REFLOW_PATCH
	if (base.mode & ATTR_HIGHLIGHT) {
		fg = &dc.col[(base.mode & ATTR_REVERSE) ? highlightbg : highlightfg];
		bg = &dc.col[(base.mode & ATTR_REVERSE) ? highlightfg : highlightbg];
	}
	#endif // KEYBOARDSELECT_PATCH

	#if ALPHA_PATCH && ALPHA_GRADIENT_PATCH
	/* Keep per-row alpha out of the palette; fullscreen is always opaque. */
	gradientbg = *bg;
	applyalpha(&gradientbg, isfullscreen ? 1.0f :
	           grad_alpha * (win.h - y * win.ch) / win.h + stat_alpha);
	bg = &gradientbg;
	#endif // ALPHA_PATCH && ALPHA_GRADIENT_PATCH

	#if WIDE_GLYPHS_PATCH
	if (dmode & DRAW_BG) {
	#endif // WIDE_GLYPHS_PATCH
	/* Intelligent cleaning up of the borders. */
	#if ANYSIZE_PATCH
	if (x == 0) {
		xclear(0, (y == 0)? 0 : winy, win.hborderpx,
			winy + win.ch +
			((winy + win.ch >= win.vborderpx + win.th)? win.h : 0));
	}
	if (winx + width >= win.hborderpx + win.tw) {
		xclear(winx + width, (y == 0)? 0 : winy, win.w,
			((winy + win.ch >= win.vborderpx + win.th)? win.h : (winy + win.ch)));
	}
	if (y == 0)
		xclear(winx, 0, winx + width, win.vborderpx);
	if (winy + win.ch >= win.vborderpx + win.th)
		xclear(winx, winy + win.ch, winx + width, win.h);
	#else
	if (x == 0) {
		xclear(0, (y == 0)? 0 : winy, borderpx,
			winy + win.ch +
			((winy + win.ch >= borderpx + win.th)? win.h : 0));
	}
	if (winx + width >= borderpx + win.tw) {
		xclear(winx + width, (y == 0)? 0 : winy, win.w,
			((winy + win.ch >= borderpx + win.th)? win.h : (winy + win.ch)));
	}
	if (y == 0)
		xclear(winx, 0, winx + width, borderpx);
	if (winy + win.ch >= borderpx + win.th)
		xclear(winx, winy + win.ch, winx + width, win.h);
	#endif // ANYSIZE_PATCH

	/* Fill the background */
	drawrect(bg, winx, winy, width, win.ch);
	#if WIDE_GLYPHS_PATCH
	}
	#endif // WIDE_GLYPHS_PATCH

	#if WIDE_GLYPHS_PATCH
	if (dmode & DRAW_FG) {
	#endif // WIDE_GLYPHS_PATCH
	/*
	 * Foreground (glyphs / box drawing / underlines) is always opaque, even
	 * when fg was swapped with the translucent default background
	 * (reverse video, selection, cursor).  Only the background may be
	 * see-through.
	 */
	if (fg->color.alpha != 0xffff) {
		opaquefg = *fg;
		opaquefg.color.alpha = 0xffff;
		setpixel(&opaquefg);
		fg = &opaquefg;
	}
	#if BOXDRAW_PATCH
	if (base.mode & ATTR_BOXDRAW) {
		drawboxes(winx, winy, width / len, win.ch, fg, bg, specs, len);
		adddamage(winx, winy, width, win.ch);
	} else {
	#endif // BOXDRAW_PATCH
	/* Render the glyphs, clipped so that they stay within their line. */
	#if WIDE_GLYPHS_PATCH
	drawglyphs(fg, specs, len, 0, winy, win.w, win.ch);
	adddamage(0, winy, win.w, win.ch);
	#else
	drawglyphs(fg, specs, len, winx, winy, width, win.ch);
	#endif // WIDE_GLYPHS_PATCH

	#if BOXDRAW_PATCH
	}
	#endif // BOXDRAW_PATCH

	/* Render underline and strikethrough. */
	if (base.mode & ATTR_UNDERLINE) {
		#if UNDERCURL_PATCH
		// Underline Color
		const int widthThreshold  = 28; // +1 width every widthThreshold px of font
		int wlw = (win.ch / widthThreshold) + 1; // Wave Line Width
		Color linecolor;
		if (UL_CTYPE(base.ul) != UC_NONE &&
			!(base.mode & ATTR_BLINK && win.mode & MODE_BLINK) &&
			!(base.mode & ATTR_INVISIBLE)
		) {
			// Special color for underline
			if (UL_CTYPE(base.ul) == UC_INDEX) {
				linecolor = dc.col[UL_RGB(base.ul) & 0xff];
			} else {
				linecolor.color.red = ((UL_RGB(base.ul) >> 16) & 0xff) * 257;
				linecolor.color.green = ((UL_RGB(base.ul) >> 8) & 0xff) * 257;
				linecolor.color.blue = (UL_RGB(base.ul) & 0xff) * 257;
			}
		} else {
			// Foreground color for underline
			linecolor = *fg;
		}
		linecolor.color.alpha = 0xffff;
		setpixel(&linecolor);

		// Underline Style
		if (UL_STYLE(base.ul) != 3) {
			drawrect(&linecolor, winx,
				winy + dc.font.ascent * chscale + 1, width, wlw);
		} else {
			int ww = win.cw;//width;
			int wh = dc.font.descent - wlw/2 - 1;//r.height/7;
			int wx = winx;
			int wy = winy + win.ch - dc.font.descent;
			#if VERTCENTER_PATCH
			wy -= win.cyo;
			#endif // VERTCENTER_PATCH

#if UNDERCURL_STYLE == UNDERCURL_CURLY
			// Draw waves
			int i = 0;
			resetclip();
			cairo_save(xw.draw);
			cairo_set_operator(xw.draw, CAIRO_OPERATOR_OVER);
			setsourceraw(&linecolor);
			cairo_set_line_width(xw.draw, wlw);
			cairo_set_antialias(xw.draw, CAIRO_ANTIALIAS_DEFAULT);
			cairo_new_path(xw.draw);
			for (i = 0; i < charlen-1; i++) {
				/* arc(x, y, w, h, a1, a2): X arcs measured counter clockwise from 3 o'clock */
				cairo_save(xw.draw);
				cairo_translate(xw.draw, wx + win.cw * i + ww / 4 + win.cw / 4.0, wy + wh / 2.0);
				cairo_scale(xw.draw, win.cw / 4.0, MAX(wh, 1) / 2.0);
				cairo_new_sub_path(xw.draw);
				cairo_arc_negative(xw.draw, 0, 0, 1, 0, -M_PI);
				cairo_restore(xw.draw);
				cairo_save(xw.draw);
				cairo_translate(xw.draw, wx + win.cw * i + ww * 0.75 + win.cw / 4.0, wy + wh / 2.0);
				cairo_scale(xw.draw, win.cw / 4.0, MAX(wh, 1) / 2.0);
				cairo_new_sub_path(xw.draw);
				cairo_arc_negative(xw.draw, 0, 0, 1, -M_PI, -2 * M_PI);
				cairo_restore(xw.draw);
			}
			// Last wave
			cairo_save(xw.draw);
			cairo_translate(xw.draw, wx + ww * i + ww / 4 + ww / 4.0, wy + wh / 2.0);
			cairo_scale(xw.draw, ww / 4.0, MAX(wh, 1) / 2.0);
			cairo_new_sub_path(xw.draw);
			cairo_arc_negative(xw.draw, 0, 0, 1, 0, -M_PI);
			cairo_restore(xw.draw);
			// Last wave tail
			cairo_save(xw.draw);
			cairo_translate(xw.draw, wx + ww * i + ww * 0.75 + ceil(ww / 2.) / 2.0, wy + wh / 2.0);
			cairo_scale(xw.draw, ceil(ww / 2.) / 2.0, MAX(wh, 1) / 2.0);
			cairo_new_sub_path(xw.draw);
			cairo_arc_negative(xw.draw, 0, 0, 1, -M_PI, -1.5 * M_PI);
			cairo_restore(xw.draw);
			// First wave tail
			cairo_save(xw.draw);
			cairo_translate(xw.draw, wx - ww/4 - 1 + ceil(ww / 2.) / 2.0, wy + wh / 2.0);
			cairo_scale(xw.draw, ceil(ww / 2.) / 2.0, MAX(wh, 1) / 2.0);
			cairo_new_sub_path(xw.draw);
			cairo_arc_negative(xw.draw, 0, 0, 1, -1.5 * M_PI, -2 * M_PI);
			cairo_restore(xw.draw);
			cairo_stroke(xw.draw);
			cairo_restore(xw.draw);
			adddamage(winx, wy - wlw, width, wh + 2 * wlw);
#elif UNDERCURL_STYLE == UNDERCURL_SPIKY
			// Make the underline corridor larger
			/*
			wy -= wh;
			*/
			wh *= 2;

			// Set the angle of the slope to 45°
			ww = wh;

			// Position of wave is independent of word, it's absolute
			wx = (wx / (ww/2)) * (ww/2);

			int marginStart = winx - wx;

			// Calculate number of points with floating precision
			float n = width;					// Width of word in pixels
			n = (n / ww) * 2;					// Number of slopes (/ or \)
			n += 2;								// Add two last points
			int npoints = n;					// Convert to int

			// Total length of underline
			float waveLength = 0;

			if (npoints >= 3) {
				// We add an aditional slot in case we use a bonus point
				UPoint *points = xmalloc(sizeof(UPoint) * (npoints + 1));

				// First point (Starts with the word bounds)
				points[0] = (UPoint) {
					.x = wx + marginStart,
					.y = (isSlopeRising(wx, 0, ww))
						? (wy - marginStart + ww/2.f)
						: (wy + marginStart)
				};

				// Second point (Goes back to the absolute point coordinates)
				points[1] = (UPoint) {
					.x = (ww/2.f) - marginStart,
					.y = (isSlopeRising(wx, 1, ww))
						? (ww/2.f - marginStart)
						: (-ww/2.f + marginStart)
				};
				waveLength += (ww/2.f) - marginStart;

				// The rest of the points
				for (int i = 2; i < npoints-1; i++) {
					points[i] = (UPoint) {
						.x = ww/2,
						.y = (isSlopeRising(wx, i, ww))
							? wh/2
							: -wh/2
					};
					waveLength += ww/2;
				}

				// Last point
				points[npoints-1] = (UPoint) {
					.x = ww/2,
					.y = (isSlopeRising(wx, npoints-1, ww))
						? wh/2
						: -wh/2
				};
				waveLength += ww/2;

				// End
				if (waveLength < width) { // Add a bonus point?
					int marginEnd = width - waveLength;
					points[npoints] = (UPoint) {
						.x = marginEnd,
						.y = (isSlopeRising(wx, npoints, ww))
							? (marginEnd)
							: (-marginEnd)
					};

					npoints++;
				} else if (waveLength > width) { // Is last point too far?
					int marginEnd = waveLength - width;
					points[npoints-1].x -= marginEnd;
					if (isSlopeRising(wx, npoints-1, ww))
						points[npoints-1].y -= (marginEnd);
					else
						points[npoints-1].y += (marginEnd);
				}

				// Draw the lines
				resetclip();
				cairo_save(xw.draw);
				cairo_rectangle(xw.draw, winx, winy, width, win.ch);
				cairo_clip(xw.draw);
				drawlines(&linecolor, wlw, points, npoints, 1);

				// Draw a second underline with an offset of 1 pixel
				if ( ((win.ch / (widthThreshold/2)) % 2)) {
					points[0].x++;

					drawlines(&linecolor, wlw, points, npoints, 1);
				}
				cairo_restore(xw.draw);
				adddamage(winx, winy, width, win.ch);

				// Free resources
				free(points);
			}
#else // UNDERCURL_CAPPED
			// Cap is half of wave width
			float capRatio = 0.5f;

			// Make the underline corridor larger
			wh *= 2;

			// Set the angle of the slope to 45°
			ww = wh;
			ww *= 1 + capRatio; // Add a bit of width for the cap

			// Position of wave is independent of word, it's absolute
			wx = (wx / ww) * ww;

			float marginStart;
			switch(getSlope(winx, 0, ww)) {
				case UNDERCURL_SLOPE_ASCENDING:
					marginStart = winx - wx;
					break;
				case UNDERCURL_SLOPE_TOP_CAP:
					marginStart = winx - (wx + (ww * (2.f/6.f)));
					break;
				case UNDERCURL_SLOPE_DESCENDING:
					marginStart = winx - (wx + (ww * (3.f/6.f)));
					break;
				case UNDERCURL_SLOPE_BOTTOM_CAP:
					marginStart = winx - (wx + (ww * (5.f/6.f)));
					break;
			}

			// Calculate number of points with floating precision
			float n = width;					// Width of word in pixels
												//					   ._.
			n = (n / ww) * 4;					// Number of points (./   \.)
			n += 2;								// Add two last points
			int npoints = n;					// Convert to int

			// Position of the pen to draw the lines
			float penX = 0;
			float penY = 0;

			if (npoints >= 3) {
				UPoint *points = xmalloc(sizeof(UPoint) * (npoints + 1));

				// First point (Starts with the word bounds)
				penX = winx;
				switch (getSlope(winx, 0, ww)) {
					case UNDERCURL_SLOPE_ASCENDING:
						penY = wy + wh/2.f - marginStart;
						break;
					case UNDERCURL_SLOPE_TOP_CAP:
						penY = wy;
						break;
					case UNDERCURL_SLOPE_DESCENDING:
						penY = wy + marginStart;
						break;
					case UNDERCURL_SLOPE_BOTTOM_CAP:
						penY = wy + wh/2.f;
						break;
				}
				points[0].x = penX;
				points[0].y = penY;

				// Second point (Goes back to the absolute point coordinates)
				switch (getSlope(winx, 1, ww)) {
					case UNDERCURL_SLOPE_ASCENDING:
						penX += ww * (1.f/6.f) - marginStart;
						penY += 0;
						break;
					case UNDERCURL_SLOPE_TOP_CAP:
						penX += ww * (2.f/6.f) - marginStart;
						penY += -wh/2.f + marginStart;
						break;
					case UNDERCURL_SLOPE_DESCENDING:
						penX += ww * (1.f/6.f) - marginStart;
						penY += 0;
						break;
					case UNDERCURL_SLOPE_BOTTOM_CAP:
						penX += ww * (2.f/6.f) - marginStart;
						penY += -marginStart + wh/2.f;
						break;
				}
				points[1].x = penX;
				points[1].y = penY;

				// The rest of the points
				for (int i = 2; i < npoints; i++) {
					switch (getSlope(winx, i, ww)) {
						case UNDERCURL_SLOPE_ASCENDING:
						case UNDERCURL_SLOPE_DESCENDING:
							penX += ww * (1.f/6.f);
							penY += 0;
							break;
						case UNDERCURL_SLOPE_TOP_CAP:
							penX += ww * (2.f/6.f);
							penY += -wh / 2.f;
							break;
						case UNDERCURL_SLOPE_BOTTOM_CAP:
							penX += ww * (2.f/6.f);
							penY += wh / 2.f;
							break;
					}
					points[i].x = penX;
					points[i].y = penY;
				}

				// End
				float waveLength = penX - winx;
				if (waveLength < width) { // Add a bonus point?
					int marginEnd = width - waveLength;
					penX += marginEnd;
					switch(getSlope(winx, npoints, ww)) {
						case UNDERCURL_SLOPE_ASCENDING:
						case UNDERCURL_SLOPE_DESCENDING:
							//penY += 0;
							break;
						case UNDERCURL_SLOPE_TOP_CAP:
							penY += -marginEnd;
							break;
						case UNDERCURL_SLOPE_BOTTOM_CAP:
							penY += marginEnd;
							break;
					}

					points[npoints].x = penX;
					points[npoints].y = penY;

					npoints++;
				} else if (waveLength > width) { // Is last point too far?
					int marginEnd = waveLength - width;
					points[npoints-1].x -= marginEnd;
					switch(getSlope(winx, npoints-1, ww)) {
						case UNDERCURL_SLOPE_TOP_CAP:
							points[npoints-1].y += marginEnd;
							break;
						case UNDERCURL_SLOPE_BOTTOM_CAP:
							points[npoints-1].y -= marginEnd;
							break;
						default:
							break;
					}
				}

				// Draw the lines
				resetclip();
				cairo_save(xw.draw);
				cairo_rectangle(xw.draw, winx, winy, width, win.ch);
				cairo_clip(xw.draw);
				drawlines(&linecolor, wlw, points, npoints, 0);

				// Draw a second underline with an offset of 1 pixel
				if ( ((win.ch / (widthThreshold/2)) % 2)) {
					for (int i = 0; i < npoints; i++)
						points[i].x++;

					drawlines(&linecolor, wlw, points, npoints, 0);
				}
				cairo_restore(xw.draw);
				adddamage(winx, winy, width, win.ch);

				// Free resources
				free(points);
			}
#endif
		}
		#elif VERTCENTER_PATCH
		drawrect(fg, winx, winy + win.cyo + dc.font.ascent * chscale + 1,
				width, 1);
		#else
		drawrect(fg, winx, winy + dc.font.ascent * chscale + 1,
				width, 1);
		#endif // UNDERCURL_PATCH | VERTCENTER_PATCH
	}

	if (base.mode & ATTR_STRUCK) {
		#if VERTCENTER_PATCH
		drawrect(fg, winx, winy + win.cyo + 2 * dc.font.ascent * chscale / 3,
				width, 1);
		#else
		drawrect(fg, winx, winy + 2 * dc.font.ascent * chscale / 3,
				width, 1);
		#endif // VERTCENTER_PATCH
	}
	#if WIDE_GLYPHS_PATCH
	}
	#endif // WIDE_GLYPHS_PATCH

	#if OPENURLONCLICK_PATCH
	/* underline url (openurlonclick patch) */
	if (url_draw && y >= url_y1 && y <= url_y2) {
		int x1 = (y == url_y1) ? url_x1 : 0;
		int x2 = (y == url_y2) ? MIN(url_x2, term.col-1) : url_maxcol;
		if (x + charlen > x1 && x <= x2) {
			int xu = MAX(x, x1);
			int wu = (x2 - xu + 1) * win.cw;
			#if ANYSIZE_PATCH
			xu = win.hborderpx + xu * win.cw;
			#else
			xu = borderpx + xu * win.cw;
			#endif // ANYSIZE_PATCH
			#if VERTCENTER_PATCH
			drawrect(fg, xu, winy + win.cyo + dc.font.ascent * chscale + 2, wu, 1);
			#else
			drawrect(fg, xu, winy + dc.font.ascent * chscale + 2, wu, 1);
			#endif // VERTCENTER_PATCH
			url_draw = (y != url_y2 || x + charlen <= x2);
		}
	}
	#endif // OPENURLONCLICK_PATCH
}

void
xdrawglyph(Glyph g, int x, int y)
{
	int numspecs;
	GlyphFontSpec *specs = xw.specbuf;

	numspecs = xmakeglyphfontspecs(specs, &g, 1, x, y);
	xdrawglyphfontspecs(specs, g, numspecs, x, y
		#if WIDE_GLYPHS_PATCH
		,DRAW_BG | DRAW_FG
		#endif // WIDE_GLYPHS_PATCH
		#if LIGATURES_PATCH
		,(g.mode & ATTR_WIDE) ? 2 : 1
		#endif // LIGATURES_PATCH
	);
}

static void
#if LIGATURES_PATCH
drawcursor(int cx, int cy, Glyph g, int ox, int oy, Glyph og, Line line, int len)
#else
drawcursor(int cx, int cy, Glyph g, int ox, int oy, Glyph og)
#endif // LIGATURES_PATCH
{
	Color drawcol;

	#if LIGATURES_PATCH
	/* Redraw the line where cursor was previously.
	 * It will restore the ligatures broken by the cursor. */
	xdrawline(line, 0, oy, len);
	#else
	/* Remove the old cursor */
	if (selected(ox, oy))
		#if SELECTION_COLORS_PATCH
		og.mode |= ATTR_SELECTED;
		#else
		og.mode ^= ATTR_REVERSE;
		#endif // SELECTION_COLORS_PATCH

	xdrawglyph(og, ox, oy);
	#endif // LIGATURES_PATCH

	#if HIDE_TERMINAL_CURSOR_PATCH
	if (IS_SET(MODE_HIDE) || !IS_SET(MODE_FOCUSED))
		return;
	#else
	if (IS_SET(MODE_HIDE))
		return;
	#endif // HIDE_TERMINAL_CURSOR_PATCH

	/*
	 * Select the right color for the right mode.
	 */
	g.mode &= ATTR_BOLD|ATTR_ITALIC|ATTR_UNDERLINE|ATTR_STRUCK|ATTR_WIDE
	#if BOXDRAW_PATCH
	|ATTR_BOXDRAW
	#endif // BOXDRAW_PATCH
	#if DYNAMIC_CURSOR_COLOR_PATCH
	|ATTR_REVERSE
	#endif // DYNAMIC_CURSOR_COLOR_PATCH
	#if KEYBOARDSELECT_PATCH && REFLOW_PATCH
	|ATTR_HIGHLIGHT
	#endif // KEYBOARDSELECT_PATCH
	;

	if (IS_SET(MODE_REVERSE)) {
		g.mode |= ATTR_REVERSE;
		g.bg = defaultfg;
		#if SELECTION_COLORS_PATCH
		g.fg = defaultcs;
		drawcol = dc.col[defaultrcs];
		#else
		if (selected(cx, cy)) {
			drawcol = dc.col[defaultcs];
			g.fg = defaultrcs;
		} else {
			drawcol = dc.col[defaultrcs];
			g.fg = defaultcs;
		}
		#endif // SELECTION_COLORS_PATCH
	} else {
		#if SELECTION_COLORS_PATCH && !DYNAMIC_CURSOR_COLOR_PATCH
		g.fg = defaultbg;
		g.bg = defaultcs;
		drawcol = dc.col[defaultcs];
		#else
		if (selected(cx, cy)) {
			#if KEYBOARDSELECT_PATCH && REFLOW_PATCH
			g.mode &= ~(ATTR_REVERSE | ATTR_HIGHLIGHT);
			#elif DYNAMIC_CURSOR_COLOR_PATCH
			g.mode &= ~ATTR_REVERSE;
			#endif // DYNAMIC_CURSOR_COLOR_PATCH
			g.fg = defaultfg;
			g.bg = defaultrcs;
		} else {
			#if DYNAMIC_CURSOR_COLOR_PATCH
			unsigned int tmpcol = g.bg;
			g.bg = g.fg;
			g.fg = tmpcol;
			#else
			g.fg = defaultbg;
			g.bg = defaultcs;
			#endif // DYNAMIC_CURSOR_COLOR_PATCH
		}

		#if DYNAMIC_CURSOR_COLOR_PATCH
		if (IS_TRUECOL(g.bg)) {
			drawcol.color.alpha = 0xffff;
			drawcol.color.red = TRUERED(g.bg);
			drawcol.color.green = TRUEGREEN(g.bg);
			drawcol.color.blue = TRUEBLUE(g.bg);
			setpixel(&drawcol);
		} else
			drawcol = dc.col[g.bg];
		#else
		drawcol = dc.col[g.bg];
		#endif // DYNAMIC_CURSOR_COLOR_PATCH
		#endif // SELECTION_COLORS_PATCH
	}

	#if KEYBOARDSELECT_PATCH && REFLOW_PATCH
	if (g.mode & ATTR_HIGHLIGHT)
		g.mode ^= ATTR_REVERSE;
	#endif // KEYBOARDSELECT_PATCH

	/* draw the new one */
	if (IS_SET(MODE_FOCUSED)) {
		switch (win.cursor) {
		#if !BLINKING_CURSOR_PATCH
		case 7: /* st extension */
			g.u = 0x2603; /* snowman (U+2603) */
			/* FALLTHROUGH */
		#endif // BLINKING_CURSOR_PATCH
		case 0: /* Blinking block */
		case 1: /* Blinking block (default) */
			#if BLINKING_CURSOR_PATCH
			if (IS_SET(MODE_BLINK))
				break;
			/* FALLTHROUGH */
			#endif // BLINKING_CURSOR_PATCH
		case 2: /* Steady block */
			xdrawglyph(g, cx, cy);
			break;
		case 3: /* Blinking underline */
			#if BLINKING_CURSOR_PATCH
			if (IS_SET(MODE_BLINK))
				break;
			/* FALLTHROUGH */
			#endif // BLINKING_CURSOR_PATCH
		case 4: /* Steady underline */
			#if ANYSIZE_PATCH
			drawrect(&drawcol,
					win.hborderpx + cx * win.cw,
					win.vborderpx + (cy + 1) * win.ch - \
						cursorthickness,
					win.cw, cursorthickness);
			#else
			drawrect(&drawcol,
					borderpx + cx * win.cw,
					borderpx + (cy + 1) * win.ch - \
						cursorthickness,
					win.cw, cursorthickness);
			#endif // ANYSIZE_PATCH
			break;
		case 5: /* Blinking bar */
			#if BLINKING_CURSOR_PATCH
			if (IS_SET(MODE_BLINK))
				break;
			/* FALLTHROUGH */
			#endif // BLINKING_CURSOR_PATCH
		case 6: /* Steady bar */
			drawrect(&drawcol,
					#if ANYSIZE_PATCH
					win.hborderpx + cx * win.cw,
					win.vborderpx + cy * win.ch,
					#else
					borderpx + cx * win.cw,
					borderpx + cy * win.ch,
					#endif // ANYSIZE_PATCH
					cursorthickness, win.ch);
			break;
		#if BLINKING_CURSOR_PATCH
		case 7: /* Blinking st cursor */
			if (IS_SET(MODE_BLINK))
				break;
			/* FALLTHROUGH */
		case 8: /* Steady st cursor */
			g.u = stcursor;
			xdrawglyph(g, cx, cy);
			break;
		#endif // BLINKING_CURSOR_PATCH
		}
	} else {
		#if ANYSIZE_PATCH
		int bx = win.hborderpx + cx * win.cw, by = win.vborderpx + cy * win.ch;
		#else
		int bx = borderpx + cx * win.cw, by = borderpx + cy * win.ch;
		#endif // ANYSIZE_PATCH
		drawrect(&drawcol, bx, by, win.cw - 1, 1);
		drawrect(&drawcol, bx, by, 1, win.ch - 1);
		drawrect(&drawcol, bx + win.cw - 1, by, 1, win.ch - 1);
		drawrect(&drawcol, bx, by + win.ch - 1, win.cw, 1);
	}
}

void
#if LIGATURES_PATCH
xdrawcursor(int cx, int cy, Glyph g, int ox, int oy, Glyph og, Line line, int len)
#else
xdrawcursor(int cx, int cy, Glyph g, int ox, int oy, Glyph og)
#endif // LIGATURES_PATCH
{
	#if LIGATURES_PATCH
	drawcursor(cx, cy, g, ox, oy, og, line, len);
	#else
	drawcursor(cx, cy, g, ox, oy, og);
	#endif // LIGATURES_PATCH
	/* input method preedit text overlays the cursor cell */
	drawpreedit(cx, cy);
}

void
xsetenv(void)
{
	/* there is no window id on wayland */
	unsetenv("WINDOWID");
}

void
xseticontitle(char *p)
{
	/* wayland has no separate icon title */
}

#if CSI_22_23_PATCH
void
xsettitle(char *p, int pop)
{
	free(titlestack[tstki]);
	if (pop) {
		titlestack[tstki] = NULL;
		tstki = (tstki - 1 + TITLESTACKSIZE) % TITLESTACKSIZE;
		p = titlestack[tstki] ? titlestack[tstki] : opt_title;
	} else if (p && p[0] != '\0') {
		titlestack[tstki] = xstrdup(p);
	} else {
		titlestack[tstki] = NULL;
		p = opt_title;
	}

	if (xw.toplevel)
		xdg_toplevel_set_title(xw.toplevel, p);
}

void
xpushtitle(void)
{
	int tstkin = (tstki + 1) % TITLESTACKSIZE;

	free(titlestack[tstkin]);
	titlestack[tstkin] = titlestack[tstki] ? xstrdup(titlestack[tstki]) : NULL;
	tstki = tstkin;
}

void
xfreetitlestack(void)
{
	for (int i = 0; i < LEN(titlestack); i++) {
		free(titlestack[i]);
		titlestack[i] = NULL;
	}
}
#else
void
xsettitle(char *p)
{
	DEFAULT(p, opt_title);

	if (p[0] == '\0')
		p = opt_title;

	if (xw.toplevel)
		xdg_toplevel_set_title(xw.toplevel, p);
}
#endif // CSI_22_23_PATCH

int
xstartdraw(void)
{
	if (!IS_SET(MODE_VISIBLE))
		return 0;
	return startframe();
}

#if LIGATURES_PATCH && WIDE_GLYPHS_PATCH
void
xdrawline(Line line, int x1, int y1, int x2)
{
	int i, j, x, ox, numspecs;
	Glyph new;
	GlyphFontSeq *seq = xw.specseq;
	GlyphFontSpec *specs = xw.specbuf;

	/* Draw line in 2 passes: background and foreground. This way wide glyphs
	   won't get truncated (#223) */

	/* background */
	i = j = ox = 0;
	for (x = x1; x < x2; x++) {
		new = line[x];
		if (new.mode == ATTR_WDUMMY)
			continue;
		if (selected(x, y1))
			#if SELECTION_COLORS_PATCH
			new.mode |= ATTR_SELECTED;
			#else
			new.mode ^= ATTR_REVERSE;
			#endif // SELECTION_COLORS_PATCH
		if ((i > 0) && ATTRCMP(seq[j].base, new)) {
			numspecs = xmakeglyphfontspecs(specs, &line[ox], x - ox, ox, y1);
			xdrawglyphfontspecs(specs, seq[j].base, numspecs, ox, y1, DRAW_BG, x - ox);
			seq[j].charlen = x - ox;
			seq[j++].numspecs = numspecs;
			specs += numspecs;
			i = 0;
		}
		if (i == 0) {
			ox = x;
			seq[j].ox= ox;
			seq[j].base = new;
		}
		i++;
	}
	if (i > 0) {
		numspecs = xmakeglyphfontspecs(specs, &line[ox], x2 - ox, ox, y1);
		xdrawglyphfontspecs(specs, seq[j].base, numspecs, ox, y1, DRAW_BG, x2 - ox);
		seq[j].charlen = x2 - ox;
		seq[j++].numspecs = numspecs;
	}

	/* foreground */
	specs = xw.specbuf;
	for (i = 0; i < j; i++) {
		xdrawglyphfontspecs(specs, seq[i].base, seq[i].numspecs, seq[i].ox, y1, DRAW_FG, seq[i].charlen);
		specs += seq[i].numspecs;
	}

	#if KEYBOARDSELECT_PATCH && REFLOW_PATCH
	kbds_drawstatusbar(y1);
	#endif // KEYBOARDSELECT_PATCH
}
#elif LIGATURES_PATCH
void
xdrawline(Line line, int x1, int y1, int x2)
{
	int i, x, ox, numspecs;
	Glyph base, new;

	GlyphFontSpec *specs = xw.specbuf;

	i = ox = 0;
	for (x = x1; x < x2; x++) {
		new = line[x];
		if (new.mode == ATTR_WDUMMY)
			continue;
		if (selected(x, y1))
			#if SELECTION_COLORS_PATCH
			new.mode |= ATTR_SELECTED;
			#else
			new.mode ^= ATTR_REVERSE;
			#endif // SELECTION_COLORS_PATCH
		if ((i > 0) && ATTRCMP(base, new)) {
			numspecs = xmakeglyphfontspecs(specs, &line[ox], x - ox, ox, y1);
			xdrawglyphfontspecs(specs, base, numspecs, ox, y1, x - ox);
			i = 0;
		}
		if (i == 0) {
			ox = x;
			base = new;
		}
		i++;
	}
	if (i > 0) {
		numspecs = xmakeglyphfontspecs(specs, &line[ox], x2 - ox, ox, y1);
		xdrawglyphfontspecs(specs, base, numspecs, ox, y1, x2 - ox);
	}

	#if KEYBOARDSELECT_PATCH && REFLOW_PATCH
	kbds_drawstatusbar(y1);
	#endif // KEYBOARDSELECT_PATCH
}
#elif WIDE_GLYPHS_PATCH
void
xdrawline(Line line, int x1, int y1, int x2)
{
	int i, x, ox, numspecs, numspecs_cached;
	Glyph base, new;
	GlyphFontSpec *specs;

	numspecs_cached = xmakeglyphfontspecs(xw.specbuf, &line[x1], x2 - x1, x1, y1);

	/* Draw line in 2 passes: background and foreground. This way wide glyphs
	   won't get truncated (#223) */
	for (int dmode = DRAW_BG; dmode <= DRAW_FG; dmode <<= 1) {
		specs = xw.specbuf;
		numspecs = numspecs_cached;
		i = ox = 0;
		for (x = x1; x < x2 && i < numspecs; x++) {
			new = line[x];
			if (new.mode == ATTR_WDUMMY)
				continue;
			if (selected(x, y1))
				#if SELECTION_COLORS_PATCH
				new.mode |= ATTR_SELECTED;
				#else
				new.mode ^= ATTR_REVERSE;
				#endif // SELECTION_COLORS_PATCH
			if (i > 0 && ATTRCMP(base, new)) {
				xdrawglyphfontspecs(specs, base, i, ox, y1, dmode);
				specs += i;
				numspecs -= i;
				i = 0;
			}
			if (i == 0) {
				ox = x;
				base = new;
			}
			i++;
		}
		if (i > 0)
			xdrawglyphfontspecs(specs, base, i, ox, y1, dmode);
	}

	#if KEYBOARDSELECT_PATCH && REFLOW_PATCH
	kbds_drawstatusbar(y1);
	#endif // KEYBOARDSELECT_PATCH
}
#else // !WIDE_GLYPHS_PATCH and !LIGATURES_PATCH
void
xdrawline(Line line, int x1, int y1, int x2)
{
	int i, x, ox, numspecs;
	Glyph base, new;

	GlyphFontSpec *specs = xw.specbuf;

	numspecs = xmakeglyphfontspecs(specs, &line[x1], x2 - x1, x1, y1);
	i = ox = 0;
	for (x = x1; x < x2 && i < numspecs; x++) {
		new = line[x];
		if (new.mode == ATTR_WDUMMY)
			continue;
		if (selected(x, y1))
			#if SELECTION_COLORS_PATCH
			new.mode |= ATTR_SELECTED;
			#else
			new.mode ^= ATTR_REVERSE;
			#endif // SELECTION_COLORS_PATCH
		if (i > 0 && ATTRCMP(base, new)) {
			xdrawglyphfontspecs(specs, base, i, ox, y1);
			specs += i;
			numspecs -= i;
			i = 0;
		}
		if (i == 0) {
			ox = x;
			base = new;
		}
		i++;
	}
	if (i > 0)
		xdrawglyphfontspecs(specs, base, i, ox, y1);

	#if KEYBOARDSELECT_PATCH && REFLOW_PATCH
	kbds_drawstatusbar(y1);
	#endif // KEYBOARDSELECT_PATCH
}
#endif // WIDE_GLYPHS_PATCH | LIGATURES_PATCH

void
xfinishdraw(void)
{
	#if SIXEL_PATCH
	ImageList *im, *next;
	cairo_surface_t *src, *scaled;
	cairo_t *cr;
	int width, height;
	int del, desty, mode, x1, x2, xend;
	#if ANYSIZE_PATCH
	int bw = win.hborderpx, bh = win.vborderpx;
	#else
	int bw = borderpx, bh = borderpx;
	#endif // ANYSIZE_PATCH
	Line line;

	/* the images are painted with their own clip */
	resetclip();

	for (im = term.images; im; im = next) {
		next = im->next;

		/* do not draw or process the image, if it is not visible */
		if (im->x >= term.col || im->y >= term.row || im->y < 0)
			continue;

		#if KEYBOARDSELECT_PATCH && REFLOW_PATCH
		/* do not draw the image on the search bar */
		if (im->y == term.row-1 && IS_SET(MODE_KBDSELECT) && kbds_issearchmode())
			continue;
		#endif // KEYBOARDSELECT_PATCH

		/* scale the image */
		width = MAX(im->width * win.cw / im->cw, 1);
		height = MAX(im->height * win.ch / im->ch, 1);
		if (!im->pixmap) {
			/* the sixel pixel format (0xAARRGGBB, alpha 0 or 255) matches ARGB32 */
			src = cairo_image_surface_create_for_data(im->pixels,
					CAIRO_FORMAT_ARGB32, im->width, im->height, im->width * 4);
			if (cairo_surface_status(src) != CAIRO_STATUS_SUCCESS) {
				cairo_surface_destroy(src);
				continue;
			}
			scaled = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
			if (cairo_surface_status(scaled) != CAIRO_STATUS_SUCCESS) {
				cairo_surface_destroy(scaled);
				cairo_surface_destroy(src);
				continue;
			}
			cr = cairo_create(scaled);
			cairo_scale(cr, (double)width / im->width, (double)height / im->height);
			cairo_set_source_surface(cr, src, 0, 0);
			cairo_pattern_set_filter(cairo_get_source(cr),
					(width == im->width && height == im->height) ?
					CAIRO_FILTER_NEAREST : CAIRO_FILTER_GOOD);
			cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
			cairo_paint(cr);
			cairo_destroy(cr);
			cairo_surface_destroy(src);
			im->pixmap = scaled;
		}
		scaled = im->pixmap;

		desty = bh + im->y * win.ch;

		/* draw only the parts of the image that are not erased */
		#if SCROLLBACK_PATCH || REFLOW_PATCH
		line = TLINE(im->y) + im->x;
		#else
		line = term.line[im->y] + im->x;
		#endif // SCROLLBACK_PATCH || REFLOW_PATCH
		xend = MIN(im->x + im->cols, term.col);
		for (del = 1, x1 = im->x; x1 < xend; x1 = x2) {
			mode = line->mode & ATTR_SIXEL;
			for (x2 = x1 + 1; x2 < xend; x2++) {
				if (((++line)->mode & ATTR_SIXEL) != mode)
					break;
			}
			if (mode) {
				int sx = (x1 - im->x) * win.cw;
				int sw = MIN((x2 - x1) * win.cw, width - sx);
				int dx = bw + x1 * win.cw;
				if (sw <= 0)
					continue;
				cairo_save(xw.draw);
				cairo_rectangle(xw.draw, dx, desty, sw, height);
				cairo_clip(xw.draw);
				cairo_set_operator(xw.draw, im->transparent ?
						CAIRO_OPERATOR_OVER : CAIRO_OPERATOR_SOURCE);
				cairo_set_source_surface(xw.draw, scaled, dx - sx, desty);
				cairo_paint(xw.draw);
				cairo_restore(xw.draw);
				adddamage(dx, desty, sw, height);
				del = 0;
			}
		}

		/* if all the parts are erased, we can delete the entire image */
		if (del && im->x + im->cols <= term.col)
			delete_image(im);
	}
	#endif // SIXEL_PATCH

	present();
}

void
xximspot(int x, int y)
{
	ime.spotx = x;
	ime.spoty = y;
	if (xw.textinput && ime.active) {
		textinput_setrect(x, y);
		zwp_text_input_v3_commit(xw.textinput);
	}
}

void
xsetpointermotion(int set)
{
	/* motion events are always delivered on wayland */
}

void
xsetmode(int set, unsigned int flags)
{
	int mode = win.mode;
	MODBIT(win.mode, set, flags);
	#if OPENURLONCLICK_PATCH
	if (win.mode & MODE_MOUSE && xw.pointerisvisible)
		wlsetpointer(POINTER_TEXT);
	#endif // OPENURLONCLICK_PATCH
	if ((win.mode & MODE_REVERSE) != (mode & MODE_REVERSE))
		redraw();
}

int
xsetcursor(int cursor)
{
	#if BLINKING_CURSOR_PATCH
	if (!BETWEEN(cursor, 0, 8)) /* 7-8: st extensions */
	#else
	if (!BETWEEN(cursor, 0, 7)) /* 7: st extension */
	#endif // BLINKING_CURSOR_PATCH
		return 1;
	#if DEFAULT_CURSOR_PATCH
	#if BLINKING_CURSOR_PATCH
	win.cursor = (cursor ? cursor : cursorstyle);
	#else
	win.cursor = (cursor ? cursor : cursorshape);
	#endif // BLINKING_CURSOR_PATCH
	#else
	win.cursor = cursor;
	#endif // DEFAULT_CURSOR_PATCH
	#if BLINKING_CURSOR_PATCH
	cursorblinks = win.cursor == 0 || win.cursor == 1 ||
	               win.cursor == 3 || win.cursor == 5 ||
	               win.cursor == 7;
	#endif // BLINKING_CURSOR_PATCH
	return 0;
}

void
xbell(void)
{
	if (bellvolume && xw.bell)
		xdg_system_bell_v1_ring(xw.bell, xw.win);
	#if VISUALBELL_1_PATCH
	if (!bellon) /* turn visual bell on */
		bellon = 1;
	#endif // VISUALBELL_1_PATCH
}

int
match(uint mask, uint state)
{
	return mask == XK_ANY_MOD || mask == (state & ~ignoremod);
}

char*
kmap(KeySym k, uint state)
{
	Key *kp;
	int i;

	/* Check for mapped keys out of X11 function keys. */
	for (i = 0; i < LEN(mappedkeys); i++) {
		if (mappedkeys[i] == k)
			break;
	}
	if (i == LEN(mappedkeys)) {
		if ((k & 0xFFFF) < 0xFD00)
			return NULL;
	}

	for (kp = key; kp < key + LEN(key); kp++) {
		if (kp->k != k)
			continue;

		if (!match(kp->mask, state))
			continue;

		if (IS_SET(MODE_APPKEYPAD) ? kp->appkey < 0 : kp->appkey > 0)
			continue;
		if (IS_SET(MODE_NUMLOCK) && kp->appkey == 2)
			continue;

		if (IS_SET(MODE_APPCURSOR) ? kp->appcursor < 0 : kp->appcursor > 0)
			continue;

		return kp->s;
	}

	return NULL;
}

void
kpress(xkb_keycode_t keycode)
{
	KeySym ksym = XKB_KEY_NoSymbol;
	char buf[64], *customkey;
	int len, screen;
	uint state;
	Rune c;
	Shortcut *bp;

	if (!xw.xkbstate)
		return;

	#if HIDECURSOR_PATCH
	if (xw.pointerisvisible && hidecursor) {
		#if OPENURLONCLICK_PATCH
		#if ANYSIZE_PATCH
		int x = xw.ptrx - win.hborderpx;
		int y = xw.ptry - win.vborderpx;
		#else
		int x = xw.ptrx - borderpx;
		int y = xw.ptry - borderpx;
		#endif // ANYSIZE_PATCH
		LIMIT(x, 0, win.tw - 1);
		LIMIT(y, 0, win.th - 1);
		if (!detecturl(x / win.cw, y / win.ch, 0)) {
			wlsetpointer(POINTER_HIDDEN);
			xw.pointerisvisible = 0;
		}
		#else
		wlsetpointer(POINTER_HIDDEN);
		xw.pointerisvisible = 0;
		#endif // OPENURLONCLICK_PATCH
	}
	#endif // HIDECURSOR_PATCH

	if (IS_SET(MODE_KBDLOCK))
		return;

	ksym = xkb_state_key_get_one_sym(xw.xkbstate, keycode);
	len = xkb_state_key_get_utf8(xw.xkbstate, keycode, buf, sizeof buf);
	if (len >= sizeof buf)
		return;
	state = modstate();

	#if KEYBOARDSELECT_PATCH && REFLOW_PATCH
	if (IS_SET(MODE_KBDSELECT) ) {
		if (kbds_issearchmode()) {
			screen = tisaltscr() ? S_ALT : S_PRI;
			for (bp = shortcuts; bp < shortcuts + LEN(shortcuts); bp++) {
				if (ksym == bp->keysym && match(bp->mod, state) &&
						(!bp->screen || bp->screen == screen) &&
						(bp->func == clippaste || bp->func == selpaste)) {
					bp->func(&(bp->arg));
					return;
				}
			}
		}
		if (match(XK_NO_MOD, state) ||
			(XKB_KEY_Shift_L | XKB_KEY_Shift_R) & state )
			win.mode ^= kbds_keyboardhandler(ksym, buf, len, 0);
		return;
	}
	#elif KEYBOARDSELECT_PATCH
	if ( IS_SET(MODE_KBDSELECT) ) {
		if ( match(XK_NO_MOD, state) ||
			(XKB_KEY_Shift_L | XKB_KEY_Shift_R) & state )
			win.mode ^= trt_kbdselect(ksym, buf, len);
		return;
	}
	#endif // KEYBOARDSELECT_PATCH

	screen = tisaltscr() ? S_ALT : S_PRI;

	/* 1. shortcuts */
	for (bp = shortcuts; bp < shortcuts + LEN(shortcuts); bp++) {
		if (ksym == bp->keysym && match(bp->mod, state) &&
				(!bp->screen || bp->screen == screen)) {
			bp->func(&(bp->arg));
			return;
		}
	}

	/* 2. custom keys from config.h */
	if ((customkey = kmap(ksym, state))) {
		ttywrite(customkey, strlen(customkey), 1);
		return;
	}

	/* 3. composed string from input method */
	if (len == 0)
		return;
	if (len == 1 && state & Mod1Mask) {
		if (IS_SET(MODE_8BIT)) {
			if (*buf < 0177) {
				c = *buf | 0x80;
				len = utf8encode(c, buf);
			}
		} else {
			buf[1] = buf[0];
			buf[0] = '\033';
			len = 2;
		}
	}
	ttywrite(buf, len, 1);
}

/* milliseconds until the next key repeat, or -1 */
static double
repeattimeout(struct timespec *now)
{
	double t;

	if (!kbrepeat.active || kbrepeat.rate <= 0)
		return -1;
	t = TIMEDIFF(kbrepeat.next, (*now));
	return t < 0 ? 0 : t;
}

static void
repeatkey(struct timespec *now)
{
	long interval;

	if (!kbrepeat.active || kbrepeat.rate <= 0)
		return;
	if (TIMEDIFF((*now), kbrepeat.next) < 0)
		return;
	kpress(kbrepeat.key);
	wlevents++;
	interval = 1000000000L / kbrepeat.rate;
	kbrepeat.next.tv_nsec += interval;
	while (kbrepeat.next.tv_nsec >= 1000000000L) {
		kbrepeat.next.tv_nsec -= 1000000000L;
		kbrepeat.next.tv_sec++;
	}
	/* do not let repeats pile up if we were busy */
	if (TIMEDIFF((*now), kbrepeat.next) > 0)
		kbrepeat.next = *now;
}

void
run(void)
{
	fd_set rfd, wfd;
	int wlfd = wl_display_get_fd(xw.dpy), ttyfd, xev, drawing, nfds, i;
	int prevevents, needdraw = 1, blinkattr = 0;
	struct timespec seltv, *tv, now, lastblink, trigger;
	double timeout, rt;

	/* Waiting for the first configure event (the window is mapped) */
	while (!xw.configured) {
		if (wl_display_dispatch(xw.dpy) < 0)
			die("wayland display error: %s\n", strerror(errno));
	}

	ttyfd = ttynew(opt_line, shell, opt_io, opt_cmd);
	ttystarted = 1;
	cresize(win.w, win.h);

	/*
	 * On wayland the window only appears once a buffer has been
	 * committed, so present the (empty) initial frame right away instead
	 * of waiting for the first tty output or input event.
	 */
	draw();
	wl_display_flush(xw.dpy);
	needdraw = 0;

	for (timeout = -1, drawing = 0, lastblink = (struct timespec){0};;) {
		/* dispatch what is already queued and flush requests */
		while (wl_display_prepare_read(xw.dpy) != 0) {
			if (wl_display_dispatch_pending(xw.dpy) < 0)
				die("wayland display error: %s\n", strerror(errno));
		}
		wl_display_flush(xw.dpy);

		FD_ZERO(&rfd);
		FD_ZERO(&wfd);
		FD_SET(ttyfd, &rfd);
		FD_SET(wlfd, &rfd);
		nfds = MAX(wlfd, ttyfd);
		if (paste.fd >= 0) {
			FD_SET(paste.fd, &rfd);
			nfds = MAX(nfds, paste.fd);
		}
		for (i = 0; i < NSENDJOBS; i++) {
			if (sendjobs[i].data) {
				FD_SET(sendjobs[i].fd, &wfd);
				nfds = MAX(nfds, sendjobs[i].fd);
			}
		}

		#if SYNC_PATCH
		if (ttyread_pending())
			timeout = 0;
		#endif // SYNC_PATCH

		clock_gettime(CLOCK_MONOTONIC, &now);
		rt = repeattimeout(&now);
		if (rt >= 0 && (timeout < 0 || rt < timeout))
			timeout = rt;

		seltv.tv_sec = timeout / 1E3;
		seltv.tv_nsec = 1E6 * (timeout - 1E3 * seltv.tv_sec);
		tv = timeout >= 0 ? &seltv : NULL;

		if (pselect(nfds+1, &rfd, &wfd, NULL, tv, NULL) < 0) {
			wl_display_cancel_read(xw.dpy);
			if (errno == EINTR)
				continue;
			die("select failed: %s\n", strerror(errno));
		}
		clock_gettime(CLOCK_MONOTONIC, &now);

		if (FD_ISSET(wlfd, &rfd))
			wl_display_read_events(xw.dpy);
		else
			wl_display_cancel_read(xw.dpy);

		#if SYNC_PATCH
		int ttyin = FD_ISSET(ttyfd, &rfd) || ttyread_pending();
		if (ttyin)
			ttyread();
		#else
		if (FD_ISSET(ttyfd, &rfd))
			ttyread();
		#endif // SYNC_PATCH

		prevevents = wlevents;
		if (wl_display_dispatch_pending(xw.dpy) < 0)
			die("wayland display error: %s\n", strerror(errno));
		repeatkey(&now);
		if (paste.fd >= 0 && FD_ISSET(paste.fd, &rfd)) {
			pasteread();
			wlevents++;
		}
		for (i = 0; i < NSENDJOBS; i++) {
			if (sendjobs[i].data && FD_ISSET(sendjobs[i].fd, &wfd))
				sendqueue_flush(sendjobs[i].fd);
		}
		xev = wlevents != prevevents;

		/*
		 * To reduce flicker and tearing, when new content or event
		 * triggers drawing, we first wait a bit to ensure we got
		 * everything, and if nothing new arrives - we draw.
		 * We start with trying to wait minlatency ms. If more content
		 * arrives sooner, we retry with shorter and shorter periods,
		 * and eventually draw even without idle after maxlatency ms.
		 * Typically this results in low latency while interacting,
		 * maximum latency intervals during `cat huge.txt`, and perfect
		 * sync with periodic updates from animations/key-repeats/etc.
		 */
		#if SYNC_PATCH
		if (ttyin || xev)
		#else
		if (FD_ISSET(ttyfd, &rfd) || xev)
		#endif // SYNC_PATCH
		{
			if (!drawing) {
				trigger = now;
				#if BLINKING_CURSOR_PATCH
				win.mode &= ~MODE_BLINK;
				lastblink = now;
				#endif // BLINKING_CURSOR_PATCH
				drawing = 1;
			}
			needdraw = 1;
			timeout = (maxlatency - TIMEDIFF(now, trigger)) \
			          / maxlatency * minlatency;
			if (timeout > 0)
				continue;  /* we have time, try to find idle */
		}

		#if SYNC_PATCH
		if (tinsync(su_timeout)) {
			/*
			 * on synchronized-update draw-suspension: don't reset
			 * drawing so that we draw ASAP once we can (just after
			 * ESU). it won't be too soon because we already can
			 * draw now but we skip. we set timeout > 0 to draw on
			 * SU-timeout even without new content.
			 */
			timeout = minlatency;
			continue;
		}
		#endif // SYNC_PATCH

		/* idle detected or maxlatency exhausted -> draw */
		timeout = -1;
		#if BLINKING_CURSOR_PATCH
		if (blinktimeout && (cursorblinks || blinkattr))
		#else
		if (blinktimeout && blinkattr)
		#endif // BLINKING_CURSOR_PATCH
		{
			timeout = blinktimeout - TIMEDIFF(now, lastblink);
			if (timeout <= 0) {
				if (-timeout > blinktimeout) /* start visible */
					win.mode |= MODE_BLINK;
				win.mode ^= MODE_BLINK;
				tsetdirtattr(ATTR_BLINK);
				lastblink = now;
				timeout = blinktimeout;
				needdraw = 1;
			}
		}

		/*
		 * Unlike X11, the compositor answers every commit with a
		 * wl_buffer.release event, which wakes this loop up.  Only
		 * draw when terminal content, input or the blink timer asked
		 * for it, otherwise draw() -> present() -> release -> draw()
		 * would spin at 100% CPU.
		 *
		 * Also never draw faster than the compositor displays frames:
		 * while the previous frame's callback is outstanding, keep
		 * accumulating changes and wait for it (with a safety timeout).
		 */
		if (needdraw || presentpending) {
			double fw = framewait(&now);
			if (fw >= 0) {
				if (timeout < 0 || fw < timeout)
					timeout = fw;
				continue;
			}
		}

		#if VISUALBELL_1_PATCH
		if (bellon) {
			bellon++;
			bellon %= 3;
			MODBIT(win.mode, !IS_SET(MODE_REVERSE), MODE_REVERSE);
			redraw();
		}
		else if (needdraw || presentpending)
			draw();
		#else
		if (needdraw || presentpending)
			draw();
		#endif // VISUALBELL_1_PATCH
		/*
		 * Scanning the whole screen for blinking attributes on every
		 * loop iteration is surprisingly expensive under heavy output;
		 * it only needs to reflect what is currently on screen.
		 */
		if (needdraw)
			blinkattr = tattrset(ATTR_BLINK);
		wl_display_flush(xw.dpy);
		drawing = 0;
		needdraw = 0;
	}
}

void
usage(void)
{
	die("usage: %s [-aiv] [-c class]"
		#if WORKINGDIR_PATCH
		" [-d path]"
		#endif // WORKINGDIR_PATCH
		" [-f font] [-g geometry]"
		" [-n name] [-o file]\n"
		"          [-T title] [-t title] [-w windowid]"
		" [[-e] command [args ...]]\n"
		"       %s [-aiv] [-c class]"
		#if WORKINGDIR_PATCH
		" [-d path]"
		#endif // WORKINGDIR_PATCH
		" [-f font] [-g geometry]"
		" [-n name] [-o file]\n"
		"          [-T title] [-t title] [-w windowid] -l line"
		" [stty_args ...]\n", argv0, argv0);
}

/* minimal XParseGeometry: [=][<width>x<height>][{+-}<x>{+-}<y>] */
static int
parsegeometry(const char *s, int *x, int *y, unsigned int *w, unsigned int *h)
{
	int mask = 0;
	char *end;
	unsigned long v;

	if (*s == '=')
		s++;
	if (*s != '+' && *s != '-') {
		v = strtoul(s, &end, 10);
		if (end != s) {
			*w = v;
			mask |= 4; /* WidthValue */
			s = end;
			if (*s == 'x' || *s == 'X') {
				s++;
				v = strtoul(s, &end, 10);
				if (end != s) {
					*h = v;
					mask |= 8; /* HeightValue */
					s = end;
				}
			}
		}
	}
	if (*s == '+' || *s == '-') {
		int neg = *s == '-';
		v = strtoul(s + 1, &end, 10);
		if (end != s + 1) {
			*x = neg ? -(int)v : (int)v;
			mask |= 1 | (neg ? 16 : 0);
			s = end;
			if (*s == '+' || *s == '-') {
				neg = *s == '-';
				v = strtoul(s + 1, &end, 10);
				if (end != s + 1) {
					*y = neg ? -(int)v : (int)v;
					mask |= 2 | (neg ? 32 : 0);
				}
			}
		}
	}
	return mask;
}

int
main(int argc, char *argv[])
{
	xw.l = xw.t = 0;
	xw.isfixed = 0;
	#if BLINKING_CURSOR_PATCH
	xsetcursor(cursorstyle);
	#else
	xsetcursor(cursorshape);
	#endif // BLINKING_CURSOR_PATCH

	ARGBEGIN {
	case 'a':
		allowaltscreen = 0;
		break;
	#if ALPHA_PATCH
	case 'A':
		opt_alpha = EARGF(usage());
		break;
	#endif // ALPHA_PATCH
	case 'c':
		opt_class = EARGF(usage());
		break;
	#if WORKINGDIR_PATCH
	case 'd':
		opt_dir = EARGF(usage());
		break;
	#endif // WORKINGDIR_PATCH
	case 'e':
		if (argc > 0)
			--argc, ++argv;
		goto run;
	case 'f':
		opt_font = EARGF(usage());
		break;
	case 'g':
		xw.gm = parsegeometry(EARGF(usage()),
				&xw.l, &xw.t, &cols, &rows);
		#if ANYGEOMETRY_PATCH
		geometry = CellGeometry;
		#endif // ANYGEOMETRY_PATCH
		break;
	#if ANYGEOMETRY_PATCH
	case 'G':
		xw.gm = parsegeometry(EARGF(usage()),
		        &xw.l, &xw.t, &width, &height);
		geometry = PixelGeometry;
		break;
	#endif // ANYGEOMETRY_PATCH
	case 'i':
		xw.isfixed = 1;
		break;
	case 'o':
		opt_io = EARGF(usage());
		break;
	case 'l':
		opt_line = EARGF(usage());
		break;
	case 'n':
		opt_name = EARGF(usage());
		break;
	case 't':
	case 'T':
		opt_title = EARGF(usage());
		break;
	case 'w':
		opt_embed = EARGF(usage());
		break;
	case 'v':
		die("%s " VERSION "\n", argv0);
		break;
	default:
		usage();
	} ARGEND;

run:
	if (argc > 0) /* eat all remaining arguments */
		opt_cmd = argv;

	if (!opt_title)
		opt_title = (opt_line || !opt_cmd) ? "st" : opt_cmd[0];

	setlocale(LC_CTYPE, "");
	#if LIGATURES_PATCH
	hbcreatebuffer();
	#endif // LIGATURES_PATCH

	#if ANYGEOMETRY_PATCH
	switch (geometry) {
	case CellGeometry:
		xinit(cols, rows);
		break;
	case PixelGeometry:
		xinit(width, height);
		cols = (win.w - 2 * borderpx) / win.cw;
		rows = (win.h - 2 * borderpx) / win.ch;
		break;
	}
	#endif // ANYGEOMETRY_PATCH

	cols = MAX(cols, 1);
	rows = MAX(rows, 1);
	#if ALPHA_PATCH && ALPHA_FOCUS_HIGHLIGHT_PATCH
	defaultbg = MAX(LEN(colorname), 256);
	#endif // ALPHA_FOCUS_HIGHLIGHT_PATCH
	tnew(cols, rows);
	#if !ANYGEOMETRY_PATCH
	xinit(cols, rows);
	#endif // ANYGEOMETRY_PATCH
	xsetenv();
	selinit();
	#if WORKINGDIR_PATCH
	if (opt_dir && chdir(opt_dir))
		die("Can't change to working directory %s\n", opt_dir);
	#endif // WORKINGDIR_PATCH
	run();
	#if LIGATURES_PATCH
	hbdestroybuffer();
	#endif // LIGATURES_PATCH

	return 0;
}
