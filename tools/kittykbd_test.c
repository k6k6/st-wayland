/* Headless regression of the kitty keyboard protocol encoder: real kpress()
 * and release path with a real xkb "us" keymap; tty output is captured. */
#define main st_program_main
#define ttywrite capture_ttywrite
#define tkittykbdflags test_kittykbdflags
#include "../wl.c"
#undef main
#include <assert.h>

static char out[256];
static int outlen;

void
capture_ttywrite(const char *s, size_t n, int may_echo)
{
	assert(outlen + n < sizeof out);
	memcpy(out + outlen, s, n);
	outlen += n;
	out[outlen] = '\0';
}

#if KITTY_KEYBOARD_PATCH
static int flags, fails;


int
test_kittykbdflags(void)
{
	return flags;
}

static void
setmods(const char *names)
{
	static const char *all[] = { "Shift", "Control", "Mod1", "Mod4" };
	xkb_mod_mask_t dep = 0, lock = 0;
	int i;

	for (i = 0; i < LEN(all); i++)
		if (strstr(names, all[i]))
			dep |= 1u << xkb_keymap_mod_get_index(xw.keymap, all[i]);
	if (strstr(names, "Num"))
		lock |= 1u << xkb_keymap_mod_get_index(xw.keymap, "Mod2");
	if (strstr(names, "Caps"))
		lock |= 1u << xkb_keymap_mod_get_index(xw.keymap, "Lock");
	xkb_state_update_mask(xw.xkbstate, dep, 0, lock, 0, 0, 0);
}

static void
show(const char *s)
{
	for (; *s; s++)
		if (*s == '\033')
			fputs("\\e", stderr);
		else if ((uchar)*s < 32 || *s == 127)
			fprintf(stderr, "\\x%02x", *s);
		else
			fputc(*s, stderr);
}

/* press, repeat and release evdev key `code` and compare what the tty got */
static void
chk(int f, const char *mods, int code, const char *press, const char *repeat,
		const char *release)
{
	const char *want[] = { press, repeat, release };
	int i;

	flags = f;
	setmods(mods);
	for (i = 0; i < 3; i++) {
		outlen = 0;
		out[0] = '\0';
		if (i == 0) {
			kpress(code + 8);
		} else if (i == 1) {
			kittyevent = KK_REPEAT;
			kpress(code + 8);
			kittyevent = KK_PRESS;
		} else {
			kittyrelease(code + 8);
		}
		if (strcmp(out, want[i])) {
			fprintf(stderr, "flags %d mods '%s' key %d event %d: got '",
					f, mods, code, i + 1);
			show(out);
			fputs("' want '", stderr);
			show(want[i]);
			fputs("'\n", stderr);
			fails++;
		}
	}
	assert(kittynheld == 0);
}

enum { ESC = 1, K1 = 2, BS = 14, TAB = 15, ENTER = 28, LCTRL = 29, A = 30,
       LSHIFT = 42, C = 46, LALT = 56, SPACE = 57, F1 = 59, F3 = 61, F5 = 63,
       KP1 = 79, KPENTER = 96, UP = 103, LEFT = 105, DEL = 111, LBRACKET = 26 };
#define E "\033["

