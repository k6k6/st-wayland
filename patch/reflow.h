#define TLINE(y) ( \
	(y) < term.scr ? thistline((term.histi + (y) - term.scr + 1 + HISTSIZE) % HISTSIZE) \
	               : term.line[(y) - term.scr] \
)

#define TLINEABS(y) ( \
	(y) < 0 ? thistline((term.histi + (y) + 1 + HISTSIZE) % HISTSIZE) : term.line[(y)] \
)

/* A read-only row may end with one cell representing a repeated tail. Views
 * remain valid until that row is changed or the event-loop compaction runs. */
typedef struct {
	Line line;
	int stored;
} LineView;
static LineView tlineview(int);
static const Glyph *tviewcell(LineView, int);
static int tviewlen(LineView);

/* term.dirty is indexed by rows of the view, which term.scr moves over the screen */
#define TDIRTLINE(y) do { \
	if ((y) + term.scr < term.row) \
		term.dirty[(y) + term.scr] = 1; \
} while (0)

#define UPDATEWRAPNEXT(alt, col) do { \
	if ((term.c.state & CURSOR_WRAPNEXT) && term.c.x + term.wrapcwidth[alt] < col) { \
		term.c.x += term.wrapcwidth[alt]; \
		term.c.state &= ~CURSOR_WRAPNEXT; \
	} \
} while (0);

static int tiswrapped(Line line);
static size_t tgetline(char *, const Glyph *);
static inline int regionselected(int, int, int, int);
static void tloaddefscreen(int, int);
static void tloadaltscreen(int, int);
static void selmove(int);
static inline void tclearglyph(Glyph *, int);
static void treflow(int, int);
static void rscrolldown(int);
static void tresizedef(int, int);
static void tresizealt(int, int);
void tfreehist(void);
void kscrolldown(const Arg *);
void kscrollup(const Arg *);
static void tscrolldirt(int, int, int);
static void tscrollup(int, int, int, int);
static void tclearregion(int, int, int, int, int);
static void tdeletechar(int);
static int tlinelen(Line len);
static char * tgetglyphs(char *buf, const Glyph *gp, const Glyph *lgp);
static void selscroll(int, int, int);

typedef struct {
	 uint b;
	 uint mask;
	 void (*func)(const Arg *);
	 const Arg arg;
} MouseKey;

extern MouseKey mkeys[];
