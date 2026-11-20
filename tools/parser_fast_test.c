/* Real parser differential test and CPU benchmark; no compositor required.
 * Compare the generic dispatcher, optimized parser, and optionally a frozen
 * st.c provided through PARSER_TEST_BASELINE in the shell driver. */
#define _GNU_SOURCE
#include <assert.h>
#include <locale.h>
#ifdef PARSER_BASELINE
#include "../st.reference.c"
#else
#include "../st.c"
#endif

static int
reference_write(const char *buf, int buflen, int show_ctrl)
{
	int charsize, n;
	Rune u;
	#if SYNC_PATCH
	int su0 = su;
	twrite_aborted = 0;
	#endif
	for (n = 0; n < buflen; n += charsize) {
		#if SIXEL_PATCH
		if (IS_SET(MODE_SIXEL) && sixel_st.state != PS_ESC) {
			charsize = sixel_parser_parse(&sixel_st, (const unsigned char *)buf + n, buflen - n);
			continue;
		}
		#endif
		if (IS_SET(MODE_UTF8)) {
			charsize = utf8decode(buf + n, &u, buflen - n);
			if (!charsize) break;
		} else { u = (uchar)buf[n]; charsize = 1; }
		#if SYNC_PATCH
		if (su0 && !su) { twrite_aborted = 1; break; }
		#endif
		if (show_ctrl && ISCONTROL(u)) {
			if (u & 0x80) { u &= 0x7f; tputc('^'); tputc('['); }
			else if (u != '\n' && u != '\r' && u != '\t') { u ^= 0x40; tputc('^'); }
		}
		tputc(u);
	}
	return n;
}

#ifdef PARSER_REFERENCE
#define parse reference_write
#else
#define parse twrite
#endif

static uint32_t seed = 20261120;
static unsigned int rnd(unsigned int n)
{
	seed = seed * 1664525u + 1013904223u;
	return (seed >> 8) % n;
}
static uint64_t digest = 1469598103934665603ull;
static void hash(uint32_t n) { digest = (digest ^ n) * 1099511628211ull; }
static void glyph(Glyph g)
{
	hash(g.u); hash(g.mode); hash(g.fg); hash(g.bg);
	#if UNDERCURL_PATCH
	if (g.mode & ATTR_UNDERLINE) hash(g.ul);
	#endif
}

#ifndef PARSER_BASELINE
/* Compare actual cell changes with the advertised range. Account for recorded
 * pixel-row moves, so a scroll does not require redrawing unchanged survivors. */
static void dirtycoverage(void)
{
	static Glyph *previous;
	static int cols, rows;
	static Line *screen;
	int *source = xmalloc(term.row * sizeof(*source));
	if (previous && cols == term.col && rows == term.row && screen == term.line) {
		for (int y = 0; y < rows; y++) source[y] = y;
		#if REFLOW_PATCH
		for (int i = 0; i < term.nscrolls; i++) {
			RowScroll s = term.scrolls[i];
			if (s.n > 0) {
				for (int y = s.top; y <= s.bot; y++)
					source[y] = y + s.n <= s.bot ? source[y + s.n] : -1;
			} else {
				for (int y = s.bot; y >= s.top; y--)
					source[y] = y + s.n >= s.top ? source[y + s.n] : -1;
			}
		}
		#endif
		for (int y = 0; y < rows; y++) {
			#if REFLOW_PATCH || SCROLLBACK_PATCH
			Line line = TLINE(y);
			#else
			Line line = term.line[y];
			#endif
			if (term.dirty[y] == DIRTY_PARTIAL) {
				assert(term.dirtycols && term.dirtycols[y].left >= 0);
				assert(term.dirtycols[y].left < term.dirtycols[y].right);
				assert(term.dirtycols[y].right <= cols);
			}
			for (int x = 0; x < cols; x++) {
				if (source[y] >= 0 && !memcmp(&previous[source[y] * cols + x],
				                                  &line[x], sizeof(Glyph)))
					continue;
				if (term.dirty[y] == DIRTY_PARTIAL &&
				    !BETWEEN(x, term.dirtycols[y].left, term.dirtycols[y].right - 1)) {
					fprintf(stderr, "dirty range [%d,%d) misses changed cell %d,%d\n",
						term.dirtycols[y].left, term.dirtycols[y].right, x, y);
					abort();
				}
				if (!term.dirty[y]) {
					fprintf(stderr, "clean row misses changed cell %d,%d\n", x, y);
					abort();
				}
			}
		}
	}
	cols = term.col; rows = term.row; screen = term.line;
	previous = xrealloc(previous, (size_t)cols * rows * sizeof(*previous));
	for (int y = 0; y < rows; y++) {
		#if REFLOW_PATCH || SCROLLBACK_PATCH
		Line line = TLINE(y);
		#else
		Line line = term.line[y];
		#endif
		memcpy(&previous[y * cols], line, cols * sizeof(Glyph));
	}
	free(source);
}
#endif