int
main(void)
{
	struct xkb_rule_names names = { .layout = "us" };

	xw.xkbctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	xw.keymap = xkb_keymap_new_from_names(xw.xkbctx, &names, 0);
	assert(xw.keymap);
	xw.xkbstate = xkb_state_new(xw.keymap);

	/* flag 1: text stays text, ambiguous keys become CSI u */
	chk(1, "", A, "a", "a", "");
	chk(1, "Shift", A, "A", "A", "");
	chk(1, "Caps", A, "A", "A", "");
	chk(1, "Control", A, E"97;5u", E"97;5u", "");
	chk(1, "Control", C, E"99;5u", E"99;5u", "");
	chk(1, "Mod1", A, E"97;3u", E"97;3u", "");
	chk(1, "Shift Control Mod1", A, E"97;8u", E"97;8u", "");
	chk(1, "Mod4", A, E"97;9u", E"97;9u", "");
	chk(1, "Control", SPACE, E"32;5u", E"32;5u", "");
	chk(1, "Control", LBRACKET, E"91;5u", E"91;5u", "");
	chk(1, "Shift Control", K1, E"49;6u", E"49;6u", "");
	chk(1, "Control Num", A, E"97;133u", E"97;133u", "");
	chk(1, "", ESC, E"27u", E"27u", "");
	chk(1, "", ENTER, "\r", "\r", "");
	chk(1, "", TAB, "\t", "\t", "");
	chk(1, "", BS, "\177", "\177", "");
	chk(1, "Num", ENTER, "\r", "\r", "");
	chk(1, "Shift", ENTER, E"13;2u", E"13;2u", "");
	chk(1, "Shift", TAB, E"9;2u", E"9;2u", "");
	chk(1, "Control", BS, E"127;5u", E"127;5u", "");
	chk(1, "", UP, E"A", E"A", "");
	chk(1, "Control", LEFT, E"1;5D", E"1;5D", "");
	chk(1, "", F1, E"P", E"P", "");
	chk(1, "", F3, E"13~", E"13~", "");
	chk(1, "Shift", F5, E"15;2~", E"15;2~", "");
	chk(1, "", DEL, E"3~", E"3~", "");
	chk(1, "", KP1, E"57424u", E"57424u", "");
	chk(1, "Num", KP1, "1", "1", "");
	chk(1, "", KPENTER, E"57414u", E"57414u", "");
	chk(1, "", LSHIFT, "", "", "");

	/* flags 1|2: repeat and release events */
	chk(3, "", A, "a", "a", E"97;1:3u");
	chk(3, "Shift", A, "A", "A", E"97;2:3u");
	chk(3, "Control", A, E"97;5u", E"97;5:2u", E"97;5:3u");
	chk(3, "", ESC, E"27u", E"27;1:2u", E"27;1:3u");
	chk(3, "", ENTER, "\r", "\r", "");
	chk(3, "Shift", ENTER, E"13;2u", E"13;2:2u", E"13;2:3u");
	chk(3, "", UP, E"A", E"1;1:2A", E"1;1:3A");
	chk(3, "Control", UP, E"1;5A", E"1;5:2A", E"1;5:3A");
	chk(3, "", F5, E"15~", E"15;1:2~", E"15;1:3~");
	chk(3, "", LCTRL, "", "", "");

	/* flag 2 alone keeps the legacy encodings of the press */
	chk(2, "Control", A, "\001", E"97;5:2u", E"97;5:3u");
	chk(2, "Mod1", A, "\033a", E"97;3:2u", E"97;3:3u");
	chk(2, "Control Mod1", C, "\033\003", E"99;7:2u", E"99;7:3u");
	chk(2, "Control", SPACE, "", E"32;5:2u", E"32;5:3u");
	chk(2, "", ESC, "\033", "\033", "");
	chk(2, "", KP1, E"F", E"1;1:2F", E"1;1:3F");

	/* flag 8: every key is an escape code, modifiers included */
	chk(8, "", A, E"97u", E"97u", "");
	chk(8, "Shift", A, E"97;2u", E"97;2u", "");
	chk(8, "Caps", A, E"97;65u", E"97;65u", "");
	chk(8, "", ENTER, E"13u", E"13u", "");
	chk(8, "", TAB, E"9u", E"9u", "");
	chk(8, "", BS, E"127u", E"127u", "");
	chk(8, "Num", KP1, E"57400;129u", E"57400;129u", "");
	chk(8, "", LSHIFT, E"57441;2u", E"57441;2u", "");
	chk(8, "", LALT, E"57443;3u", E"57443;3u", "");

	/* flags 1|2|8 */
	chk(11, "", A, E"97u", E"97;1:2u", E"97;1:3u");
	chk(11, "", ENTER, E"13u", E"13;1:2u", E"13;1:3u");
	chk(11, "", LCTRL, E"57442;5u", E"57442;5:2u", E"57442;1:3u");
	chk(11, "Shift", LCTRL, E"57442;6u", E"57442;6:2u", E"57442;2:3u");

	/* protocol off: legacy tables, and no release for a key pressed so */
	chk(0, "", A, "a", "a", "");
	chk(0, "", UP, E"A", E"A", "");

	/* a press seen under the protocol is released under it, once */
	flags = 3;
	setmods("");
	kpress(A + 8);
	kpress(C + 8);
	assert(kittynheld == 2);
	outlen = 0;
	kittyrelease(A + 8);
	kittyrelease(A + 8);
	assert(!strcmp(out, E"97;1:3u"));
	kittyreset(); /* focus loss drops the rest */
	outlen = 0, out[0] = '\0';
	kittyrelease(C + 8);
	assert(outlen == 0);

	if (fails)
		return 1;
	puts("ok");
	return 0;
}
#else
int
main(void)
{
	puts("ok (disabled)");
	return 0;
}
#endif // KITTY_KEYBOARD_PATCH
