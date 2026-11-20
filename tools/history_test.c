/* Feed the real parser, history, selection and reflow without a compositor.
 * Linked with the Wayland backend for its normal configuration and helpers. */
#define _GNU_SOURCE
#include <locale.h>
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#ifdef HISTORY_ALLOCATIONS
static void *history_malloc(size_t);
static void *history_realloc(void *, size_t);
static void history_free(void *);
#define malloc history_malloc
#define realloc history_realloc
#define free history_free
#endif
#include "../st.c"
#ifdef HISTORY_ALLOCATIONS
#undef malloc
#undef realloc
#undef free

/* Track parser-owned allocations without changing production code. The same
 * instrumentation runs against the frozen reference; CPU timings use a
 * separate build without these wrappers. */
static struct {
	void *ptr;
	size_t bytes;
} allocations[32768];
static size_t allocated, allocation_peak;

static size_t allocationslot(void *ptr, int insert)
{
	size_t slot = ((uintptr_t)ptr >> 4) * 2654435761u % LEN(allocations);
	size_t vacant = LEN(allocations);
	for (size_t i = 0; i < LEN(allocations); i++) {
		void *found = allocations[slot].ptr;
		if (found == ptr)
			return slot;
		if (found == (void *)(uintptr_t)1 && vacant == LEN(allocations))
			vacant = slot;
		if (!found)
			return insert ? (vacant < LEN(allocations) ? vacant : slot) : LEN(allocations);
		slot = (slot + 1) % LEN(allocations);
	}
	if (!insert)
		return LEN(allocations);
	assert(vacant < LEN(allocations));
	return vacant;
}

static void allocationremove(void *ptr)
{
	if (!ptr) return;
	size_t slot = allocationslot(ptr, 0);
	if (slot == LEN(allocations)) return; /* e.g. an external strdup */
	allocated -= allocations[slot].bytes;
	allocations[slot].ptr = (void *)(uintptr_t)1;
}

static void allocationadd(void *ptr, size_t bytes)
{
	if (!ptr) return;
	size_t slot = allocationslot(ptr, 1);
	allocations[slot].ptr = ptr;
	allocations[slot].bytes = bytes;
	allocated += bytes;
	allocation_peak = MAX(allocation_peak, allocated);
}

static void *history_malloc(size_t bytes)
{
	void *ptr = malloc(bytes);
	allocationadd(ptr, bytes);
	return ptr;
}

static void *history_realloc(void *ptr, size_t bytes)
{
	allocationremove(ptr);
	void *next = realloc(ptr, bytes);
	assert(next || !bytes);
	allocationadd(next, bytes);
	return next;
}

static void history_free(void *ptr)
{
	allocationremove(ptr);
	free(ptr);
}
#define malloc history_malloc
#define realloc history_realloc
#define free history_free
#endif

static unsigned int seed = 20261119;
static unsigned int rnd(unsigned int n)
{
	seed = seed * 1664525u + 1013904223u;
	return (seed >> 10) % n;
}

static void feed(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	assert(n > 0 && n < (int)sizeof buf);
	for (int off = 0; off < n;) {
		int used = twrite(buf + off, n - off, 0);
		assert(used > 0); off += used;
	}
}

static void compact(void)
{
#ifdef HISTORY_COMPACT
	thistcompact();
#endif
}

static uint64_t digest;
static void hash(uint32_t n)
{
	digest ^= n; digest *= 1099511628211ull;
}

