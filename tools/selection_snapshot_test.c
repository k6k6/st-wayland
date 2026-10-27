/* Exercise the actual selection callbacks and send queue without a display.
 * Real nonblocking pipes provide short writes/EAGAIN; only protocol destruction
 * and one deterministic EINTR are substituted. --wrap=free observes teardown. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <wayland-client.h>
#include "primary-selection-unstable-v1-client-protocol.h"
static void test_clip_destroy(struct wl_data_source *);
static void test_primary_destroy(struct zwp_primary_selection_source_v1 *);
#define wl_data_source_destroy test_clip_destroy
#define zwp_primary_selection_source_v1_destroy test_primary_destroy
#define main st_program_main
#include "../wl.c"
#undef main
#undef wl_data_source_destroy
#undef zwp_primary_selection_source_v1_destroy

static int clip_destroyed, primary_destroyed, interrupted_fd = -1, interrupts;
static struct { void *ptr; int freed; } watched[128];
static size_t nwatched;

void __real_free(void *);
ssize_t __real_write(int, const void *, size_t);

void
__wrap_free(void *ptr)
{
	for (size_t i = nwatched; i > 0; i--)
		if (watched[i-1].ptr == ptr && !watched[i-1].freed) {
			watched[i-1].freed++;
			break;
		}
	__real_free(ptr);
}

ssize_t
__wrap_write(int fd, const void *buf, size_t len)
{
	if (fd == interrupted_fd) {
		interrupted_fd = -1;
		interrupts++;
		errno = EINTR;
		return -1;
	}
	return __real_write(fd, buf, len);
}

static void
test_clip_destroy(struct wl_data_source *source)
{
	assert(source == (void *)1);
	clip_destroyed++;
}

static void
test_primary_destroy(struct zwp_primary_selection_source_v1 *source)
{
	assert(source == (void *)2);
	primary_destroyed++;
}

static void
watch(SelectionSnapshot *s)
{
	assert(nwatched + 2 <= LEN(watched));
	watched[nwatched++].ptr = s;
	watched[nwatched++].ptr = s->bytes;
}

static void
clear_primary(void)
{
	selection_replace(&xsel.primary_snapshot, &xsel.primary, NULL);
}

static void
clear_clipboard(void)
{
	selection_replace(&xsel.clipboard_snapshot, &xsel.clipboard, NULL);
}

static void
assert_freed(void)
{
	for (size_t i = 0; i < nwatched; i++)
		assert(watched[i].freed == 1);
	for (int i = 0; i < NSENDJOBS; i++)
		assert(sendjobs[i].data == NULL && sendjobs[i].snapshot == NULL);
	memset(watched, 0, sizeof(watched));
	nwatched = 0;
}

static char *
payload(size_t len, int salt)
{
	char *s = xmalloc(len + 1);
	for (size_t i = 0; i < len; i++)
		s[i] = '!' + (i * 17 + salt) % 90;
	s[len] = '\0';
	return s;
}

static void
newpipe(int p[2])
{
	assert(pipe2(p, O_NONBLOCK | O_CLOEXEC) == 0);
	/* Linux permits an unprivileged caller to shrink its pipe to one page. */
	assert(fcntl(p[1], F_SETPIPE_SZ, 4096) >= 4096);
}

static SendJob *
job(int fd)
{
	for (int i = 0; i < NSENDJOBS; i++)
		if (sendjobs[i].data && sendjobs[i].fd == fd)
			return &sendjobs[i];
	return NULL;
}

static void
receive(int fd, const char *expected, size_t len)
{
	char buf[733];
	size_t got = 0;
	ssize_t n;
	for (size_t turns = 0;; turns++) {
		assert(turns < len * 2 + 100);
		n = read(fd, buf, sizeof(buf));
		if (n > 0) {
			assert(got + n <= len);
			assert(memcmp(expected + got, buf, n) == 0);
			got += n;
		} else if (n == 0) {
			assert(got == len);
			break;
		} else {
			assert(errno == EAGAIN);
		}
		sendqueue_flush(-1);
	}
	close(fd);
}