static void state(int step)
{
	#ifndef PARSER_BASELINE
	dirtycoverage();
	#endif
	hash(term.col); hash(term.row); hash(term.c.x); hash(term.c.y);
	hash(term.c.state); hash(term.mode); hash(term.esc); hash(term.lastc);
	hash(term.top); hash(term.bot); hash(term.charset); hash(term.icharset);
	hash(sel.mode); hash(sel.type); hash(sel.snap); hash(sel.alt);
	hash(sel.nb.x); hash(sel.nb.y); hash(sel.ne.x); hash(sel.ne.y);
	hash(sel.ob.x); hash(sel.ob.y); hash(sel.oe.x); hash(sel.oe.y);
	hash(csiescseq.len); hash(csiescseq.priv); hash(csiescseq.narg);
	hash(csiescseq.mode[0]); hash(csiescseq.mode[1]);
	for (size_t i = 0; i < csiescseq.len; i++) hash((uchar)csiescseq.buf[i]);
	for (int i = 0; i < ESC_ARG_SIZ; i++) {
		hash(csiescseq.arg[i]);
		#if UNDERCURL_PATCH
		for (int j = 0; j < CAR_PER_ARG; j++) hash(csiescseq.carg[i][j]);
		#endif
	}
	glyph(term.c.attr);
	for (unsigned int i = 0; i < LEN(term.trantbl); i++) hash(term.trantbl[i]);
	for (int y = 0; y < term.row; y++) {
		/* Dirty ranges may conservatively add a row (e.g. an old wrap flag).
		 * Actual mutation coverage is checked separately above. */
		for (int x = 0; x < term.col; x++) glyph(term.line[y][x]);
		term.dirty[y] = 0;
	}
	#if REFLOW_PATCH
	hash(term.histf); hash(term.scr); hash(term.nscrolls);
	hash(term.wrapcwidth[0]); hash(term.wrapcwidth[1]);
	for (int y = -term.histf; y < 0; y++) {
		Line line = TLINEABS(y);
		int idx = (y + term.histi + HISTSIZE + 1) % HISTSIZE;
		hash(term.histcols[idx]);
		for (int x = 0; x < term.histcols[idx]; x++) glyph(line[x]);
	}
	thistcompact();
	#endif
	#if SIXEL_PATCH
	for (ImageList *im = term.images; im; im = im->next) {
		hash(im->x); hash(im->y); hash(im->width); hash(im->height);
		for (size_t i = 0; i < (size_t)im->width * im->height * 4; i++) hash(im->pixels[i]);
	}
	#endif
	char *text = getsel();
	if (text) {
		for (unsigned char *p = (void *)text; *p; p++) hash(*p);
		free(text);
	}
	printf("%d %016llx\n", step, (unsigned long long)digest);
	#if REFLOW_PATCH
	/* Match a completed frame between operations: scroll records and dirty
	 * ranges now describe only the changes since the saved cell snapshot. */
	term.nscrolls = 0;
	#endif
}

/* Exercise the same incomplete-UTF-8 buffering contract as ttyread(), with
 * randomized splits inside UTF-8, CSI, OSC and DCS sequences. */
