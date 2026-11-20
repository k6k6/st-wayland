/* See LICENSE for license details. Software paths for the pixman renderer. */
#include "vec.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_STROKER_H
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define DASH_MAX 16
#define CACHE_BUCKETS 64
#define CACHE_ENTRIES 128
#define CACHE_BYTES (512 * 1024)
#define CACHE_ITEM_BYTES (32 * 1024)
#define CACHE_COMMANDS 64
#define CACHE_WORDS (CACHE_COMMANDS * 7 + 64)

typedef struct {
	double x, y;
} Point;
typedef struct {
	unsigned char type;
	Point p[3];
} Command;
enum { MOVE, LINE, CUBIC, CLOSE };
struct VecPath {
	Command *commands;
	size_t count, capacity;
	int current;
	Point point, start;
};
typedef struct {
	int x1, y1, x2, y2;
	pixman_image_t *mask;
} Clip;
struct VecPattern {
	unsigned refs;
	pixman_image_t *image;
	int gradient;
	Point a, b;
	pixman_gradient_stop_t *stops;
	int nstops;
	VecMatrix matrix;
	VecExtend extend;
	VecFilter filter;
};
typedef struct State {
	VecMatrix matrix, source_matrix;
	VecPattern *source;
	Clip clip;
	VecOperator op;
	VecAntialias aa;
	VecFillRule fillrule;
	VecLineCap cap;
	VecLineJoin join;
	double width, miter, dashes[DASH_MAX], dashoffset;
	int ndash;
	struct State *previous;
} State;
typedef struct Group {
	pixman_image_t *image;
	int x, y;
	struct Group *previous;
} Group;
typedef struct {
	pixman_image_t *image;
	int x, y, w, h;
} Mask;
typedef struct CacheEntry {
	struct CacheEntry *next, *older, *newer;
	uint64_t hash;
	size_t count, bytes;
	Mask mask;
	uint64_t words[];
} CacheEntry;
struct VecContext {
	pixman_image_t *image;
	int x, y, error;
	State state;
	VecPath path;
	Group *groups;
	FT_Library ft;
	FT_Stroker stroker;
	CacheEntry *cache[CACHE_BUCKETS], *oldest, *newest;
	size_t cachebytes, cachecount;
#ifdef VEC_CACHE_TEST
	int cache_disabled;
	VecCacheStats cachestats;
#endif
};
typedef struct {
	uint64_t words[CACHE_WORDS], hash;
	size_t count;
	int x, y;
} CacheQuery;

static void
cacheunlink(VecContext *c, CacheEntry *e)
{
	if (e->older)
		e->older->newer = e->newer;
	else
		c->oldest = e->newer;
	if (e->newer)
		e->newer->older = e->older;
	else
		c->newest = e->older;
}
static void
cachetouch(VecContext *c, CacheEntry *e)
{
	e->older = c->newest;
	e->newer = NULL;
	if (c->newest)
		c->newest->newer = e;
	else
		c->oldest = e;
	c->newest = e;
}
static void
cacheevict(VecContext *c)
{
	CacheEntry *e = c->oldest;
	CacheEntry **link = &c->cache[e->hash % CACHE_BUCKETS];
	while (*link != e)
		link = &(*link)->next;
	*link = e->next;
	cacheunlink(c, e);
	c->cachebytes -= e->bytes;
	c->cachecount--;
	pixman_image_unref(e->mask.image);
	free(e);
#ifdef VEC_CACHE_TEST
	c->cachestats.evictions++;
#endif
}
#ifdef VEC_CACHE_TEST
void
vec_test_cache_enable(VecContext *c, int enabled)
{
	c->cache_disabled = !enabled;
}
VecCacheStats
vec_test_cache_stats(const VecContext *c)
{
	VecCacheStats s = c->cachestats;
	s.entries = c->cachecount;
	s.bytes = c->cachebytes;
	return s;
}
#endif