static void
sharing(void)
{
	char *owned = payload(100, 3);
	setsel(owned, 0);
	SelectionSnapshot *s = xsel.primary_snapshot;
	watch(s);
	assert(s->bytes == owned && xsel.primary == owned && s->len == 100);
	clipcopy(NULL);
	assert(xsel.clipboard_snapshot == s && xsel.clipboard == owned && s->refs == 2);
	/* Repeated copy and a separately allocated equal selection share bytes. */
	clipcopy(NULL);
	setsel(xstrdup(owned), 0);
	assert(xsel.primary_snapshot == s && xsel.clipboard_snapshot == s && s->refs == 2);
	setsel(NULL, 0); /* Preserve the existing NULL getsel() no-op. */
	assert(xsel.primary_snapshot == s);
	clear_primary();
	assert(xsel.primary == NULL && xsel.clipboard == owned && s->refs == 1);
	clear_clipboard();
	assert_freed();
	puts("PASS: ownership transfer, equal-content sharing, independent clipboard, exact frees");
}

static void
independent_clipboard(void)
{
#if !CLIPBOARD_PATCH
	setsel(payload(70, 1), 0);
	clipcopy(NULL);
	SelectionSnapshot *clipboard = xsel.clipboard_snapshot;
	watch(clipboard);
	setsel(payload(80, 2), 0);
	watch(xsel.primary_snapshot);
	assert(xsel.primary_snapshot != clipboard);
	assert(xsel.clipboard_snapshot == clipboard && clipboard->refs == 1);
	/* Equal to clipboard while different from primary: share its old bytes. */
	setsel(xstrdup(clipboard->bytes), 0);
	assert(xsel.primary_snapshot == clipboard && clipboard->refs == 2);
	assert(xsel.primary == xsel.clipboard);
	clear_primary();
	clear_clipboard();
	assert_freed();
	puts("PASS: independent clipboard survives replacement and is reused on equality");
#endif
}

static void
concurrent(void)
{
	const size_t len = 131071;
	char *expected = payload(len, 7);
	setsel(xstrdup(expected), 0);
	clipcopy(NULL);
	SelectionSnapshot *old = xsel.primary_snapshot;
	watch(old);
	int pipes[NSENDJOBS][2], extra[2];
	for (int i = 0; i < NSENDJOBS; i++) {
		newpipe(pipes[i]);
		if (i % 2)
			datasource_send(NULL, NULL, "text/plain", pipes[i][1]);
		else
			primsource_send(NULL, NULL, "text/plain", pipes[i][1]);
		SendJob *j = job(pipes[i][1]);
		assert(j && j->snapshot == old && j->data == old->bytes);
		assert(j->off > 0 && j->off < j->len); /* real short write */
		size_t off = j->off;
		sendqueue_flush(j->fd); /* full pipe: real EAGAIN */
		assert(j->off == off);
	}
	assert(old->refs == 2 + NSENDJOBS);
	newpipe(extra);
	sendjob_add(extra[1], old);
	char byte;
	assert(read(extra[0], &byte, 1) == 0); /* ninth requester is closed */
	assert(old->refs == 2 + NSENDJOBS);
	close(extra[0]);

	/* A transient interruption must leave the job and its reference intact. */
	interrupted_fd = pipes[0][1];
	size_t off = job(interrupted_fd)->off;
	sendqueue_flush(interrupted_fd);
	assert(interrupts == 1 && job(pipes[0][1])->off == off);
	assert(old->refs == 2 + NSENDJOBS);

	setsel(payload(9000, 11), 0);
	SelectionSnapshot *replacement = xsel.primary_snapshot;
	watch(replacement);
	assert(replacement != old);
#if !CLIPBOARD_PATCH
	assert(xsel.clipboard_snapshot == old); /* manual clipboard is independent */
	clipcopy(NULL);
#endif
	assert(xsel.clipboard_snapshot == replacement && old->refs == NSENDJOBS);
	xsel.clipsource = (void *)1;
	xsel.primsource = (void *)2;
	datasource_cancelled(NULL, xsel.clipsource);
	primsource_cancelled(NULL, xsel.primsource);
	assert(!xsel.clipsource && !xsel.primsource);
	assert(clip_destroyed == 1 && primary_destroyed == 1);
	assert(xsel.primary_snapshot == replacement && xsel.clipboard_snapshot == replacement);
	clear_primary();
	clipcopy(NULL); /* clearing clipboard from an empty primary */
	assert(!xsel.clipboard && !xsel.clipboard_snapshot);
	assert(old->refs == NSENDJOBS);
	for (int i = 0; i < NSENDJOBS; i++) {
		receive(pipes[i][0], expected, len);
		if (i + 1 < NSENDJOBS)
			assert(old->refs == NSENDJOBS - i - 1);
	}
	free(expected);
	assert_freed();
	puts("PASS: 8 shared pipe transfers, short writes/EAGAIN/EINTR, queue limit, replace/clear/cancel");
}

