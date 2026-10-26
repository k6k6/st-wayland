/*
 * Selection peer for tools/selection_sigpipe_test.sh.
 *
 * Maps a focusable toplevel (a compositor only sends selection events to
 * clients that hold keyboard focus), then asks the selection owner for the
 * primary selection:
 *
 *   --drop   request the data and close both ends of the pipe immediately,
 *            the way a clipboard manager that only sniffs a prefix does.
 *   --read   read the data to EOF and print it to stdout.
 *
 * Exit codes: 0 = action done, 2 = no selection offer within the timeout,
 * 1 = setup failure.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "primary-selection-unstable-v1-client-protocol.h"

#define PEER_TIMEOUT_MS 8000

static struct wl_compositor *comp;
static struct xdg_wm_base *wmbase;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct zwp_primary_selection_device_manager_v1 *pmgr;
static struct zwp_primary_selection_device_v1 *pdev;
static struct wl_surface *surf;
static struct wl_keyboard *kbd;
static int configured, focused, done, drop, dropped, datafd = -1;

static void
buffer_release(void *data, struct wl_buffer *b)
{
	wl_buffer_destroy(b);
}

static const struct wl_buffer_listener bufferlistener = { buffer_release };

/* attach a 2x2 opaque buffer so the toplevel actually maps */
static void
attach_blank(void)
{
	struct wl_shm_pool *pool;
	struct wl_buffer *buf;
	uint32_t *data;
	int fd, stride = 8, i;
	size_t size = stride * 2;
	char tmpl[] = "/tmp/selectionpeer-shm-XXXXXX";
	void *map;

	if ((fd = mkstemp(tmpl)) < 0)
		return;
	unlink(tmpl);
	if (ftruncate(fd, size) < 0)
		goto out;
	map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		goto out;
	data = map;
	for (i = 0; i < (int)(size / 4); i++)
		data[i] = 0xff202020;
	pool = wl_shm_create_pool(shm, fd, size);
	buf = wl_shm_pool_create_buffer(pool, 0, 2, 2, stride,
		WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	wl_buffer_add_listener(buf, &bufferlistener, NULL);
	wl_surface_attach(surf, buf, 0, 0);
	wl_surface_commit(surf);
out:
	close(fd);
}

static void
kbd_keymap(void *data, struct wl_keyboard *k, uint32_t format, int fd,
		uint32_t size)
{
	if (fd >= 0)
		close(fd);
}

static void
kbd_enter(void *data, struct wl_keyboard *k, uint32_t serial,
		struct wl_surface *s, struct wl_array *keys)
{
	focused = 1;
}

static void
kbd_leave(void *data, struct wl_keyboard *k, uint32_t serial,
		struct wl_surface *s)
{
	focused = 0;
}

static void
kbd_key(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t time,
		uint32_t key, uint32_t state)
{
}

static void
kbd_modifiers(void *data, struct wl_keyboard *k, uint32_t serial,
		uint32_t depressed, uint32_t latched, uint32_t locked,
		uint32_t group)
{
}

static void
kbd_repeat_info(void *data, struct wl_keyboard *k, int32_t rate,
		int32_t delay)
{
}

static const struct wl_keyboard_listener kbdlistener = {
	kbd_keymap, kbd_enter, kbd_leave, kbd_key, kbd_modifiers,
	kbd_repeat_info,
};

static void
seat_capabilities(void *data, struct wl_seat *s, uint32_t caps)
{
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !kbd) {
		kbd = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(kbd, &kbdlistener, NULL);
	}
}

static void
seat_name(void *data, struct wl_seat *s, const char *name)
{
}

static const struct wl_seat_listener seatlistener = {
	seat_capabilities, seat_name,
};

static void
wmbase_ping(void *data, struct xdg_wm_base *w, uint32_t serial)
{
	xdg_wm_base_pong(w, serial);
}

static const struct xdg_wm_base_listener wmbase_listener = { wmbase_ping };

static void
xdgsurface_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
	xdg_surface_ack_configure(xs, serial);
	configured = 1;
}

static const struct xdg_surface_listener xdgsurface_listener = {
	xdgsurface_configure,
};

static void
toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h,
		struct wl_array *states)
{
}

static void
toplevel_close(void *data, struct xdg_toplevel *t)
{
}

static void
toplevel_bounds(void *data, struct xdg_toplevel *t, int32_t w, int32_t h)
{
}

static void
toplevel_caps(void *data, struct xdg_toplevel *t, struct wl_array *caps)
{
}

static const struct xdg_toplevel_listener toplevel_listener = {
	toplevel_configure, toplevel_close, toplevel_bounds, toplevel_caps,
};

static void
offer_offer(void *data, struct zwp_primary_selection_offer_v1 *o,
		const char *mime)
{
}

static const struct zwp_primary_selection_offer_v1_listener offer_listener = {
	offer_offer,
};

static void
dev_data_offer(void *data, struct zwp_primary_selection_device_v1 *d,
		struct zwp_primary_selection_offer_v1 *o)
{
	if (o)
		zwp_primary_selection_offer_v1_add_listener(o, &offer_listener,
			NULL);
}

static void
dev_selection(void *data, struct zwp_primary_selection_device_v1 *d,
		struct zwp_primary_selection_offer_v1 *offer)
{
	int fds[2];