void
vec_matrix_init_identity(VecMatrix *m)
{
	*m = (VecMatrix){1, 0, 0, 1, 0, 0};
}
static VecMatrix
multiply(VecMatrix a, VecMatrix b)
{
	return (VecMatrix){a.xx * b.xx + a.xy * b.yx,	     a.yx * b.xx + a.yy * b.yx,
			   a.xx * b.xy + a.xy * b.yy,	     a.yx * b.xy + a.yy * b.yy,
			   a.xx * b.x0 + a.xy * b.y0 + a.x0, a.yx * b.x0 + a.yy * b.y0 + a.y0};
}
static int
inverse(VecMatrix a, VecMatrix *b)
{
	double d = a.xx * a.yy - a.xy * a.yx;
	if (!isfinite(d) || fabs(d) < 1e-15)
		return 0;
	*b = (VecMatrix){a.yy / d, -a.yx / d, -a.xy / d, a.xx / d, 0, 0};
	b->x0 = -(b->xx * a.x0 + b->xy * a.y0);
	b->y0 = -(b->yx * a.x0 + b->yy * a.y0);
	return 1;
}
static Point
transform(VecMatrix m, Point p)
{
	return (Point){m.xx * p.x + m.xy * p.y + m.x0, m.yx * p.x + m.yy * p.y + m.y0};
}
void
vec_matrix_translate(VecMatrix *m, double x, double y)
{
	*m = multiply(*m, (VecMatrix){1, 0, 0, 1, x, y});
}
void
vec_matrix_scale(VecMatrix *m, double x, double y)
{
	*m = multiply(*m, (VecMatrix){x, 0, 0, y, 0, 0});
}
void
vec_translate(VecContext *c, double x, double y)
{
	vec_matrix_translate(&c->state.matrix, x, y);
}
void
vec_scale(VecContext *c, double x, double y)
{
	vec_matrix_scale(&c->state.matrix, x, y);
}
void
vec_get_matrix(VecContext *c, VecMatrix *m)
{
	*m = c->state.matrix;
}
void
vec_set_matrix(VecContext *c, const VecMatrix *m)
{
	c->state.matrix = *m;
}
void
vec_identity_matrix(VecContext *c)
{
	vec_matrix_init_identity(&c->state.matrix);
}
static void
clipfree(Clip *clip)
{
	if (clip->mask)
		pixman_image_unref(clip->mask);
	clip->mask = NULL;
}
static void
statefree(State *s)
{
	clipfree(&s->clip);
	vec_pattern_destroy(s->source);
}
static void
statecopy(State *to, const State *from)
{
	*to = *from;
	if (to->clip.mask)
		pixman_image_ref(to->clip.mask);
	vec_pattern_reference(to->source);
}
int
vec_status(const VecContext *c)
{
	return !c || c->error ? VEC_STATUS_ERROR : VEC_STATUS_SUCCESS;
}
void
vec_save(VecContext *c)
{
	State *s = malloc(sizeof(*s));
	if (!s) {
		c->error = 1;
		return;
	}
	statecopy(s, &c->state);
	c->state.previous = s;
}
void
vec_restore(VecContext *c)
{
	State *s = c->state.previous;
	if (!s) {
		c->error = 1;
		return;
	}
	statefree(&c->state);
	c->state = *s;
	free(s);
}
void
vec_reset_clip(VecContext *c)
{
	clipfree(&c->state.clip);
	c->state.clip = (Clip){c->x, c->y, c->x + pixman_image_get_width(c->image),
			       c->y + pixman_image_get_height(c->image), NULL};
}
VecContext *
vec_create(pixman_image_t *image)
{
	VecContext *c;
	if (!image)
		return NULL;
	c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	c->image = pixman_image_ref(image);
	c->state.width = 2;
	c->state.miter = 10;
	vec_matrix_init_identity(&c->state.matrix);
	vec_matrix_init_identity(&c->state.source_matrix);
	if (FT_Init_FreeType(&c->ft) || FT_Stroker_New(c->ft, &c->stroker))
		c->error = 1;
	vec_reset_clip(c);
	vec_set_source_rgb(c, 0, 0, 0);
	if (c->error) {
		vec_destroy(c);
		return NULL;
	}
	return c;
}
void
vec_destroy(VecContext *c)
{
	if (!c)
		return;
	while (c->groups) {
		VecPattern *p = vec_pop_group(c);
		vec_pattern_destroy(p);
	}
	while (c->state.previous)
		vec_restore(c);
	statefree(&c->state);
	while (c->oldest)
		cacheevict(c);
	free(c->path.commands);
	pixman_image_unref(c->image);
	if (c->stroker)
		FT_Stroker_Done(c->stroker);
	if (c->ft)
		FT_Done_FreeType(c->ft);
	free(c);
}
#define PROPERTY(name, type, field)                                                                \
	void vec_set_##name(VecContext *c, type v) { c->state.field = v; }                         \
	type vec_get_##name(VecContext *c) { return c->state.field; }
PROPERTY(operator, VecOperator, op)
PROPERTY(antialias, VecAntialias, aa)
PROPERTY(fill_rule, VecFillRule, fillrule)
PROPERTY(line_width, double, width)
PROPERTY(line_cap, VecLineCap, cap)
PROPERTY(line_join, VecLineJoin, join)
PROPERTY(miter_limit, double, miter)
#undef PROPERTY
void
vec_set_dash(VecContext *c, const double *d, int n, double offset)
{
	if (n < 0 || n > DASH_MAX || !isfinite(offset)) {
		c->error = 1;
		return;
	}
	double sum = 0;
	for (int i = 0; i < n; i++) {
		if (!isfinite(d[i]) || d[i] < 0) {
			c->error = 1;
			return;
		}
		sum += d[i];
	}
	if (n && sum <= 0) {
		c->error = 1;
		return;
	}
	c->state.ndash = n;
	c->state.dashoffset = offset;
	if (n)
		memcpy(c->state.dashes, d, n * sizeof(*d));
}
int
vec_get_dash_count(VecContext *c)
{
	return c->state.ndash;
}
void
vec_get_dash(VecContext *c, double *d, double *offset)
{
	if (d)
		memcpy(d, c->state.dashes, c->state.ndash * sizeof(*d));
	if (offset)
		*offset = c->state.dashoffset;
}

