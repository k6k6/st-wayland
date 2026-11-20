/* Headless semantic tests for the FreeType/pixman path renderer. */
#include "../patch/vec.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define W 64
#define H 48
static uint32_t
pixel(pixman_image_t *image, int x, int y)
{
	return *(uint32_t *)((char *)pixman_image_get_data(image) +
			     y * pixman_image_get_stride(image) + 4 * x);
}
static void
clear(pixman_image_t *image)
{
	memset(pixman_image_get_data(image), 0,
	       (size_t)pixman_image_get_stride(image) * pixman_image_get_height(image));
}
static unsigned
alpha(pixman_image_t *image, int x, int y)
{
	return pixel(image, x, y) >> 24;
}
#ifdef VEC_CACHE_TEST
static void
samepixels(pixman_image_t *a, pixman_image_t *b)
{
	assert(!memcmp(pixman_image_get_data(a), pixman_image_get_data(b),
		       (size_t)pixman_image_get_stride(a) * pixman_image_get_height(a)));
}
static void
curve(VecContext *c, double d)
{
	vec_move_to(c, 1 + d, 2);
	vec_curve_to(c, 16, -2, -4, 14, 13, 13);
	vec_line_to(c, 10, 3);
	vec_close_path(c);
}
static void
cachedraw(VecContext *c, int i)
{
	int kind = i % 12, x = ((i / 12) % 6) * 9 - 4, y = ((i / 72) % 4) * 9 - 3;
	vec_save(c);
	vec_set_source_rgba(c, (i % 7) / 6., (i % 11) / 10., (i % 13) / 12., .35);
	vec_set_operator(c, (VecOperator)(i % 5));
	vec_set_antialias(c, i % 3 ? VEC_ANTIALIAS_DEFAULT : VEC_ANTIALIAS_NONE);
	vec_rectangle(c, x, y, 17, 17);
	vec_clip(c);
	vec_translate(c, x, y);
	if (kind == 1 || kind == 8) {
		/* A cached clip is retained while a second clip modifies its own
		 * mask, and while a group inherits that nonrectangular clip. */
		curve(c, .25);
		vec_clip(c);
		vec_rectangle(c, 2.125, 3.5, 10.5, 7.25);
		vec_clip(c);
	}
	if (kind >= 8)
		vec_push_group_with_content(c, VEC_CONTENT_ALPHA);
	if (kind == 2 || kind == 7) {
		VecMatrix m = {1.2, .125, -.25, .8, x + .5, y - .25};
		vec_set_matrix(c, &m);
	}
	if (kind == 4 || kind == 9) {
		double dashes[] = {1.25, 2, .5};
		vec_set_dash(c, dashes, 3, -.625);
	}
	vec_set_line_cap(c, (VecLineCap)(kind % 3));
	vec_set_line_join(c, (VecLineJoin)((kind / 3) % 3));
	vec_set_line_width(c, 1.75);
	vec_set_miter_limit(c, 2.75);
	/* 1/128 exercises opposite-sign ties in FreeType's 26.6 rounding. */
	curve(c, kind == 6 ? 1. / 128 : .25);
	if (kind & 1) {
		vec_set_fill_rule(c, kind % 3 ? VEC_FILL_RULE_EVEN_ODD : VEC_FILL_RULE_WINDING);
		vec_rectangle(c, 3, 4, 4, 4);
		vec_fill(c);
	} else {
		vec_stroke(c);
	}
	if (kind >= 8) {
		VecPattern *p = vec_pop_group(c);
		vec_mask(c, p);
		vec_pattern_destroy(p);
	}
	vec_restore(c);
}
static void
cachetest(void)
{
	pixman_image_t *a = pixman_image_create_bits(PIXMAN_a8r8g8b8, W, H, NULL, 0);
	pixman_image_t *b = pixman_image_create_bits(PIXMAN_a8r8g8b8, W, H, NULL, 0);
	VecContext *cached = vec_create(a), *plain = vec_create(b);
	assert(cached && plain);
	vec_test_cache_enable(plain, 0);
	for (int i = 0; i < 3000; i++) {
		if (i % 41 == 0) {
			clear(a);
			clear(b);
		}
		cachedraw(cached, i);
		cachedraw(plain, i);
		samepixels(a, b);
	}
	VecCacheStats s = vec_test_cache_stats(cached);
	assert(s.hits > 1000);
	/* Retain a cached clip across eviction, then use its original mask. */
	VecContext *contexts[] = {cached, plain};
	for (int j = 0; j < 2; j++) {
		curve(contexts[j], .125);
		vec_clip(contexts[j]);
		vec_save(contexts[j]);
		vec_reset_clip(contexts[j]);
	}
	for (int i = 0; i < 400; i++) {
		for (int j = 0; j < 2; j++) {
			curve(contexts[j], i / 64.);
			vec_fill(contexts[j]);
		}
		samepixels(a, b);
	}
	for (int j = 0; j < 2; j++) {
		vec_restore(contexts[j]);
		vec_set_source_rgba(contexts[j], .25, .75, .5, .625);
		vec_paint(contexts[j]);
		assert(vec_status(contexts[j]) == VEC_STATUS_SUCCESS);
	}
	samepixels(a, b);
	s = vec_test_cache_stats(cached);
	assert(s.evictions > 100 && s.entries <= 128 && s.bytes <= 512 * 1024);
	assert(vec_test_cache_stats(plain).hits == 0);
	printf("vec cache: 3400 differential draws PASS, hits=%zu misses=%zu evictions=%zu "
	       "entries=%zu bytes=%zu\n",
	       s.hits, s.misses, s.evictions, s.entries, s.bytes);
	vec_destroy(cached);
	vec_destroy(plain);
	pixman_image_unref(a);
	pixman_image_unref(b);
	/* Exercise the byte limit before the entry limit, plus large one-offs. */
	a = pixman_image_create_bits(PIXMAN_a8r8g8b8, 320, 256, NULL, 0);
	b = pixman_image_create_bits(PIXMAN_a8r8g8b8, 320, 256, NULL, 0);
	cached = vec_create(a);
	plain = vec_create(b);
	assert(cached && plain);
	vec_test_cache_enable(plain, 0);
	for (int i = 0; i < 50; i++) {
		vec_rectangle(cached, i / 64., 0, 180, 130);
		vec_fill(cached);
		vec_rectangle(plain, i / 64., 0, 180, 130);
		vec_fill(plain);
		samepixels(a, b);
	}
	s = vec_test_cache_stats(cached);
	assert(s.evictions > 0 && s.entries < 128 && s.bytes <= 512 * 1024);
	vec_rectangle(cached, 0, 0, 320, 256);
	vec_fill(cached);
	assert(vec_test_cache_stats(cached).entries == s.entries);
	assert(vec_test_cache_stats(cached).bytes == s.bytes);
	vec_destroy(cached);
	vec_destroy(plain);
	pixman_image_unref(a);
	pixman_image_unref(b);
	puts("vec cache: byte budget, entry budget, retained clips and one-off bypass PASS");
}
static void
cachebench(int enabled, int misses, int stroke)
{
	pixman_image_t *image = pixman_image_create_bits(PIXMAN_a8r8g8b8, W, H, NULL, 0);
	VecContext *c = vec_create(image);
	assert(c);
	vec_test_cache_enable(c, enabled);
	vec_set_source_rgba(c, .5, .7, .9, .8);
	const int count = 120000;
	clock_t start = clock();
	for (int i = 0; i < count; i++) {
		vec_save(c);
		int x = i % 4 * 12, y = i / 4 % 3 * 12;
		vec_rectangle(c, x, y, 12, 12);
		vec_clip(c);
		vec_translate(c, x, y);
		curve(c, misses ? (i % 512) / 64. : .25);
		if (stroke)
			vec_stroke(c);
		else
			vec_fill(c);
		vec_restore(c);
	}
	double seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
	VecCacheStats s = vec_test_cache_stats(c);
	printf("vec bench: enabled=%d misses=%d stroke=%d draws=%d seconds=%.6f "
	       "hits=%zu misses=%zu\n",
	       enabled, misses, stroke, count, seconds, s.hits, s.misses);
	assert(vec_status(c) == VEC_STATUS_SUCCESS);
	vec_destroy(c);
	pixman_image_unref(image);
}
#endif
int
main(int argc, char **argv)
{
#ifdef VEC_CACHE_TEST
	if (argc == 2 && !strcmp(argv[1], "--bench")) {
		for (int stroke = 0; stroke <= 1; stroke++)
			for (int misses = 0; misses <= 1; misses++)
				for (int enabled = 0; enabled <= 1; enabled++)
					cachebench(enabled, misses, stroke);
		return 0;
	}
	cachetest();
#else
	(void)argc;
	(void)argv;
#endif
	pixman_image_t *image = pixman_image_create_bits(PIXMAN_a8r8g8b8, W, H, NULL, 0);
	assert(image);
	VecContext *c = vec_create(image);
	assert(c && vec_status(c) == VEC_STATUS_SUCCESS);
	VecPath *empty = vec_copy_path(c);
	assert(empty);
	vec_append_path(c, empty);
	vec_path_destroy(empty);
	vec_set_source_rgba(c, 1, 0, 0, .5);
	vec_rectangle(c, 2, 3, 8, 9);
	vec_fill(c);
	assert(pixel(image, 2, 3) == 0x80800000);
	assert(pixel(image, 9, 11) == 0x80800000);
	assert(pixel(image, 10, 3) == 0);
	/* Save/restore includes clipping and the source, but deliberately not path. */
	vec_save(c);
	vec_rectangle(c, 4, 5, 2, 3);
	vec_clip(c);
	vec_set_source_rgb(c, 0, 1, 0);
	vec_paint(c);
	vec_restore(c);
	assert(pixel(image, 4, 5) == 0xff00ff00 && pixel(image, 3, 5) == 0x80800000);
	clear(image);
	vec_new_path(c);
	vec_rectangle(c, 0, 0, 12, 12);
	vec_rectangle(c, 3, 3, 6, 6);
	vec_set_fill_rule(c, VEC_FILL_RULE_EVEN_ODD);
	vec_fill(c);
	assert(alpha(image, 1, 1) == 128 && alpha(image, 5, 5) == 0);
	vec_set_fill_rule(c, VEC_FILL_RULE_WINDING);
	/* SOURCE respects fractional shape coverage rather than clearing its bbox. */
	vec_set_source_rgb(c, 0, 0, 1);
	vec_paint(c);
	vec_set_operator(c, VEC_OPERATOR_SOURCE);
	vec_set_source_rgba(c, 1, 0, 0, .5);
	vec_move_to(c, 10, 10);
	vec_line_to(c, 20, 20);
	vec_line_to(c, 10, 20);
	vec_close_path(c);
	vec_fill(c);
	assert(pixel(image, 19, 11) == 0xff0000ff);
	assert(alpha(image, 11, 18) == 128);
	vec_set_operator(c, VEC_OPERATOR_OVER);
	/* An even-odd clip leaves its hole untouched. */
	clear(image);
	vec_rectangle(c, 0, 0, 20, 20);
	vec_rectangle(c, 5, 5, 10, 10);
	vec_set_fill_rule(c, VEC_FILL_RULE_EVEN_ODD);
	vec_clip(c);
	vec_set_fill_rule(c, VEC_FILL_RULE_WINDING);
	vec_set_source_rgb(c, 1, 1, 1);
	vec_paint(c);
	assert(alpha(image, 1, 1) == 255 && alpha(image, 7, 7) == 0);
	vec_reset_clip(c);
	/* Transformed elliptic path, stroked after restoring CTM, has a circular pen. */
	clear(image);
	vec_save(c);
	vec_translate(c, 20, 20);
	vec_scale(c, 12, 6);
	vec_arc(c, 0, 0, 1, 0, 6.283185307179586);
	vec_restore(c);
	vec_set_line_width(c, 2);
	vec_stroke(c);
	assert(alpha(image, 20, 20) == 0 && alpha(image, 31, 20) > 0 && alpha(image, 20, 14) > 0);
	/* Dashed lines must contain real gaps and the proper rounded endpoints. */
	clear(image);
	double dash[] = {4, 4};
	vec_set_dash(c, dash, 2, 0);
	vec_set_line_cap(c, VEC_LINE_CAP_BUTT);
	vec_set_line_width(c, 2);
	vec_move_to(c, 4, 10);
	vec_line_to(c, 40, 10);
	vec_stroke(c);
	assert(alpha(image, 5, 9) > 0 && alpha(image, 10, 9) == 0 && alpha(image, 13, 9) > 0);
	vec_set_dash(c, NULL, 0, 0);
	/* A group builds alpha only; negative geometry removes coverage, not bg. */
	clear(image);
	vec_set_source_rgba(c, 1, 0, 0, .5);
	vec_save(c);
	vec_rectangle(c, 7, 8, 12, 13);
	vec_clip(c);
	vec_push_group_with_content(c, VEC_CONTENT_ALPHA);
	vec_set_source_rgb(c, 1, 1, 1);
	vec_rectangle(c, 7, 8, 12, 13);
	vec_fill(c);
	vec_set_operator(c, VEC_OPERATOR_DEST_OUT);
	vec_rectangle(c, 10, 11, 5, 6);
	vec_fill(c);
	VecPattern *mask = vec_pop_group(c);
	vec_mask(c, mask);
	vec_pattern_destroy(mask);
	vec_restore(c);
	assert(pixel(image, 8, 9) == 0x80800000 && pixel(image, 11, 12) == 0 &&
	       pixel(image, 20, 10) == 0);
	/* Group-local transforms do not change placement on returning to parent. */
	clear(image);
	vec_push_group_with_content(c, VEC_CONTENT_ALPHA);
	vec_translate(c, 9, 7);
	vec_set_source_rgb(c, 1, 1, 1);
	vec_rectangle(c, 0, 0, 3, 4);
	vec_fill(c);
	mask = vec_pop_group(c);
	vec_mask(c, mask);
	vec_pattern_destroy(mask);
	assert(pixel(image, 9, 7) == 0x80800000 && pixel(image, 0, 0) == 0);
	/* Device-space repeat mask remains anchored across translated cells. */
	clear(image);
	pixman_image_t *tile = pixman_image_create_bits(PIXMAN_a8, 2, 1, NULL, 0);
	assert(tile);
	((unsigned char *)pixman_image_get_data(tile))[0] = 255;
	mask = vec_pattern_create_for_image(tile);
	vec_pattern_set_extend(mask, VEC_EXTEND_REPEAT);
	vec_translate(c, 3, 4);
	VecMatrix matrix;
	vec_get_matrix(c, &matrix);
	vec_pattern_set_matrix(mask, &matrix);
	vec_rectangle(c, 0, 0, 10, 10);
	vec_clip(c);
	vec_mask(c, mask);
	assert(alpha(image, 4, 4) == 128 && alpha(image, 5, 4) == 0);
	vec_pattern_destroy(mask);
	pixman_image_unref(tile);
	vec_identity_matrix(c);
	vec_reset_clip(c);
	/* Gradient endpoint mapping is defined at pixel centers. */
	clear(image);
	VecPattern *gradient = vec_pattern_create_linear(.5, 0, 15.5, 0);
	vec_pattern_add_color_stop_rgba(gradient, 0, 1, 1, 1, 1);
	vec_pattern_add_color_stop_rgba(gradient, 1, 1, 1, 1, 0);
	vec_set_source(c, gradient);
	vec_pattern_destroy(gradient);
	vec_rectangle(c, 0, 0, 16, 3);
	vec_fill(c);
	assert(alpha(image, 0, 1) == 255 && alpha(image, 15, 1) == 0);
	for (int x = 1; x < 16; x++)
		assert(alpha(image, x, 1) <= alpha(image, x - 1, 1));
	assert(vec_status(c) == VEC_STATUS_SUCCESS);
	vec_destroy(c);
	pixman_image_unref(image);
	puts("vec: fill rules, clip/state, SOURCE coverage, transformed curves, dashes, alpha "
	     "groups, repeat masks and gradients PASS");
	return 0;
}