static void
multiple_generations(void)
{
	int pipes[NSENDJOBS][2];
	SelectionSnapshot *snapshots[2];
	char *expected[2];
	const size_t lengths[2] = { 49157, 57347 };
	for (int generation = 0; generation < 2; generation++) {
		expected[generation] = payload(lengths[generation], 20 + generation);
		setsel(xstrdup(expected[generation]), 0);
		clipcopy(NULL);
		snapshots[generation] = xsel.primary_snapshot;
		watch(snapshots[generation]);
		for (int i = generation; i < NSENDJOBS; i += 2) {
			newpipe(pipes[i]);
			if (i % 4 < 2)
				primsource_send(NULL, NULL, "text/plain", pipes[i][1]);
			else
				datasource_send(NULL, NULL, "text/plain", pipes[i][1]);
			assert(job(pipes[i][1])->data == snapshots[generation]->bytes);
		}
	}
	clear_primary();
	clear_clipboard();
	assert(snapshots[0]->refs == 4 && snapshots[1]->refs == 4);
	for (int i = 0; i < NSENDJOBS; i++) {
		int generation = i % 2;
		receive(pipes[i][0], expected[generation], lengths[generation]);
		if (i < NSENDJOBS - 2)
			assert(snapshots[generation]->refs == 3 - i / 2);
	}
	free(expected[0]);
	free(expected[1]);
	assert_freed();
	puts("PASS: 8 concurrent readers across two generations receive their own complete bytes");
}

static void
no_history_cache(void)
{
	int p[2];
	setsel(payload(65537, 15), 0);
	clipcopy(NULL);
	SelectionSnapshot *old = xsel.primary_snapshot;
	watch(old);
	char *expected = xstrdup(old->bytes);
	newpipe(p);
	primsource_send(NULL, NULL, "text/plain", p[1]);
	clear_primary();
	clear_clipboard();
	assert(old->refs == 1);
	setsel(xstrdup(expected), 0);
	SelectionSnapshot *fresh = xsel.primary_snapshot;
	watch(fresh);
	assert(fresh != old && fresh->bytes != old->bytes);
	clear_primary();
	clear_clipboard();
	receive(p[0], expected, 65537);
	free(expected);
	assert_freed();
	puts("PASS: pending transfers retain old bytes without historical interning");
}

static void
broken_pipe_and_empty(void)
{
	int p[2];
	struct sigaction action = { .sa_handler = SIG_DFL };
	sigemptyset(&action.sa_mask);
	assert(sigaction(SIGPIPE, &action, NULL) == 0);
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGPIPE);
	assert(sigprocmask(SIG_UNBLOCK, &mask, NULL) == 0);
	setsel(payload(65537, 17), 0);
	clipcopy(NULL);
	SelectionSnapshot *s = xsel.primary_snapshot;
	watch(s);
	newpipe(p);
	datasource_send(NULL, NULL, "text/plain", p[1]);
	assert(s->refs == 3 && job(p[1]));
	close(p[0]);
	sendqueue_flush(p[1]);
	assert(!job(p[1]) && s->refs == 2);
	assert(sigpending(&mask) == 0 && !sigismember(&mask, SIGPIPE));
	newpipe(p);
	close(p[0]);
	primsource_send(NULL, NULL, "text/plain", p[1]); /* immediate EPIPE */
	assert(!job(p[1]) && s->refs == 2);
	clear_primary();
	clear_clipboard();
	assert_freed();

	setsel(xstrdup(""), 0);
	clipcopy(NULL);
	s = xsel.primary_snapshot;
	watch(s);
	newpipe(p);
	primsource_send(NULL, NULL, "text/plain", p[1]);
	char byte;
	assert(read(p[0], &byte, 1) == 0 && s->refs == 2);
	close(p[0]);
	clear_primary();
	clear_clipboard();
	newpipe(p);
	datasource_send(NULL, NULL, "text/plain", p[1]);
	assert(read(p[0], &byte, 1) == 0);
	close(p[0]);
	assert_freed();
	puts("PASS: default SIGPIPE survives immediate/deferred EPIPE; empty/absent selections close cleanly");
}

int
main(void)
{
	alarm(20);
	for (int i = 0; i < NSENDJOBS; i++) sendjobs[i].fd = -1;
	sharing();
	independent_clipboard();
	concurrent();
	multiple_generations();
	no_history_cache();
	broken_pipe_and_empty();
	puts("all selection snapshot tests passed");
	return 0;
}
