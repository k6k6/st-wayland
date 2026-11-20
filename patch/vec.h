/* Small path renderer backed by FreeType outlines and pixman. */
#ifndef ST_VEC_H
#define ST_VEC_H
#include <pixman.h>
#include <stddef.h>

typedef struct VecContext VecContext;
typedef struct VecPath VecPath;
typedef struct VecPattern VecPattern;
typedef struct { double xx, yx, xy, yy, x0, y0; } VecMatrix;
typedef enum { VEC_OPERATOR_OVER, VEC_OPERATOR_SOURCE, VEC_OPERATOR_CLEAR,
               VEC_OPERATOR_DEST_OUT, VEC_OPERATOR_ADD } VecOperator;
typedef enum { VEC_ANTIALIAS_DEFAULT, VEC_ANTIALIAS_NONE, VEC_ANTIALIAS_GRAY } VecAntialias;
typedef enum { VEC_FILL_RULE_WINDING, VEC_FILL_RULE_EVEN_ODD } VecFillRule;
typedef enum { VEC_LINE_CAP_BUTT, VEC_LINE_CAP_ROUND, VEC_LINE_CAP_SQUARE } VecLineCap;
typedef enum { VEC_LINE_JOIN_MITER, VEC_LINE_JOIN_ROUND, VEC_LINE_JOIN_BEVEL } VecLineJoin;
typedef enum { VEC_EXTEND_NONE, VEC_EXTEND_REPEAT, VEC_EXTEND_PAD } VecExtend;
typedef enum { VEC_FILTER_NEAREST, VEC_FILTER_BILINEAR } VecFilter;
enum { VEC_STATUS_SUCCESS = 0, VEC_STATUS_ERROR = 1, VEC_CONTENT_ALPHA = 1 };

VecContext *vec_create(pixman_image_t *);
void vec_destroy(VecContext *);
int vec_status(const VecContext *);
#ifdef VEC_CACHE_TEST
typedef struct {
    size_t hits, misses, evictions, entries, bytes;
} VecCacheStats;
void vec_test_cache_enable(VecContext *, int);
VecCacheStats vec_test_cache_stats(const VecContext *);
#endif
void vec_save(VecContext *);
void vec_restore(VecContext *);
void vec_new_path(VecContext *);
void vec_new_sub_path(VecContext *);
void vec_move_to(VecContext *, double, double);
void vec_line_to(VecContext *, double, double);
void vec_curve_to(VecContext *, double, double, double, double, double, double);
void vec_close_path(VecContext *);
void vec_rectangle(VecContext *, double, double, double, double);
void vec_arc(VecContext *, double, double, double, double, double);
void vec_arc_negative(VecContext *, double, double, double, double, double);
VecPath *vec_copy_path(VecContext *);
void vec_append_path(VecContext *, const VecPath *);
void vec_path_destroy(VecPath *);
int vec_path_equal(const VecPath *, const VecPath *);
void vec_path_extents(VecContext *, double *, double *, double *, double *);
void vec_fill(VecContext *);
void vec_stroke(VecContext *);
void vec_clip(VecContext *);
void vec_reset_clip(VecContext *);
void vec_clip_extents(VecContext *, double *, double *, double *, double *);
void vec_translate(VecContext *, double, double);
void vec_scale(VecContext *, double, double);
void vec_get_matrix(VecContext *, VecMatrix *);
void vec_set_matrix(VecContext *, const VecMatrix *);
void vec_identity_matrix(VecContext *);
void vec_matrix_init_identity(VecMatrix *);
void vec_matrix_translate(VecMatrix *, double, double);
void vec_matrix_scale(VecMatrix *, double, double);
void vec_set_operator(VecContext *, VecOperator);
VecOperator vec_get_operator(VecContext *);
void vec_set_antialias(VecContext *, VecAntialias);
VecAntialias vec_get_antialias(VecContext *);
void vec_set_fill_rule(VecContext *, VecFillRule);
VecFillRule vec_get_fill_rule(VecContext *);
void vec_set_line_width(VecContext *, double);
double vec_get_line_width(VecContext *);
void vec_set_line_cap(VecContext *, VecLineCap);
VecLineCap vec_get_line_cap(VecContext *);
void vec_set_line_join(VecContext *, VecLineJoin);
VecLineJoin vec_get_line_join(VecContext *);
void vec_set_miter_limit(VecContext *, double);
double vec_get_miter_limit(VecContext *);
void vec_set_dash(VecContext *, const double *, int, double);
int vec_get_dash_count(VecContext *);
void vec_get_dash(VecContext *, double *, double *);
void vec_set_source_rgba(VecContext *, double, double, double, double);
void vec_set_source_rgb(VecContext *, double, double, double);
void vec_set_source(VecContext *, VecPattern *);
VecPattern *vec_get_source(VecContext *);
VecPattern *vec_pattern_reference(VecPattern *);
VecPattern *vec_pattern_create_for_image(pixman_image_t *);
VecPattern *vec_pattern_create_linear(double, double, double, double);
void vec_pattern_add_color_stop_rgba(VecPattern *, double, double, double, double, double);
void vec_pattern_set_extend(VecPattern *, VecExtend);
void vec_pattern_set_filter(VecPattern *, VecFilter);
void vec_pattern_set_matrix(VecPattern *, const VecMatrix *);
void vec_pattern_destroy(VecPattern *);
void vec_mask(VecContext *, VecPattern *);
void vec_paint(VecContext *);
void vec_push_group_with_content(VecContext *, int);
VecPattern *vec_pop_group(VecContext *);
#endif