static int
reserve(VecContext *c, size_t n)
{
	VecPath *p = &c->path;
	if (n <= p->capacity)
		return 1;
	if (n > 32760) {
		c->error = 1;
		return 0;
	}
	size_t cap = MAX(n, MAX(32, p->capacity * 2));
	Command *q = realloc(p->commands, cap * sizeof(*q));
	if (!q) {
		c->error = 1;
		return 0;
	}
	p->commands = q;
	p->capacity = cap;
	return 1;
}
static void
command(VecContext *c, unsigned char type, Point a, Point b, Point d)
{
	if (!reserve(c, c->path.count + 1))
		return;
	c->path.commands[c->path.count++] = (Command){type, {a, b, d}};
}
void
vec_new_path(VecContext *c)
{
	c->path.count = 0;
	c->path.current = 0;
}
void
vec_new_sub_path(VecContext *c)
{
	c->path.current = 0;
}
void
vec_move_to(VecContext *c, double x, double y)
{
	Point p = transform(c->state.matrix, (Point){x, y});
	command(c, MOVE, p, p, p);
	c->path.point = c->path.start = p;
	c->path.current = 1;
}
void
vec_line_to(VecContext *c, double x, double y)
{
	if (!c->path.current) {
		vec_move_to(c, x, y);
		return;
	}
	Point p = transform(c->state.matrix, (Point){x, y});
	command(c, LINE, p, p, p);
	c->path.point = p;
}
void
vec_curve_to(VecContext *c, double x1, double y1, double x2, double y2, double x3, double y3)
{
	if (!c->path.current)
		vec_move_to(c, x1, y1);
	Point a = transform(c->state.matrix, (Point){x1, y1}),
	      b = transform(c->state.matrix, (Point){x2, y2}),
	      p = transform(c->state.matrix, (Point){x3, y3});
	command(c, CUBIC, a, b, p);
	c->path.point = p;
}
void
vec_close_path(VecContext *c)
{
	if (!c->path.current)
		return;
	command(c, CLOSE, c->path.start, c->path.start, c->path.start);
	c->path.point = c->path.start;
	c->path.current = 0;
}
void
vec_rectangle(VecContext *c, double x, double y, double w, double h)
{
	vec_move_to(c, x, y);
	vec_line_to(c, x + w, y);
	vec_line_to(c, x + w, y + h);
	vec_line_to(c, x, y + h);
	vec_close_path(c);
}
static void
arc(VecContext *c, double x, double y, double r, double a, double b, int negative)
{
	if (r < 0 || !isfinite(a) || !isfinite(b) || !isfinite(r)) {
		c->error = 1;
		return;
	}
	if (negative) {
		while (b > a)
			b -= 2 * M_PI;
		if (a - b > 2 * M_PI)
			b = a - 2 * M_PI;
	} else {
		while (b < a)
			b += 2 * M_PI;
		if (b - a > 2 * M_PI)
			b = a + 2 * M_PI;
	}
	if (c->path.current)
		vec_line_to(c, x + r * cos(a), y + r * sin(a));
	else
		vec_move_to(c, x + r * cos(a), y + r * sin(a));
	int n = (int)ceil(fabs(b - a) / (M_PI / 2));
	if (!n)
		return;
	double step = (b - a) / n;
	for (int i = 0; i < n; i++) {
		double t = a + step, k = 4. / 3 * tan(step / 4);
		vec_curve_to(c, x + r * (cos(a) - k * sin(a)), y + r * (sin(a) + k * cos(a)),
			     x + r * (cos(t) + k * sin(t)), y + r * (sin(t) - k * cos(t)),
			     x + r * cos(t), y + r * sin(t));
		a = t;
	}
}
void
vec_arc(VecContext *c, double x, double y, double r, double a, double b)
{
	arc(c, x, y, r, a, b, 0);
}
void
vec_arc_negative(VecContext *c, double x, double y, double r, double a, double b)
{
	arc(c, x, y, r, a, b, 1);
}
VecPath *
vec_copy_path(VecContext *c)
{
	VecPath *p = malloc(sizeof(*p));
	if (!p) {
		c->error = 1;
		return NULL;
	}
	*p = c->path;
	p->commands = NULL;
	p->capacity = p->count;
	if (p->count) {
		p->commands = malloc(p->count * sizeof(Command));
		if (!p->commands) {
			free(p);
			c->error = 1;
			return NULL;
		}
		memcpy(p->commands, c->path.commands, p->count * sizeof(Command));
	}
	return p;
}
void
vec_append_path(VecContext *c, const VecPath *p)
{
	if (!p || !p->count)
		return;
	if (!reserve(c, c->path.count + p->count))
		return;
	memcpy(c->path.commands + c->path.count, p->commands, p->count * sizeof(Command));
	c->path.count += p->count;
	c->path.current = p->current;
	c->path.point = p->point;
	c->path.start = p->start;
}
void
vec_path_destroy(VecPath *p)
{
	if (p) {
		free(p->commands);
		free(p);
	}
}
int
vec_path_equal(const VecPath *a, const VecPath *b)
{
	if (!a || !b || a->count != b->count)
		return 0;
	for (size_t i = 0; i < a->count; i++) {
		if (a->commands[i].type != b->commands[i].type)
			return 0;
		for (int j = 0; j < 3; j++)
			if (a->commands[i].p[j].x != b->commands[i].p[j].x ||
			    a->commands[i].p[j].y != b->commands[i].p[j].y)
				return 0;
	}
	return 1;
}
static void
extents(VecContext *c, int clip, double *x1, double *y1, double *x2, double *y2)
{
	VecMatrix inv;
	if (!inverse(c->state.matrix, &inv)) {
		*x1 = *y1 = *x2 = *y2 = 0;
		return;
	}
	*x1 = *y1 = HUGE_VAL;
	*x2 = *y2 = -HUGE_VAL;
	if (clip) {
		Clip *k = &c->state.clip;
		Point ps[] = {{k->x1, k->y1}, {k->x2, k->y1}, {k->x2, k->y2}, {k->x1, k->y2}};
		for (int i = 0; i < 4; i++) {
			Point p = transform(inv, ps[i]);
			*x1 = MIN(*x1, p.x);
			*x2 = MAX(*x2, p.x);
			*y1 = MIN(*y1, p.y);
			*y2 = MAX(*y2, p.y);
		}
	} else
		for (size_t i = 0; i < c->path.count; i++) {
			Command *k = &c->path.commands[i];
			for (int j = 0; j < (k->type == CUBIC ? 3 : 1); j++) {
				Point p = transform(inv, k->p[j]);
				*x1 = MIN(*x1, p.x);
				*x2 = MAX(*x2, p.x);
				*y1 = MIN(*y1, p.y);
				*y2 = MAX(*y2, p.y);
			}
		}
	if (!isfinite(*x1))
		*x1 = *y1 = *x2 = *y2 = 0;
}
void
vec_path_extents(VecContext *c, double *a, double *b, double *d, double *e)
{
	extents(c, 0, a, b, d, e);
}
void
vec_clip_extents(VecContext *c, double *a, double *b, double *d, double *e)
{
	extents(c, 1, a, b, d, e);
}