static void state(int step)
{
	digest = 1469598103934665603ull;
	hash(term.col); hash(term.row); hash(term.histf); hash(term.scr);
	hash(term.c.x); hash(term.c.y); hash(term.c.state); hash(term.mode);
	for (int y = tisaltscr() ? 0 : -term.histf; y < term.row; y++) {
		Line line = y < 0 ? TLINEABS(y) : term.line[y];
		for (int x = 0; x < term.col; x++) {
			Glyph g = line[x];
			hash(g.u); hash(g.mode); hash(g.fg); hash(g.bg);
			#if UNDERCURL_PATCH
			/* Only meaningful underline bits: untouched clear cells may have
			 * uninitialized ul in the original representation. */
			if (g.mode & ATTR_UNDERLINE) hash(g.ul);
			#endif
		}
	}
	#if SIXEL_PATCH
	for (ImageList *im = term.images; im; im = im->next) {
		hash(im->x); hash(im->y); hash(im->cols); hash(im->width); hash(im->height);
		for (int i = 0; i < im->width * im->height * 4; i++) hash(im->pixels[i]);
	}
	#endif
	char *text = getsel();
	if (text) {
		for (unsigned char *p = (unsigned char *)text; *p; p++) hash(*p);
		free(text);
	}
	printf("%d %016llx\n", step, (unsigned long long)digest);
	compact();
}

#ifdef HISTORY_COMPACT
static void codec(void)
{
	const int widths[] = {1, 2, 41, 209};
	for (unsigned int w = 0; w < LEN(widths); w++) {
		int col = widths[w];
		Line expected = xmalloc(col * sizeof(Glyph));
		for (int run = 0; run < 30; run++) {
			Glyph last = {.u = run % 2 ? ' ' : 0x4e2d,
				.mode = run % 3 ? ATTR_SET | ATTR_UNDERLINE : ATTR_WRAP,
				.fg = 0x1001122, .bg = 0x1034455};
			#if SIXEL_PATCH
			if (!(run % 3)) last.mode |= ATTR_SIXEL;
			#endif
			#if UNDERCURL_PATCH
			last.ul = 0x24123456;
			#endif
			for (int x = 0; x < col; x++) expected[x] = last;
			int prefix = run % col;
			for (int x = 0; x < prefix; x++) expected[x].u = 'a' + x % 26;
			thiststore(0, expected, col);
			assert(!memcmp(thistline(0), expected, col * sizeof(Glyph)));
			/* Mutations on an expanded line must survive its next compaction. */
			term.hist[0][col-1].bg ^= 0x12345;
			expected[col-1].bg ^= 0x12345;
			compact();
			assert(!memcmp(thistline(0), expected, col * sizeof(Glyph)));
			compact();
		}
		free(expected);
	}
	tfreehist();
}
#endif

static void keyboard(void)
{
#if KEYBOARDSELECT_PATCH
	win.mode ^= kbds_keyboardhandler(-1, NULL, 0, 0);
	for (int i = 0; i < 5 * term.row; i++) {
		kbds_keyboardhandler(XKB_KEY_k, NULL, 0, 0);
		if (i == 2 * term.row) kbds_oc = kbds_c;
		Line current = kbds_c.line, previous = kbds_oc.line;
		Glyph a = current[kbds_c.x], b = previous[kbds_oc.x];
		compact();
		assert(!memcmp(&a, &current[kbds_c.x], sizeof a));
		assert(!memcmp(&b, &previous[kbds_oc.x], sizeof b));
	}
	kbds_keyboardhandler(XKB_KEY_slash, NULL, 0, 0);
	for (char *p = "history"; *p; p++) kbds_keyboardhandler(0, p, 1, 0);
	kbds_keyboardhandler(XKB_KEY_Return, NULL, 0, 0);
	compact(); state(-20);
	kbds_keyboardhandler(XKB_KEY_n, NULL, 0, 0);
	compact(); state(-21);
	win.mode ^= kbds_keyboardhandler(XKB_KEY_Escape, NULL, 0, 1);
	compact(); state(-22);
#endif
}

