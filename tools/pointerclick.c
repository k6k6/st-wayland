/*
 * Virtual pointer helper for tools/selection_sigpipe_test.sh.
 *
 *   pointerclick X Y [CLICKS] [INTERVAL_MS]
 *
 * Moves a wlr virtual pointer to (X, Y) in compositor layout coordinates and
 * clicks there CLICKS times (default 1, 2 makes a double click that selects a
 * word in st).  Requires the compositor to expose
 * zwlr_virtual_pointer_manager_v1 (wlroots based compositors do).
 */
#define _GNU_SOURCE
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct zwlr_virtual_pointer_manager_v1 *vpmgr;
static struct wl_seat *seat;
static int outw = 1920, outh = 1080;

static void
registry_global(void *data, struct wl_registry *reg, uint32_t name,
		const char *iface, uint32_t version)
{
	if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name))
		vpmgr = wl_registry_bind(reg, name,
			&zwlr_virtual_pointer_manager_v1_interface,
			version < 2 ? version : 2);
	else if (!strcmp(iface, wl_seat_interface.name))
		seat = wl_registry_bind(reg, name, &wl_seat_interface,
			version < 5 ? version : 5);
}

static void
registry_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	registry_global, registry_global_remove,
};

static uint32_t
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int
main(int argc, char *argv[])
{
	struct wl_display *dpy;
	struct wl_registry *reg;
	struct zwlr_virtual_pointer_v1 *vp;
	int x, y, clicks = 1, interval = 30, i;

	if (argc < 3) {
		fprintf(stderr, "usage: %s X Y [CLICKS] [INTERVAL_MS]\n", argv[0]);
		return 1;
	}
	x = atoi(argv[1]);
	y = atoi(argv[2]);
	if (argc > 3)
		clicks = atoi(argv[3]);
	if (argc > 4)
		interval = atoi(argv[4]);
	if (getenv("POINTERCLICK_OUTPUT_W"))
		outw = atoi(getenv("POINTERCLICK_OUTPUT_W"));
	if (getenv("POINTERCLICK_OUTPUT_H"))
		outh = atoi(getenv("POINTERCLICK_OUTPUT_H"));

	if (!(dpy = wl_display_connect(NULL))) {
		fprintf(stderr, "pointerclick: cannot connect\n");
		return 1;
	}
	reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &registry_listener, NULL);
	wl_display_roundtrip(dpy);
	wl_display_roundtrip(dpy);
	if (!vpmgr) {
		fprintf(stderr, "pointerclick: no virtual pointer manager\n");
		return 2;
	}
	vp = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(vpmgr,
		seat);
	wl_display_roundtrip(dpy);

	zwlr_virtual_pointer_v1_motion_absolute(vp, now_ms(),
		wl_fixed_from_int(x), wl_fixed_from_int(y),
		wl_fixed_from_int(outw), wl_fixed_from_int(outh));
	zwlr_virtual_pointer_v1_frame(vp);
	wl_display_roundtrip(dpy);
	usleep(100 * 1000);

	for (i = 0; i < clicks; i++) {
		zwlr_virtual_pointer_v1_button(vp, now_ms(), BTN_LEFT,
			WL_POINTER_BUTTON_STATE_PRESSED);
		zwlr_virtual_pointer_v1_frame(vp);
		wl_display_roundtrip(dpy);
		usleep(25 * 1000);
		zwlr_virtual_pointer_v1_button(vp, now_ms(), BTN_LEFT,
			WL_POINTER_BUTTON_STATE_RELEASED);
		zwlr_virtual_pointer_v1_frame(vp);
		wl_display_roundtrip(dpy);
		if (i + 1 < clicks)
			usleep(interval * 1000);
	}
	return 0;
}