static void feedraw(const char *data, size_t len, int show_ctrl)
{
	char pending[256];
	size_t off = 0, left = 0;
	while (off < len || left) {
		size_t chunk = 1 + rnd(31), n = MIN(len - off, chunk);
		assert(left + n <= sizeof pending);
		memcpy(pending + left, data + off, n); left += n; off += n;
		int used = parse(pending, left, show_ctrl);
		assert(used >= 0 && (size_t)used <= left);
		hash(used);
		#if SYNC_PATCH
		if (!used && twrite_aborted) continue;
		#endif
		assert(used || off < len); /* all test operations end in complete input */
		left -= used; memmove(pending, pending + used, left);
	}
}
static void feed(const char *text) { feedraw(text, strlen(text), 0); }

/* Parse extreme and malformed parameters without executing commands with
 * enormous coordinates. Keep the frozen parser's exact conversion behavior. */
static void csi_parameters(void)
{
	const char *cases[] = {
		"m", ";m", "?;25h", "38:2::1:2:3m", "4:3;58:2:1:2:3m",
		"0;0000000000;999999999;1000000000m", "2147483647;2147483648m",
		"4294967295;4294967296m", "9223372036854775807m",
		"9223372036854775808m", "-9223372036854775808m",
		"-9223372036854775809m", "+7;-0; 12;\t34m", "-;+;:m",
		"4::3:::7:;58:2::255:128:64m", "1:2;3:4m", "1;2:3;4m",
		"1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17m",
		"1234567890123456789012345678901234567890:1m", "?25$z",
	};
	const char chars[] = "0123456789;;::?+- \t<=>!$";
	for (int i = 0; i < 4000 + (int)LEN(cases); i++) {
		csireset();
		if (i < (int)LEN(cases)) {
			csiescseq.len = strlen(cases[i]);
			memcpy(csiescseq.buf, cases[i], csiescseq.len);
		} else {
			csiescseq.len = 1 + rnd(sizeof csiescseq.buf - 2);
			for (size_t j = 0; j + 1 < csiescseq.len; j++)
				csiescseq.buf[j] = chars[rnd(sizeof chars - 1)];
			csiescseq.buf[csiescseq.len - 1] = 'm';
		}
		csiparse();
		hash(csiescseq.priv); hash(csiescseq.narg);
		hash(csiescseq.mode[0]); hash(csiescseq.mode[1]);
		for (int j = 0; j < ESC_ARG_SIZ; j++) {
			hash(csiescseq.arg[j]);
			#if UNDERCURL_PATCH
			for (int k = 0; k < CAR_PER_ARG; k++) hash(csiescseq.carg[j][k]);
			#endif
		}
		printf("csi %d %016llx\n", i, (unsigned long long)digest);
	}
	csireset();
}