static void images(void)
{
#if SIXEL_PATCH
	feed("\033[r\033[2J\033[2;2H\033P0;1q\"1;1;40;48#1;2;100;0;0!40~-!40C-!40~-!40C-!40~-!40C-!40~-!40C\033\\");
	for (int i = 0; i < 18; i++) feed("line %d\r\n", i);
	kscrollup(&(Arg){.i = 12});
	tresize(41, 18); state(-10);
	tresize(209, 12); state(-11);
	kscrolldown(&(Arg){.i = HISTSIZE});
	feed("\033[?1049h"); tresize(83, 20);
	feed("\033[?1049l"); state(-12);
#endif
}

static size_t historybytes(void)
{
	size_t bytes = 0;
	for (int i = 0; i < HISTSIZE; i++)
		bytes += abs(term.histlen[i]) * sizeof(Glyph);
	return bytes;
}

/* Read operations must preserve the packed allocation and the actual cells.
 * Semantic digests are also compared with a frozen, unmodified implementation
 * when HISTORY_TEST_REFERENCE is supplied to the driver. */
static void readscans(void)
{
	feed("\033[?1049l\033[r\033[2J\033[H\033[3J");
	tresize(209, 12);
	for (int i = 0; i < HISTSIZE + 20; i++)
		feed("%04d: history \xe4\xb8\xad beta\r\n", i);
	compact();
	for (int type = SEL_REGULAR; type <= SEL_RECTANGULAR; type++) {
		selstart(type == SEL_REGULAR ? 0 : 7, -term.histf, 0);
		selextend(type == SEL_REGULAR ? term.col-1 : 22, term.row-1, type, 0);
		compact();
		size_t before = historybytes();
		char *text = getsel();
		assert(text && strlen(text) > 10000);
		digest = 1469598103934665603ull;
		for (unsigned char *p = (unsigned char *)text; *p; p++) hash(*p);
		printf("copy %d %zu %016llx\n", type, strlen(text), (unsigned long long)digest);
		free(text);
		#ifdef HISTORY_READVIEW
		assert(historybytes() == before);
		#endif
		fprintf(stderr, "copy type=%d history-bytes=%zu -> %zu\n", type, before, historybytes());
		compact(); selclear();
	}
	#if KEYBOARDSELECT_PATCH
	win.mode ^= kbds_keyboardhandler(-1, NULL, 0, 0);
	static const char *queries[] = {"absent_XYZ", "history", "\xe4\xb8\xad", "absent_XYZ"};
	for (size_t q = 0; q < LEN(queries); q++) {
		compact();
		size_t before = historybytes();
		kbds_keyboardhandler(XKB_KEY_slash, NULL, 0, 0);
		#ifdef HISTORY_READVIEW
		assert(historybytes() == before); /* clearing highlights stays packed */
		#endif
		kbds_pasteintosearch(queries[q], strlen(queries[q]), 0);
		kbds_keyboardhandler(XKB_KEY_Return, NULL, 0, 0);
		#ifdef HISTORY_READVIEW
		/* Only the destination cursor/visible rows may require expansion. */
		assert(historybytes() <= before + (term.row + 2) * term.col * sizeof(Glyph));
		#endif
		fprintf(stderr, "search query=%zu history-bytes=%zu -> %zu\n", q, before, historybytes());
		printf("search %zu pos=%d,%d scroll=%d\n", q, kbds_c.x, kbds_c.y, term.scr);
		compact(); state(4100 + q);
		for (int dir = -1; dir <= 1; dir += 2) {
			kbds_quant = 3;
			kbds_searchnext(dir);
			printf("next %zu dir=%d pos=%d,%d scroll=%d\n", q, dir, kbds_c.x, kbds_c.y, term.scr);
			compact(); state(4120 + 2 * q + (dir > 0));
		}
	}
	win.mode ^= kbds_keyboardhandler(XKB_KEY_Escape, NULL, 0, 1);
	compact(); state(4140);

	/* Repeated meaningful cells can occupy a logical full-width tail.
	 * Highlighting only part of it must split the tail, never mark all copies. */
	feed("\033[r\033[2J\033[H\033[3J");
	tresize(17, 5);
	for (int i = 0; i < 25; i++) {
		feed("xxxxxxxxxxxxxxxxx\r\n");
		feed("                 \r\n");
		feed("abcdefghijklmno\xe4\xb8\xad" "pqr\xe4\xb8\xad" "STuv\r\n");
		feed("abcdefghijklmnopqxxxxxxxxxxxxxxxxx\r\n");
	}
	compact();
	static const char *edgequeries[] = {"qxxx", "xxx", "   ", "\xe4\xb8\xad" "pqr", "o\xe4\xb8\xad", "STu", "uV"};
	win.mode ^= kbds_keyboardhandler(-1, NULL, 0, 0);
	for (size_t q = 0; q < LEN(edgequeries); q++) {
		kbds_keyboardhandler(XKB_KEY_slash, NULL, 0, 0);
		kbds_pasteintosearch(edgequeries[q], strlen(edgequeries[q]), 0);
		kbds_keyboardhandler(XKB_KEY_Return, NULL, 0, 0);
		printf("edge %zu pos=%d,%d scroll=%d\n", q, kbds_c.x, kbds_c.y, term.scr);
		compact(); state(4160 + q);
		kbds_quant = 2; kbds_searchnext(-1);
		printf("edge-back %zu pos=%d,%d scroll=%d\n", q, kbds_c.x, kbds_c.y, term.scr);
		compact(); state(4180 + q);
	}
	win.mode ^= kbds_keyboardhandler(XKB_KEY_Escape, NULL, 0, 1);
	compact(); state(4200);
	/* Alternate-screen line ending uses spaces, not ATTR_SET. */
	feed("\033[?1049h\033[2J\033[Hxxx xxx  \r\n\xe4\xb8\xad" "pqr STuv");
	win.mode ^= kbds_keyboardhandler(-1, NULL, 0, 0);
	kbds_keyboardhandler(XKB_KEY_slash, NULL, 0, 0);
	kbds_pasteintosearch("xxx", 3, 0);
	kbds_keyboardhandler(XKB_KEY_Return, NULL, 0, 0);
	state(4220);
	win.mode ^= kbds_keyboardhandler(XKB_KEY_Escape, NULL, 0, 1);
	feed("\033[?1049l");
	#endif
}