	if (!offer || done)
		return;
	done = 1;
	if (pipe(fds) < 0) {
		fprintf(stderr, "selectionpeer: pipe: %s\n", strerror(errno));
		exit(1);
	}
	zwp_primary_selection_offer_v1_receive(offer, "text/plain;charset=utf-8",
		fds[1]);
	/* the request may not be flushed yet: the owner has no reader */
	close(fds[1]);
	if (drop) {
		/* the read end is gone before the owner even gets the request;
		 * stay alive for a moment so that the request really is sent */
		close(fds[0]);
		fprintf(stderr, "selectionpeer: read end closed\n");
		dropped = 1;
		return;
	}
	datafd = fds[0];
	fcntl(datafd, F_SETFL, fcntl(datafd, F_GETFL) | O_NONBLOCK);
}

/* drain the selection data; returns 1 at EOF */
static int
readdata(void)
{
	char buf[4096];
	ssize_t n;

	for (;;) {
		n = read(datafd, buf, sizeof(buf));
		if (n > 0) {
			fwrite(buf, 1, n, stdout);
			continue;
		}
		if (n == 0)
			return 1;
		if (errno == EAGAIN || errno == EINTR)
			return 0;
		fprintf(stderr, "selectionpeer: read: %s\n", strerror(errno));
		return -1;
	}
}

static const struct zwp_primary_selection_device_v1_listener dev_listener = {
	dev_data_offer, dev_selection,
};

static void
registry_global(void *data, struct wl_registry *reg, uint32_t name,
		const char *iface, uint32_t version)
{
	if (!strcmp(iface, wl_compositor_interface.name)) {
		comp = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
	} else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		wmbase = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wmbase, &wmbase_listener, NULL);
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, wl_seat_interface.name)) {
		seat = wl_registry_bind(reg, name, &wl_seat_interface, 5);
		wl_seat_add_listener(seat, &seatlistener, NULL);
	} else if (!strcmp(iface,
			zwp_primary_selection_device_manager_v1_interface.name)) {
		pmgr = wl_registry_bind(reg, name,
			&zwp_primary_selection_device_manager_v1_interface, 1);
	}
}

static void
registry_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	registry_global, registry_global_remove,
};

/* dispatch events for at most timeout_ms, 1 when something was read */
static int
wait_events(struct wl_display *dpy, int timeout_ms)
{
	struct pollfd pfd;

	pfd.fd = wl_display_get_fd(dpy);
	pfd.events = POLLIN;
	pfd.revents = 0;
	while (wl_display_prepare_read(dpy) != 0) {
		if (wl_display_dispatch_pending(dpy) < 0)
			return -1;
	}
	wl_display_flush(dpy);
	if (poll(&pfd, 1, timeout_ms) <= 0) {
		wl_display_cancel_read(dpy);
		return 0;
	}
	if (wl_display_read_events(dpy) < 0)
		return -1;
	return wl_display_dispatch_pending(dpy) < 0 ? -1 : 1;
}

int
main(int argc, char *argv[])
{
	struct wl_display *dpy;
	struct wl_registry *reg;
	struct xdg_surface *xs;
	struct xdg_toplevel *tl;
	int waited = 0, r;

	drop = argc > 1 && !strcmp(argv[1], "--drop");
	if (argc > 1 && !drop && strcmp(argv[1], "--read")) {
		fprintf(stderr, "usage: %s --drop|--read\n", argv[0]);
		return 1;
	}

	if (!(dpy = wl_display_connect(NULL))) {
		fprintf(stderr, "selectionpeer: cannot connect\n");
		return 1;
	}
	reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &registry_listener, NULL);
	wl_display_roundtrip(dpy);
	wl_display_roundtrip(dpy);
	if (!comp || !wmbase || !shm || !seat || !pmgr) {
		fprintf(stderr, "selectionpeer: missing globals\n");
		return 1;
	}

	surf = wl_compositor_create_surface(comp);
	xs = xdg_wm_base_get_xdg_surface(wmbase, surf);
	xdg_surface_add_listener(xs, &xdgsurface_listener, NULL);
	tl = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(tl, &toplevel_listener, NULL);
	xdg_toplevel_set_title(tl, "selectionpeer");
	xdg_toplevel_set_app_id(tl, "selectionpeer");
	wl_surface_commit(surf);
	wl_display_roundtrip(dpy);
	if (configured)
		attach_blank();
	wl_display_flush(dpy);

	pdev = zwp_primary_selection_device_manager_v1_get_device(pmgr, seat);
	zwp_primary_selection_device_v1_add_listener(pdev, &dev_listener, NULL);

	/* wait for keyboard focus and then for a selection offer */
	while (!done && waited < PEER_TIMEOUT_MS) {
		r = wait_events(dpy, 100);
		if (r < 0) {
			fprintf(stderr, "selectionpeer: display error: %s\n",
				strerror(errno));
			return 1;
		}
		waited += 100;
	}
	if (!done) {
		fprintf(stderr, "selectionpeer: no selection offer%s\n",
			focused ? "" : " (never got focus)");
		return 2;
	}
	if (dropped) {
		/* give the compositor time to hand the request to the owner */
		for (waited = 0; waited < 1000; waited += 20) {
			if (wait_events(dpy, 20) < 0)
				break;
		}
		return 0;
	}

	/* get the data itself; the owner may write it in several chunks */
	while (waited < PEER_TIMEOUT_MS) {
		if ((r = readdata()) != 0)
			return r < 0 ? 1 : 0;
		if (wait_events(dpy, 20) < 0) {
			fprintf(stderr, "selectionpeer: display error: %s\n",
				strerror(errno));
			return 1;
		}
		waited += 20;
	}
	fprintf(stderr, "selectionpeer: selection transfer timed out\n");
	return 2;
}