static void differential(void)
{
	const char *ops[] = {
		"ASCII -> != 0123456789 abcdefghijklmnopqrstuvwxyz\r\n",
		"\xe4\xb8\xad\xe6\x96\x87\xce\xbb e\xcc\x81\r\n",
		"\033[1;31mred\033[4:3;58:2:100:50:25munder\033[m\r\n",
		"\033[3;7H\xe4\xb8\xad\033[DZ\033[2;6H\xe4\xb8\xad\033[2DX",
		"\033[4hINSERT\033[4l", "\033[?7lOVERFLOW012345678901234567890\033[?7h",
		"\033(0lqqk\r\nx  x\r\nmqqj\033(B", "\033)0\016lqqk\017ASCII",
		"\033[2;5r\033[?6h\033[Horigin\r\n\033[?6l\033[r",
		"\033[2K\033[3P\033[2@\033[4X", "\033[S\033[T\033[L\033[M",
		"\033[?1049hALT\r\n\033[?1049l", "\033[31m\0337\033[Hsaved\0338restored\033[m",
		"\033]2;parser ASCII title\007", "\033]2;string with \xe4\xb8\xad\033\\",
		"\033[5iPRINT \xe4\xb8\xad\r\n\033[4i",
		"\033%@A\xa3\xb1Z\033%G\r\n", "\a\b\t\r\n\177",
		"\033[?2026hSYNCHRONIZED\033[?2026lNEXT",
		"\033]133;A\007prompt> \033]133;B\007command\r\n",
		"\033[38:2::10:20:30mRGB\033[48;2;40;50;60mBG\033[m",
		"\033[2;1H\xe4\xb8\xad\xe4\xb8\xad\033[2;2Habcd\033[2;1HABCDE",
		"\033[?7hABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
		"\033[?7lABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789\033[?7h",
		"\033[3b\033[0m", "\033[1\030cancel\033[2\032cancel",
		"bad utf8: \xc0\xaf \xed\xa0\x80 \xf4\x90\x80\x80 end",
		#if SIXEL_PATCH
		"\033P0;1q\"1;1;4;6#1;2;100;0;0!4~\033\\text",
		#endif
	};
	/* A one-column grid exercises the zero-length fast-run fallback. Keep
	 * this before wide text: reflowing wide history to one column has a
	 * separate pre-existing limitation outside this parser optimization. */
	tresize(1, 3);
	feed("ASCII wrap\r\n\033[?7lLAST CELL\033[?7h");
	state(-2);
	feed("\033c");
	tresize(41, 10);
	state(-1);
	for (size_t i = 0; i < LEN(ops); i++) { feed(ops[i]); state(i); }
	for (int i = 0; i < 4000; i++) {
		if (i % 29 == 0) {
			selstart(rnd(term.col), rnd(term.row), 0);
			selextend(rnd(term.col), rnd(term.row), SEL_REGULAR, 0);
		}
		if (i % 37 == 0) { selclear(); tresize(2 + rnd(39), 2 + rnd(10)); }
		#if REFLOW_PATCH
		if (i % 23 == 0) kscrollup(&(Arg){.i = 1 + rnd(7)});
		if (i % 31 == 0) kscrolldown(&(Arg){.i = 1 + rnd(7)});
		#endif
		if (i % 17 == 0) feedraw("A\001\033[31m\xc2\x9bZ\n", sizeof("A\001\033[31m\xc2\x9bZ\n") - 1, 1);
		else feed(ops[rnd(LEN(ops))]);
		state(100 + i);
	}
	off_t printed = lseek(iofd, 0, SEEK_END);
	hash(printed);
	char bytes[512];
	for (off_t off = 0; off < printed;) {
		ssize_t n = pread(iofd, bytes, sizeof bytes, off); assert(n > 0);
		for (ssize_t i = 0; i < n; i++) hash((uchar)bytes[i]);
		off += n;
	}
	printf("printer %016llx\n", (unsigned long long)digest);
	csi_parameters();
}

static void benchmark(void)
{
	const char *chunks[] = {
		"abcdefghijklmnopqrstuvwxyz0123456789 abcdefghijklmnopqrstuvwxyz0123456789\r\n",
		"\033[31ma\033[32mb\033[33mc\033[34md\033[35me\033[36mf\033[0m\r\n",
		"\xe4\xb8\xad\xe6\x96\x87\xce\xbb \xe4\xb8\xad\xe6\x96\x87\xce\xbb\r\n"
	};
	for (size_t scene = 0; scene < LEN(chunks); scene++) {
		char block[65536]; size_t size = 0, n = strlen(chunks[scene]);
		while (size + n <= sizeof block) { memcpy(block + size, chunks[scene], n); size += n; }
		feed("\033c\033[?1049h\033[?7l");
		for (int i = 0; i < 4; i++) assert(parse(block, size, 0) == (int)size);
		struct timespec a, b; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &a);
		for (int i = 0; i < 256; i++) assert(parse(block, size, 0) == (int)size);
		clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &b);
		printf("%zu bytes=%zu cpu_ms=%.3f\n", scene, size * 256,
			(b.tv_sec-a.tv_sec)*1000. + (b.tv_nsec-a.tv_nsec)/1e6);
	}
}

int main(int argc, char **argv)
{
	setlocale(LC_CTYPE, ""); xloadcols(); win.cw = 10; win.ch = 20;
	selinit(); tnew(41, 10);
	cmdfd = open("/dev/null", O_WRONLY); assert(cmdfd >= 0);
	FILE *printer = tmpfile(); assert(printer); iofd = fileno(printer);
	if (argc > 1 && !strcmp(argv[1], "bench")) { tresize(160, 45); benchmark(); }
	else differential();
	fclose(printer); close(cmdfd);
	return 0;
}