static void reflowmemory(void)
{
	feed("\033[?1049l\033[r\033[2J\033[H\033[3J");
	tresize(160, 45);
	for (int i = 0; i < HISTSIZE + 50; i++)
		feed("%04d short \xe4\xb8\xad log\r\n", i);
	compact();
	const int widths[] = {80, 320, 41, 209, 160};
	for (size_t i = 0; i < LEN(widths); i++) {
		#ifdef HISTORY_ALLOCATIONS
		size_t before = allocated;
		allocation_peak = allocated;
		#endif
		tresize(widths[i], 45);
		#ifdef HISTORY_ALLOCATIONS
		fprintf(stderr, "reflow col=%d live=%zu peak=%zu extra=%zu\n",
		        widths[i], allocated, allocation_peak, allocation_peak - before);
		#ifdef HISTORY_REFLOW_COMPACT
		/* A full history of short rows must no longer coexist as two
		 * expanded grids during width changes. */
		assert(allocation_peak - before < HISTSIZE * 80 * sizeof(Glyph) / 2);
		#endif
		#endif
		state(4300 + i);
	}
	/* Fill the ring with wrapped lines, meaningful repeated tails and wide
	 * boundaries, then discard more than HISTSIZE rows during narrow reflow. */
	feed("\033[r\033[2J\033[H\033[3J");
	tresize(41, 9);
	for (int i = 0; i < HISTSIZE + 15; i++) {
		feed("%04d xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", i);
		feed("\xe4\xb8\xad\xe4\xb8\xad qqqqqqqqqqqqqqqqqqqqqqqqqq\r\n");
	}
	compact();
	kscrollup(&(Arg){.i = 17});
	const int narrow[] = {2, 3, 83, 209, 41};
	for (size_t i = 0; i < LEN(narrow); i++) {
		tresize(narrow[i], 4 + i);
		state(4320 + i);
	}
}

