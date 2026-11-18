#!/usr/bin/env python3
"""Compile-time bridge to the real kitty corner/stop functions, never a copy.

Usage: cursortrail_reference.py KITTY_SOURCE OUTPUT_HEADER
The source stays in its own checkout. Generated code lives in the disposable
cursortrail_test.sh build and is deleted with it.
"""
import hashlib
from pathlib import Path
import sys

source = Path(sys.argv[1]) / 'kitty/cursor_trail.c'
text = source.read_text()


def function(name):
    start = text.index('\n' + name + '(')
    start = text.rfind('\n', 0, start - 1) + 1
    end = text.index('\n}\n', start) + 3
    return text[start:end]


prelude = r'''
#include <float.h>
#define KITTY_REFERENCE_SHA "%s"
typedef int64_t monotonic_t;
static double monotonic_t_to_s_double(monotonic_t t) { return t / 1e9; }
typedef struct {
    float corner_x[4], corner_y[4], cursor_edge_x[2], cursor_edge_y[2];
    float opacity;
    monotonic_t updated_at;
    bool needs_render, target_updated, window_changed;
} KittyReferenceTrail;
typedef struct {
    struct { unsigned int width, height; } cell_size;
    struct { bool mDECTCEM; } modes;
} KittyReferenceScreen;
typedef struct {
    struct { KittyReferenceScreen *screen; } render_data;
} KittyReferenceWindow;
typedef struct { struct { bool in_progress; } live_resize; } KittyReferenceOSWindow;
static struct {
    float cursor_trail_decay_fast, cursor_trail_decay_slow;
    int cursor_trail_start_threshold_x, cursor_trail_start_threshold_y;
} kitty_reference_options;
#define CursorTrail KittyReferenceTrail
#define Window KittyReferenceWindow
#define OSWindow KittyReferenceOSWindow
#define OPT(x) kitty_reference_options.x
#define WD w->render_data
#define EDGE(axis, index) ct->cursor_edge_##axis[index]
#define norm kitty_reference_norm
''' % hashlib.sha256(text.encode()).hexdigest()[:12]
coords_start = text.index('typedef struct ndc_coords')
coords_end = text.index('} ndc_coords;', coords_start) + len('} ndc_coords;')
parts = [prelude, function('norm'), text[coords_start:coords_end]]
parts += [function(name) for name in ('should_skip_cursor_trail_update',
                                     'update_cursor_trail_corners',
                                     'update_cursor_trail_needs_render')]
parts += ['\n'.join('#undef ' + name for name in
                    ('CursorTrail', 'Window', 'OSWindow', 'OPT', 'WD', 'EDGE', 'norm'))]
Path(sys.argv[2]).write_text('\n\n'.join(parts) + '\n')