static uint16_t
channel(double x)
{
	return (uint16_t)lround(MAX(0, MIN(1, x)) * 65535);
}
static pixman_color_t
color(double r, double g, double b, double a)
{
	a = MAX(0, MIN(1, a));
	return (pixman_color_t){channel(r * a), channel(g * a), channel(b * a), channel(a)};
}
static VecPattern *
pattern(void)
{
	VecPattern *p = calloc(1, sizeof(*p));
	if (p) {
		p->refs = 1;
		vec_matrix_init_identity(&p->matrix);
		p->filter = VEC_FILTER_NEAREST;
	}
	return p;
}
VecPattern *
vec_pattern_reference(VecPattern *p)
{
	if (p)
		p->refs++;
	return p;
}
void
vec_pattern_destroy(VecPattern *p)
{
	if (p && !--p->refs) {
		if (p->image)
			pixman_image_unref(p->image);
		free(p->stops);
		free(p);
	}
}
VecPattern *
vec_pattern_create_for_image(pixman_image_t *image)
{
	VecPattern *p = pattern();
	if (p && image)
		p->image = pixman_image_ref(image);
	return p;
}
VecPattern *
vec_pattern_create_linear(double x1, double y1, double x2, double y2)
{
	VecPattern *p = pattern();
	if (p) {
		p->gradient = 1;
		p->a = (Point){x1, y1};
		p->b = (Point){x2, y2};
		p->extend = VEC_EXTEND_PAD;
	}
	return p;
}
void
vec_pattern_add_color_stop_rgba(VecPattern *p, double at, double r, double g, double b, double a)
{
	if (!p || !p->gradient)
		return;
	pixman_gradient_stop_t *s = realloc(p->stops, (p->nstops + 1) * sizeof(*s));
	if (!s)
		return;
	p->stops = s;
	int i = p->nstops++;
	while (i && pixman_fixed_to_double(s[i - 1].x) > at) {
		s[i] = s[i - 1];
		i--;
	}
	/* Pixman gradient stops are unpremultiplied, unlike solid colors. */
	s[i] = (pixman_gradient_stop_t){pixman_double_to_fixed(MAX(0, MIN(1, at))),
					{channel(r), channel(g), channel(b), channel(a)}};
}
void
vec_pattern_set_extend(VecPattern *p, VecExtend e)
{
	if (p)
		p->extend = e;
}
void
vec_pattern_set_filter(VecPattern *p, VecFilter f)
{
	if (p)
		p->filter = f;
}
void
vec_pattern_set_matrix(VecPattern *p, const VecMatrix *m)
{
	if (p)
		p->matrix = *m;
}
void
vec_set_source(VecContext *c, VecPattern *p)
{
	vec_pattern_reference(p);
	vec_pattern_destroy(c->state.source);
	c->state.source = p;
	c->state.source_matrix = c->state.matrix;
	if (!p)
		c->error = 1;
}
VecPattern *
vec_get_source(VecContext *c)
{
	return c->state.source;
}
void
vec_set_source_rgba(VecContext *c, double r, double g, double b, double a)
{
	VecPattern *p = pattern();
	if (!p) {
		c->error = 1;
		return;
	}
	pixman_color_t col = color(r, g, b, a);
	p->image = pixman_image_create_solid_fill(&col);
	if (!p->image)
		c->error = 1;
	vec_set_source(c, p);
	vec_pattern_destroy(p);
}
void
vec_set_source_rgb(VecContext *c, double r, double g, double b)
{
	vec_set_source_rgba(c, r, g, b, 1);
}
static pixman_image_t *
patternimage(VecContext *c, VecPattern *p, VecMatrix ctm)
{
	if (!p)
		return NULL;
	pixman_image_t *im;
	if (p->gradient) {
		pixman_point_fixed_t a = {pixman_double_to_fixed(p->a.x),
					  pixman_double_to_fixed(p->a.y)},
				     b = {pixman_double_to_fixed(p->b.x),
					  pixman_double_to_fixed(p->b.y)};
		im = pixman_image_create_linear_gradient(&a, &b, p->stops, p->nstops);
	} else
		im = p->image ? pixman_image_ref(p->image) : NULL;
	if (!im) {
		c->error = 1;
		return NULL;
	}
	VecMatrix inv;
	if (!inverse(ctm, &inv)) {
		pixman_image_unref(im);
		c->error = 1;
		return NULL;
	}
	VecMatrix m = multiply(p->matrix, inv);
	pixman_transform_t t = {{{pixman_double_to_fixed(m.xx), pixman_double_to_fixed(m.xy),
				  pixman_double_to_fixed(m.x0)},
				 {pixman_double_to_fixed(m.yx), pixman_double_to_fixed(m.yy),
				  pixman_double_to_fixed(m.y0)},
				 {0, 0, pixman_fixed_1}}};
	pixman_image_set_transform(im, &t);
	pixman_image_set_repeat(im, p->extend == VEC_EXTEND_REPEAT ? PIXMAN_REPEAT_NORMAL
				    : p->extend == VEC_EXTEND_PAD  ? PIXMAN_REPEAT_PAD
								   : PIXMAN_REPEAT_NONE);
	pixman_image_set_filter(
	    im, p->filter == VEC_FILTER_BILINEAR ? PIXMAN_FILTER_BILINEAR : PIXMAN_FILTER_NEAREST,
	    NULL, 0);
	return im;
}

/* FreeType uses 26.6 coordinates with y pointing up. Keep path commands in
 * device coordinates, and invert the current CTM only while constructing a
 * stroke so non-uniform scales apply to the pen as well as its centreline. */
static FT_Vector
vector(Point p)
{
	return (FT_Vector){(FT_Pos)llround(p.x * 64), (FT_Pos)llround(-p.y * 64)};
}
static int
validpoint(Point p)
{
	return isfinite(p.x) && isfinite(p.y) && fabs(p.x) < 8388607 && fabs(p.y) < 8388607;
}
static void
cacheword(CacheQuery *q, uint64_t word)
{
	q->words[q->count++] = word;
	q->hash = (q->hash ^ word) * UINT64_C(1099511628211);
}
static void
cachedouble(CacheQuery *q, double value)
{
	uint64_t bits;
	memcpy(&bits, &value, sizeof(bits));
	cacheword(q, bits);
}
/* Only coverage is cached: colors, source transforms and compositing operators
 * are still evaluated on every draw. Fractional clips modify masks in place,
 * so they must never receive a shared cache image. */