int main(void)
{
	setlocale(LC_CTYPE, "");
	xloadcols(); win.cw = 10; win.ch = 20;
	selinit(); tnew(209, 12);
	cmdfd = open("/dev/null", O_WRONLY);
	assert(cmdfd >= 0);
#ifdef HISTORY_COMPACT
	codec();
#endif
	for (int i = 0; i < HISTSIZE + 70; i++)
		feed("\033[38;2;%d;80;110m%04d: history -> \xe4\xb8\xad\033[m\r\n", i % 256, i);
	state(-1);
	keyboard();
	images();
	feed("\033c"); /* finish the image phase before the text-only fuzz */
	const int widths[] = {2, 3, 41, 83, 209};
	for (int step = 0; step < 3500; step++) {
		switch (rnd(16)) {
		case 0: case 1: case 2: case 3:
			feed("\033[3%d;4%dm%04d \xe4\xb8\xad -> %c\033[m\r\n", rnd(8), rnd(8), step, 'a' + rnd(26)); break;
		case 4: feed("\033[%d;%dH", 1 + rnd(term.row), 1 + rnd(term.col)); break;
		case 5: feed("\033[%d%c", 1 + rnd(4), "STLM"[rnd(4)]); break;
		case 6: feed("\033[%d;%dr", 1 + rnd(term.row), 1 + rnd(term.row)); break;
		case 7: feed(rnd(2) ? "\033M" : "\033[r"); break;
		case 8: kscrollup(&(Arg){.i = 1 + rnd(70)}); break;
		case 9: kscrolldown(&(Arg){.i = 1 + rnd(70)}); break;
		case 10: feed(rnd(2) ? "\033[K" : "\033[2J"); break;
		case 11:
			feed(tisaltscr() ? "\033[?1049l" : "\033[?1049h"); break;
		case 12: selclear(); tresize(widths[rnd(LEN(widths))], 2 + rnd(24)); break;
		case 13:
			selstart(rnd(term.col), rnd(term.row), 0);
			selextend(rnd(term.col), rnd(term.row), SEL_REGULAR, 0); break;
		case 14:
			if (!(step % 127)) feed("\033[3J");
			else selclear(); break;
		case 15:
			#if UNDERCURL_PATCH
			feed("\033[4:3;58:2:100:50:25m  \033[m");
			#else
			feed("\033[4m  \033[m");
			#endif
			break;
		}
		compact();
		if (!(step % 13)) state(step);
	}
	feed("\033[?1049l\033[r\033[3J"); tresize(209, 12);
	for (int i = 0; i < HISTSIZE + 20; i++) feed("%04d: short log entry\r\n", i);
	state(4000);
	size_t bytes = 0, usable = 0;
	for (int i = 0; i < HISTSIZE; i++) if (term.hist[i]) {
#ifdef HISTORY_COMPACT
		bytes += abs(term.histlen[i]) * sizeof(Glyph);
#else
		bytes += term.col * sizeof(Glyph);
#endif
		usable += malloc_usable_size(term.hist[i]);
	}
	fprintf(stderr, "history=%d cells=%zu requested=%zu usable=%zu\n", term.histf, sizeof(Glyph), bytes, usable);
	/* Reading all history must not permanently expand it. */
	state(4001);
#ifdef HISTORY_COMPACT
	size_t after = 0;
	for (int i = 0; i < HISTSIZE; i++) after += abs(term.histlen[i]) * sizeof(Glyph);
	assert(after == bytes);
#endif
	readscans();
	reflowmemory();
	puts("PASS: history cells, wraps, styles, SIXEL, selection, scrolling, resize and reflow");
	return 0;
}