static int
cachequery(VecContext *c, int stroke, CacheQuery *q)
{
	State *s = &c->state;
	Clip clip = s->clip;
	if (clip.mask || c->path.count > CACHE_COMMANDS)
		return 0;
#ifdef VEC_CACHE_TEST
	if (c->cache_disabled)
		return 0;
#endif
	q->count = 0;
	q->x = q->y = 0;
	q->hash = UINT64_C(14695981039346656037);
	cacheword(q, stroke);
	cacheword(q, s->aa);
	cacheword(q, c->path.count);
	VecMatrix inv;
	int translated = 0;
	if (stroke) {
		/* Identical stroker inputs under an integer translation have exactly
		 * translated 26.6 output. Other CTMs retain their full absolute key:
		 * normalizing floating-point transforms can change boundary rounding.
		 * Modest limits keep translated output well inside validpoint(). */
		VecMatrix m = s->matrix;
		translated = m.xx == 1 && m.yy == 1 && m.xy == 0 && m.yx == 0 &&
			     fabs(m.x0) < 32768 && fabs(m.y0) < 32768 && m.x0 == floor(m.x0) &&
			     m.y0 == floor(m.y0) && s->width > 0 && s->width <= 1024 &&
			     s->miter >= 0 && s->miter <= 1024;
		if (translated) {
			q->x = m.x0;
			q->y = m.y0;
			inverse(m, &inv);
			m.x0 = m.y0 = 0;
		}
		cacheword(q, translated);
		cachedouble(q, m.xx);
		cachedouble(q, m.xy);
		cachedouble(q, m.yx);
		cachedouble(q, m.yy);
		cachedouble(q, m.x0);
		cachedouble(q, m.y0);
		cachedouble(q, s->width);
		cachedouble(q, s->miter);
		cacheword(q, s->cap);
		cacheword(q, s->join);
		cacheword(q, s->ndash);
		cachedouble(q, s->dashoffset);
		for (int i = 0; i < s->ndash; i++)
			cachedouble(q, s->dashes[i]);
	} else {
		Point p = c->path.commands[0].p[0];
		if (!validpoint(p))
			return 0;
		q->x = floor(p.x);
		q->y = floor(p.y);
		cacheword(q, s->fillrule);
	}
	FT_Pos xmin = LONG_MAX, ymin = LONG_MAX, xmax = LONG_MIN, ymax = LONG_MIN;
	for (size_t i = 0; i < c->path.count; i++) {
		Command *k = &c->path.commands[i];
		cacheword(q, k->type);
		int n = stroke || k->type == CUBIC ? 3 : k->type == CLOSE ? 0 : 1;
		for (int j = 0; j < n; j++) {
			Point p = k->p[j];
			if (stroke) {
				if (translated) {
					p = transform(inv, p);
					if (!isfinite(p.x) || !isfinite(p.y) ||
					    fabs(p.x) >= 32768 || fabs(p.y) >= 32768)
						return 0;
				}
				cachedouble(q, p.x);
				cachedouble(q, p.y);
			} else {
				if (!validpoint(p))
					return 0;
				FT_Vector v = vector(p);
				xmin = MIN(xmin, v.x);
				xmax = MAX(xmax, v.x);
				ymin = MIN(ymin, -v.y);
				ymax = MAX(ymax, -v.y);
				/* Quantize before translating: llround() treats negative
				 * half-way values differently from positive ones. */
				cacheword(q, (int64_t)v.x - (int64_t)q->x * 64);
				cacheword(q, -(int64_t)v.y - (int64_t)q->y * 64);
			}
		}
	}
	if (!stroke && xmin != LONG_MAX) {
		/* A larger surrounding clip does not alter this path's raster. */
		clip.x1 = MAX(clip.x1, (int)floor(xmin / 64.));
		clip.y1 = MAX(clip.y1, (int)floor(ymin / 64.));
		clip.x2 = MIN(clip.x2, (int)ceil(xmax / 64.));
		clip.y2 = MIN(clip.y2, (int)ceil(ymax / 64.));
	}
	cacheword(q, (int64_t)clip.x1 - q->x);
	cacheword(q, (int64_t)clip.y1 - q->y);
	cacheword(q, (int64_t)clip.x2 - q->x);
	cacheword(q, (int64_t)clip.y2 - q->y);
	q->hash ^= q->hash >> 33;
	q->hash *= UINT64_C(0xff51afd7ed558ccd);
	q->hash ^= q->hash >> 33;
	return 1;
}
static int
cachefind(VecContext *c, const CacheQuery *q, Mask *mask)
{
	for (CacheEntry *e = c->cache[q->hash % CACHE_BUCKETS]; e; e = e->next) {
		if (e->hash != q->hash || e->count != q->count ||
		    memcmp(e->words, q->words, q->count * sizeof(*q->words)))
			continue;
		cacheunlink(c, e);
		cachetouch(c, e);
		*mask = e->mask;
		mask->x += q->x;
		mask->y += q->y;
		pixman_image_ref(mask->image);
#ifdef VEC_CACHE_TEST
		c->cachestats.hits++;
#endif
		return 1;
	}
#ifdef VEC_CACHE_TEST
	c->cachestats.misses++;
#endif
	return 0;
}
static void
cacheinsert(VecContext *c, const CacheQuery *q, Mask mask)
{
	if (!mask.image)
		return;
	size_t keybytes = sizeof(CacheEntry) + q->count * sizeof(*q->words);
	/* Charge image bookkeeping conservatively as well as the full stride. */
	size_t bytes = keybytes + 512 + (size_t)pixman_image_get_stride(mask.image) * mask.h;
	if (bytes > CACHE_ITEM_BYTES)
		return;
	CacheEntry *e = malloc(keybytes);
	if (!e)
		return;
	while (c->cachecount >= CACHE_ENTRIES || c->cachebytes + bytes > CACHE_BYTES)
		cacheevict(c);
	*e = (CacheEntry){.next = c->cache[q->hash % CACHE_BUCKETS],
			  .hash = q->hash,
			  .count = q->count,
			  .bytes = bytes,
			  .mask = mask};
	memcpy(e->words, q->words, q->count * sizeof(*q->words));
	e->mask.x -= q->x;
	e->mask.y -= q->y;
	pixman_image_ref(e->mask.image);
	c->cache[q->hash % CACHE_BUCKETS] = e;
	cachetouch(c, e);
	c->cachecount++;
	c->cachebytes += bytes;
}
static int
filloutline(VecContext *c, FT_Outline *out)
{
	size_t n = 0, contours = 0;
	int active = 0;
	for (size_t i = 0; i < c->path.count; i++) {
		Command *k = &c->path.commands[i];
		if (k->type == MOVE) {
			contours++;
			n++;
		} else if (k->type == CUBIC)
			n += 3;
		else if (k->type == LINE)
			n++;
	}
	if (n > 32760 || contours > 32760 || FT_Outline_New(c->ft, n, contours, out)) {
		c->error = 1;
		return 0;
	}
	out->n_points = out->n_contours = 0;
	for (size_t i = 0; i < c->path.count; i++) {
		Command *k = &c->path.commands[i];
		if (k->type == MOVE) {
			if (active)
				out->contours[out->n_contours++] = out->n_points - 1;
			active = 1;
		}
		if (k->type == CLOSE) {
			if (active)
				out->contours[out->n_contours++] = out->n_points - 1;
			active = 0;
			continue;
		}
		for (int j = 0; j < (k->type == CUBIC ? 3 : 1); j++) {
			if (!validpoint(k->p[j])) {
				FT_Outline_Done(c->ft, out);
				c->error = 1;
				return 0;
			}
			out->points[out->n_points] = vector(k->p[j]);
			out->tags[out->n_points++] =
			    k->type == CUBIC && j < 2 ? FT_CURVE_TAG_CUBIC : FT_CURVE_TAG_ON;
		}
	}
	if (active)
		out->contours[out->n_contours++] = out->n_points - 1;
	out->flags |= c->state.fillrule == VEC_FILL_RULE_EVEN_ODD ? FT_OUTLINE_EVEN_ODD_FILL : 0;
	return 1;
}
typedef struct {
	VecContext *c;
	double remain;
	int index, on, active;
	Point point;
} Dash;
static void
dashreset(Dash *d, VecContext *c, Point p)
{
	*d = (Dash){.c = c, .point = p};
	double total = 0;
	int n = c->state.ndash;
	for (int i = 0; i < n; i++)
		total += c->state.dashes[i];
	if (n & 1)
		total *= 2;
	double off = fmod(c->state.dashoffset, total);
	if (off < 0)
		off += total;
	d->remain = c->state.dashes[0];
	d->on = 1;
	while (off >= d->remain) {
		off -= d->remain;
		d->index++;
		d->on = !d->on;
		d->remain = c->state.dashes[d->index % n];
	}
	d->remain -= off;
}
static int
dashline(Dash *d, Point end)
{
	VecContext *c = d->c;
	Point start = d->point;
	double dx = end.x - start.x, dy = end.y - start.y, len = hypot(dx, dy), pos = 0;
	if (len < 1e-12) {
		d->point = end;
		return 1;
	}
	while (pos < len - 1e-10) {
		if (d->remain < 1e-10) {
			if (d->active) {
				if (FT_Stroker_EndSubPath(c->stroker))
					return 0;
				d->active = 0;
			}
			d->index++;
			d->on = !d->on;
			d->remain = c->state.dashes[d->index % c->state.ndash];
			continue;
		}
		double step = MIN(d->remain, len - pos);
		Point a = {start.x + dx * pos / len, start.y + dy * pos / len};
		pos += step;
		Point b = {start.x + dx * pos / len, start.y + dy * pos / len};
		if (d->on) {
			FT_Vector va = vector(a), vb = vector(b);
			if (!d->active) {
				if (FT_Stroker_BeginSubPath(c->stroker, &va, 1))
					return 0;
				d->active = 1;
			}
			if (FT_Stroker_LineTo(c->stroker, &vb))
				return 0;
		}
		d->remain -= step;
	}
	d->point = end;
	return 1;
}
static int
dashcurve(Dash *d, Point a, Point b, Point end, int depth)
{
	Point start = d->point;
	double dist =
	    fabs((end.x - start.x) * (a.y - start.y) - (end.y - start.y) * (a.x - start.x)) +
	    fabs((end.x - start.x) * (b.y - start.y) - (end.y - start.y) * (b.x - start.x));
	double length = hypot(end.x - start.x, end.y - start.y);
	if (depth >= 12 || (length > 1e-10 && dist <= .08 * length))
		return dashline(d, end);
	Point x = {(start.x + a.x) / 2, (start.y + a.y) / 2},
	      y = {(a.x + b.x) / 2, (a.y + b.y) / 2}, z = {(b.x + end.x) / 2, (b.y + end.y) / 2};
	Point u = {(x.x + y.x) / 2, (x.y + y.y) / 2}, v = {(y.x + z.x) / 2, (y.y + z.y) / 2},
	      m = {(u.x + v.x) / 2, (u.y + v.y) / 2};
	return dashcurve(d, x, u, m, depth + 1) && dashcurve(d, v, z, end, depth + 1);
}
static int
strokeoutline(VecContext *c, FT_Outline *out)
{
	VecMatrix inv;
	if (!inverse(c->state.matrix, &inv) || c->state.width <= 0)
		return 0;
	FT_Stroker_LineCap cap = c->state.cap == VEC_LINE_CAP_ROUND    ? FT_STROKER_LINECAP_ROUND
				 : c->state.cap == VEC_LINE_CAP_SQUARE ? FT_STROKER_LINECAP_SQUARE
								       : FT_STROKER_LINECAP_BUTT;
	FT_Stroker_LineJoin join = c->state.join == VEC_LINE_JOIN_ROUND ? FT_STROKER_LINEJOIN_ROUND
				   : c->state.join == VEC_LINE_JOIN_BEVEL
				       ? FT_STROKER_LINEJOIN_BEVEL
				       : FT_STROKER_LINEJOIN_MITER_FIXED;
	FT_Stroker_Set(c->stroker, (FT_Fixed)llround(c->state.width * 32), cap, join,
		       (FT_Fixed)llround(c->state.miter * 65536));
	FT_Stroker_Rewind(c->stroker);
	for (size_t i = 0; i < c->path.count;) {
		if (c->path.commands[i].type != MOVE) {
			i++;
			continue;
		}
		size_t end = i + 1;
		while (end < c->path.count && c->path.commands[end].type != MOVE)
			end++;
		int closed = c->path.commands[end - 1].type == CLOSE;
		Point start = transform(inv, c->path.commands[i].p[0]);
		if (!validpoint(start))
			goto fail;
		FT_Vector v = vector(start);
		Dash dash;
		if (c->state.ndash)
			dashreset(&dash, c, start);
		else if (FT_Stroker_BeginSubPath(c->stroker, &v, !closed))
			goto fail;
		for (size_t j = i + 1; j < end; j++) {
			Command *k = &c->path.commands[j];
			Point p[3];
			for (int q = 0; q < 3; q++) {
				p[q] = transform(inv, k->p[q]);
				if (!validpoint(p[q]))
					goto fail;
			}
			if (c->state.ndash) {
				if (k->type == CUBIC) {
					if (!dashcurve(&dash, p[0], p[1], p[2], 0))
						goto fail;
				} else if (k->type == LINE || k->type == CLOSE) {
					if (!dashline(&dash, p[0]))
						goto fail;
				}
			} else if (k->type == LINE) {
				v = vector(p[0]);
				if (FT_Stroker_LineTo(c->stroker, &v))
					goto fail;
			} else if (k->type == CUBIC) {
				FT_Vector a = vector(p[0]), b = vector(p[1]), d = vector(p[2]);
				if (FT_Stroker_CubicTo(c->stroker, &a, &b, &d))
					goto fail;
			}
		}
		if (!c->state.ndash || dash.active)
			if (FT_Stroker_EndSubPath(c->stroker))
				goto fail;
		i = end;
	}
	FT_UInt points, contours;
	if (FT_Stroker_GetCounts(c->stroker, &points, &contours) || points > 32760 ||
	    contours > 32760 || FT_Outline_New(c->ft, points, contours, out))
		goto fail;
	out->n_points = out->n_contours = 0;
	FT_Stroker_Export(c->stroker, out);
	for (int i = 0; i < out->n_points; i++) {
		Point p = {out->points[i].x / 64., -out->points[i].y / 64.};
		p = transform(c->state.matrix, p);
		if (!validpoint(p)) {
			FT_Outline_Done(c->ft, out);
			goto fail;
		}
		out->points[i] = vector(p);
	}
	return 1;
fail:
	c->error = 1;
	return 0;
}
typedef struct {
	unsigned char *data;
	int stride, height, binary;
} Spans;
static void
spans(int y, int count, const FT_Span *span, void *user)
{
	Spans *s = user;
	if (y < 0 || y >= s->height)
		return;
	unsigned char *row = s->data + (s->height - 1 - y) * s->stride;
	for (int i = 0; i < count; i++) {
		unsigned char coverage =
		    s->binary ? (span[i].coverage >= 128 ? 255 : 0) : span[i].coverage;
		memset(row + span[i].x, coverage, span[i].len);
	}
}
static Mask
raster(VecContext *c, FT_Outline *out)
{
	Mask m = {0};
	if (!out->n_points)
		return m;
	FT_BBox b;
	FT_Outline_Get_CBox(out, &b);
	Clip *k = &c->state.clip;
	int x1 = MAX((int)floor(b.xMin / 64.), k->x1), x2 = MIN((int)ceil(b.xMax / 64.), k->x2);
	int y1 = MAX((int)floor(-b.yMax / 64.), k->y1), y2 = MIN((int)ceil(-b.yMin / 64.), k->y2);
	if (x2 <= x1 || y2 <= y1)
		return m;
	m = (Mask){pixman_image_create_bits(PIXMAN_a8, x2 - x1, y2 - y1, NULL, 0), x1, y1, x2 - x1,
		   y2 - y1};
	if (!m.image) {
		c->error = 1;
		return m;
	}
	FT_Outline_Translate(out, -x1 * 64, y2 * 64);
	Spans s = {(void *)pixman_image_get_data(m.image), pixman_image_get_stride(m.image), m.h,
		   c->state.aa == VEC_ANTIALIAS_NONE};
	FT_Raster_Params p = {0};
	p.flags = FT_RASTER_FLAG_AA | FT_RASTER_FLAG_DIRECT | FT_RASTER_FLAG_CLIP;
	p.gray_spans = spans;
	p.user = &s;
	p.clip_box = (FT_BBox){0, 0, m.w, m.h};
	if (FT_Outline_Render(c->ft, out, &p)) {
		c->error = 1;
		pixman_image_unref(m.image);
		m.image = NULL;
	}
	return m;
}
static Mask
pathmask(VecContext *c, int stroke)
{
	FT_Outline out = {0};
	Mask mask = {0};
	if (c->error || !c->path.count)
		return mask;
	CacheQuery query;
	int cacheable = cachequery(c, stroke, &query);
	if (cacheable && cachefind(c, &query, &mask))
		return mask;
	if (stroke ? strokeoutline(c, &out) : filloutline(c, &out)) {
		mask = raster(c, &out);
		FT_Outline_Done(c->ft, &out);
		if (cacheable)
			cacheinsert(c, &query, mask);
	}
	return mask;
}
static void
applyclip(VecContext *c, Mask *m)
{
	Clip *k = &c->state.clip;
	if (k->mask && m->image)
		pixman_image_composite32(PIXMAN_OP_IN_REVERSE, k->mask, NULL, m->image,
					 m->x - k->x1, m->y - k->y1, 0, 0, 0, 0, m->w, m->h);
}
static void
composite(VecContext *c, Mask *m)
{
	if (!m->image)
		return;
	applyclip(c, m);
	pixman_image_t *src = patternimage(c, c->state.source, c->state.source_matrix);
	if (!src)
		return;
	int dx = m->x - c->x, dy = m->y - c->y;
	if (c->state.op == VEC_OPERATOR_SOURCE || c->state.op == VEC_OPERATOR_CLEAR) {
		pixman_image_composite32(PIXMAN_OP_OUT_REVERSE, m->image, NULL, c->image, 0, 0, 0,
					 0, dx, dy, m->w, m->h);
		if (c->state.op == VEC_OPERATOR_SOURCE)
			pixman_image_composite32(PIXMAN_OP_ADD, src, m->image, c->image, m->x, m->y,
						 0, 0, dx, dy, m->w, m->h);
	} else
		pixman_image_composite32(
		    c->state.op == VEC_OPERATOR_DEST_OUT ? PIXMAN_OP_OUT_REVERSE
		    : c->state.op == VEC_OPERATOR_ADD	 ? PIXMAN_OP_ADD
							 : PIXMAN_OP_OVER,
		    src, m->image, c->image, m->x, m->y, 0, 0, dx, dy, m->w, m->h);
	pixman_image_unref(src);
}
void
vec_fill(VecContext *c)
{
	Mask m = pathmask(c, 0);
	composite(c, &m);
	if (m.image)
		pixman_image_unref(m.image);
	vec_new_path(c);
}
void
vec_stroke(VecContext *c)
{
	Mask m = pathmask(c, 1);
	composite(c, &m);
	if (m.image)
		pixman_image_unref(m.image);
	vec_new_path(c);
}
static int
rectpath(VecContext *c, int *left, int *top, int *right, int *bottom)
{
	if (c->path.count != 5)
		return 0;
	Command *k = c->path.commands;
	if (k[0].type != MOVE || k[1].type != LINE || k[2].type != LINE || k[3].type != LINE ||
	    k[4].type != CLOSE)
		return 0;
	Point a = k[0].p[0], b = k[1].p[0], d = k[2].p[0], e = k[3].p[0];
	if (!((a.y == b.y && b.x == d.x && d.y == e.y && e.x == a.x) ||
	      (a.x == b.x && b.y == d.y && d.x == e.x && e.y == a.y)))
		return 0;
	double x1 = MIN(a.x, d.x), x2 = MAX(a.x, d.x), y1 = MIN(a.y, d.y), y2 = MAX(a.y, d.y);
	if (x1 != floor(x1) || x2 != floor(x2) || y1 != floor(y1) || y2 != floor(y2) ||
	    !validpoint(a) || !validpoint(d))
		return 0;
	*left = x1;
	*right = x2;
	*top = y1;
	*bottom = y2;
	return 1;
}
void
vec_clip(VecContext *c)
{
	int x1, y1, x2, y2;
	Clip *k = &c->state.clip;
	if (!k->mask && rectpath(c, &x1, &y1, &x2, &y2)) {
		k->x1 = MAX(x1, k->x1);
		k->y1 = MAX(y1, k->y1);
		k->x2 = MAX(k->x1, MIN(x2, k->x2));
		k->y2 = MAX(k->y1, MIN(y2, k->y2));
		vec_new_path(c);
		return;
	}
	Mask m = pathmask(c, 0);
	applyclip(c, &m);
	clipfree(k);
	*k = (Clip){m.x, m.y, m.x + m.w, m.y + m.h, m.image};
	vec_new_path(c);
}
void
vec_mask(VecContext *c, VecPattern *p)
{
	Clip *k = &c->state.clip;
	int w = k->x2 - k->x1, h = k->y2 - k->y1;
	if (w <= 0 || h <= 0)
		return;
	Mask m = {pixman_image_create_bits(PIXMAN_a8, w, h, NULL, 0), k->x1, k->y1, w, h};
	if (!m.image) {
		c->error = 1;
		return;
	}
	pixman_image_t *im = patternimage(c, p, c->state.matrix);
	if (im) {
		pixman_image_composite32(PIXMAN_OP_SRC, im, NULL, m.image, m.x, m.y, 0, 0, 0, 0, w,
					 h);
		pixman_image_unref(im);
		composite(c, &m);
	}
	pixman_image_unref(m.image);
}
void
vec_paint(VecContext *c)
{
	Clip *k = &c->state.clip;
	int w = k->x2 - k->x1, h = k->y2 - k->y1;
	if (w <= 0 || h <= 0)
		return;
	Mask m = {pixman_image_create_bits(PIXMAN_a8, w, h, NULL, 0), k->x1, k->y1, w, h};
	if (!m.image) {
		c->error = 1;
		return;
	}
	memset(pixman_image_get_data(m.image), 255, (size_t)pixman_image_get_stride(m.image) * h);
	composite(c, &m);
	pixman_image_unref(m.image);
}
void
vec_push_group_with_content(VecContext *c, int content)
{
	(void)content;
	Clip *k = &c->state.clip;
	Group *g = malloc(sizeof(*g));
	pixman_image_t *image = pixman_image_create_bits(PIXMAN_a8, MAX(1, k->x2 - k->x1),
							 MAX(1, k->y2 - k->y1), NULL, 0);
	if (!g || !image) {
		free(g);
		if (image)
			pixman_image_unref(image);
		c->error = 1;
		return;
	}
	State *before = c->state.previous;
	vec_save(c);
	if (c->state.previous == before) {
		free(g);
		pixman_image_unref(image);
		return;
	}
	*g = (Group){c->image, c->x, c->y, c->groups};
	c->groups = g;
	c->image = image;
	c->x = k->x1;
	c->y = k->y1;
}
VecPattern *
vec_pop_group(VecContext *c)
{
	Group *g = c->groups;
	if (!g) {
		c->error = 1;
		return NULL;
	}
	VecPattern *p = vec_pattern_create_for_image(c->image);
	VecMatrix origin = {1, 0, 0, 1, -c->x, -c->y};
	if (!p)
		c->error = 1;
	pixman_image_unref(c->image);
	c->image = g->image;
	c->x = g->x;
	c->y = g->y;
	c->groups = g->previous;
	free(g);
	vec_restore(c);
	/* The returned pattern belongs to the restored parent user space.
	 * Transforms used only inside the group must not move its finished pixels. */
	if (p)
		p->matrix = multiply(origin, c->state.matrix);
	return p;
}
