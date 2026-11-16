/* Kitty keyboard protocol encoder, following kitty/key_encoding.c.
 * Implements flags 1 (disambiguate), 2 (event types) and 8 (all keys as
 * escape codes); st.c owns the flag stacks. */
enum { KK_SHIFT = 1, KK_ALT = 2, KK_CTRL = 4, KK_SUPER = 8,
       KK_CAPS = 64, KK_NUM = 128, KK_LOCKS = KK_CAPS | KK_NUM };
enum { KK_PRESS = 1, KK_REPEAT, KK_RELEASE };

static const struct {
	xkb_keysym_t sym;
	uint num;
	char trailer;
} kittyfkeys[] = {
	{ XKB_KEY_Escape,               27, 'u' },
	{ XKB_KEY_Return,               13, 'u' },
	{ XKB_KEY_Tab,                   9, 'u' },
	{ XKB_KEY_ISO_Left_Tab,          9, 'u' },
	{ XKB_KEY_BackSpace,           127, 'u' },
	{ XKB_KEY_Insert,                2, '~' },
	{ XKB_KEY_Delete,                3, '~' },
	{ XKB_KEY_Left,                  1, 'D' },
	{ XKB_KEY_Right,                 1, 'C' },
	{ XKB_KEY_Up,                    1, 'A' },
	{ XKB_KEY_Down,                  1, 'B' },
	{ XKB_KEY_Page_Up,               5, '~' },
	{ XKB_KEY_Page_Down,             6, '~' },
	{ XKB_KEY_Home,                  1, 'H' },
	{ XKB_KEY_End,                   1, 'F' },
	{ XKB_KEY_Caps_Lock,         57358, 'u' },
	{ XKB_KEY_Scroll_Lock,       57359, 'u' },
	{ XKB_KEY_Num_Lock,          57360, 'u' },
	{ XKB_KEY_Print,             57361, 'u' },
	{ XKB_KEY_Pause,             57362, 'u' },
	{ XKB_KEY_Menu,              57363, 'u' },
	{ XKB_KEY_F1,                    1, 'P' },
	{ XKB_KEY_F2,                    1, 'Q' },
	{ XKB_KEY_F3,                   13, '~' },
	{ XKB_KEY_F4,                    1, 'S' },
	{ XKB_KEY_F5,                   15, '~' },
	{ XKB_KEY_F6,                   17, '~' },
	{ XKB_KEY_F7,                   18, '~' },
	{ XKB_KEY_F8,                   19, '~' },
	{ XKB_KEY_F9,                   20, '~' },
	{ XKB_KEY_F10,                  21, '~' },
	{ XKB_KEY_F11,                  23, '~' },
	{ XKB_KEY_F12,                  24, '~' },
	{ XKB_KEY_KP_Decimal,        57409, 'u' },
	{ XKB_KEY_KP_Divide,         57410, 'u' },
	{ XKB_KEY_KP_Multiply,       57411, 'u' },
	{ XKB_KEY_KP_Subtract,       57412, 'u' },
	{ XKB_KEY_KP_Add,            57413, 'u' },
	{ XKB_KEY_KP_Enter,          57414, 'u' },
	{ XKB_KEY_KP_Equal,          57415, 'u' },
	{ XKB_KEY_KP_Separator,      57416, 'u' },
	{ XKB_KEY_KP_Left,           57417, 'u' },
	{ XKB_KEY_KP_Right,          57418, 'u' },
	{ XKB_KEY_KP_Up,             57419, 'u' },
	{ XKB_KEY_KP_Down,           57420, 'u' },
	{ XKB_KEY_KP_Page_Up,        57421, 'u' },
	{ XKB_KEY_KP_Page_Down,      57422, 'u' },
	{ XKB_KEY_KP_Home,           57423, 'u' },
	{ XKB_KEY_KP_End,            57424, 'u' },
	{ XKB_KEY_KP_Insert,         57425, 'u' },
	{ XKB_KEY_KP_Delete,         57426, 'u' },
	{ XKB_KEY_KP_Begin,              1, 'E' },
	{ XKB_KEY_XF86AudioPlay,     57428, 'u' },
	{ XKB_KEY_XF86AudioPause,    57429, 'u' },
	{ XKB_KEY_XF86AudioStop,     57432, 'u' },
	{ XKB_KEY_XF86AudioForward,  57433, 'u' },
	{ XKB_KEY_XF86AudioRewind,   57434, 'u' },
	{ XKB_KEY_XF86AudioNext,     57435, 'u' },
	{ XKB_KEY_XF86AudioPrev,     57436, 'u' },
	{ XKB_KEY_XF86AudioRecord,   57437, 'u' },
	{ XKB_KEY_XF86AudioLowerVolume, 57438, 'u' },
	{ XKB_KEY_XF86AudioRaiseVolume, 57439, 'u' },
	{ XKB_KEY_XF86AudioMute,     57440, 'u' },
	{ XKB_KEY_Shift_L,           57441, 'u' },
	{ XKB_KEY_Control_L,         57442, 'u' },
	{ XKB_KEY_Alt_L,             57443, 'u' },
	{ XKB_KEY_Super_L,           57444, 'u' },
	{ XKB_KEY_Hyper_L,           57445, 'u' },
	{ XKB_KEY_Meta_L,            57446, 'u' },
	{ XKB_KEY_Shift_R,           57447, 'u' },
	{ XKB_KEY_Control_R,         57448, 'u' },
	{ XKB_KEY_Alt_R,             57449, 'u' },
	{ XKB_KEY_Super_R,           57450, 'u' },
	{ XKB_KEY_Hyper_R,           57451, 'u' },
	{ XKB_KEY_Meta_R,            57452, 'u' },
	{ XKB_KEY_ISO_Level3_Shift,  57453, 'u' },
	{ XKB_KEY_ISO_Level5_Shift,  57454, 'u' },
};

static int kittyevent = KK_PRESS;
/* keys whose press the application saw, so it also gets their release */
static xkb_keycode_t kittyheld[32];
static int kittynheld;

static int
kittyfkey(xkb_keysym_t sym, uint *num, char *trailer)
{
	int i;

	*trailer = 'u';
	if (BETWEEN(sym, XKB_KEY_F13, XKB_KEY_F35)) {
		*num = 57376 + sym - XKB_KEY_F13;
		return 1;
	}
	if (BETWEEN(sym, XKB_KEY_KP_0, XKB_KEY_KP_9)) {
		*num = 57399 + sym - XKB_KEY_KP_0;
		return 1;
	}
	for (i = 0; i < LEN(kittyfkeys); i++) {
		if (kittyfkeys[i].sym == sym) {
			*num = kittyfkeys[i].num;
			*trailer = kittyfkeys[i].trailer;
			return 1;
		}
	}
	return 0;
}

/* without flags 1 and 8 the keypad is not told apart from the main keys */
static xkb_keysym_t
kittykpnormal(xkb_keysym_t sym)
{
	if (BETWEEN(sym, XKB_KEY_KP_0, XKB_KEY_KP_9))
		return XKB_KEY_0 + sym - XKB_KEY_KP_0;
	switch (sym) {
	case XKB_KEY_KP_Decimal:   return XKB_KEY_period;
	case XKB_KEY_KP_Divide:    return XKB_KEY_slash;
	case XKB_KEY_KP_Multiply:  return XKB_KEY_asterisk;
	case XKB_KEY_KP_Subtract:  return XKB_KEY_minus;
	case XKB_KEY_KP_Add:       return XKB_KEY_plus;
	case XKB_KEY_KP_Enter:     return XKB_KEY_Return;
	case XKB_KEY_KP_Equal:     return XKB_KEY_equal;
	case XKB_KEY_KP_Separator: return XKB_KEY_comma;
	case XKB_KEY_KP_Left:      return XKB_KEY_Left;
	case XKB_KEY_KP_Right:     return XKB_KEY_Right;
	case XKB_KEY_KP_Up:        return XKB_KEY_Up;
	case XKB_KEY_KP_Down:      return XKB_KEY_Down;
	case XKB_KEY_KP_Page_Up:   return XKB_KEY_Page_Up;
	case XKB_KEY_KP_Page_Down: return XKB_KEY_Page_Down;
	case XKB_KEY_KP_Home:      return XKB_KEY_Home;
	case XKB_KEY_KP_End:       return XKB_KEY_End;
	case XKB_KEY_KP_Insert:    return XKB_KEY_Insert;
	case XKB_KEY_KP_Delete:    return XKB_KEY_Delete;
	}
	return sym;
}

static uint
kittymodbit(xkb_keysym_t sym)
{
	switch (sym) {
	case XKB_KEY_Shift_L: case XKB_KEY_Shift_R:     return KK_SHIFT;
	case XKB_KEY_Control_L: case XKB_KEY_Control_R: return KK_CTRL;
	case XKB_KEY_Alt_L: case XKB_KEY_Alt_R:         return KK_ALT;
	case XKB_KEY_Super_L: case XKB_KEY_Super_R:     return KK_SUPER;
	}
	return 0;
}

static void
kittysend(uint num, char trailer, uint mods, int event, int flags)
{
	char buf[48];
	int n, actions = (flags & 2) && event != KK_PRESS;

	n = snprintf(buf, sizeof buf, "\033[");
	if (num != 1 || mods || actions)
		n += snprintf(buf + n, sizeof buf - n, "%u", num);
	if (mods || actions)
		n += snprintf(buf + n, sizeof buf - n, ";%u", mods + 1);
	if (actions)
		n += snprintf(buf + n, sizeof buf - n, ":%d", event);
	buf[n++] = trailer;
	ttywrite(buf, n, 1);
}

static char
kittyctrled(char k)
{
	switch (k) {
	case ' ': case '2': case '@':  return 0;
	case '3': case '[':            return 27;
	case '4': case '\\':           return 28;
	case '5': case ']':            return 29;
	case '6': case '^': case '~':  return 30;
	case '7': case '/': case '_':  return 31;
	case '8': case '?':            return 127;
	}
	return BETWEEN(k, 'a', 'z') ? k - 'a' + 1 : k;
}

#define KITTYLEGACYKEY(k) (BETWEEN(k, 32, 126) && !BETWEEN(k, 'A', 'Z'))

/* legacy encoding of a modified ASCII key, used when only flag 2 is set */
static int
kittylegacy(uint key, uint shifted, uint mods)
{
	char out[2] = { '\033' }, k = key;
	uint m = mods;
	int esc = 0;

	if (!KITTYLEGACYKEY(key) && !(shifted && KITTYLEGACYKEY(shifted)))
		return 0;
	if ((m & KK_SHIFT) && shifted && shifted != key &&
	    (!(m & KK_CTRL) || !BETWEEN(key, 'a', 'z'))) {
		k = shifted;
		m &= ~KK_SHIFT;
	}
	if (mods == KK_SHIFT) {
		out[1] = k;
	} else if (m == KK_ALT || (k == ' ' && m == (KK_ALT | KK_SHIFT))) {
		esc = 1;
		out[1] = k;
	} else if (m == KK_CTRL || (k == ' ' && m == (KK_CTRL | KK_SHIFT))) {
		out[1] = kittyctrled(k);
	} else if (m == (KK_CTRL | KK_ALT)) {
		esc = 1;
		out[1] = kittyctrled(k);
	} else {
		return 0;
	}
	ttywrite(out + !esc, 1 + esc, 1);
	return 1;
}

/* Returns 0 when the protocol is off and the legacy tables apply. */
static int
kittykey(xkb_keycode_t keycode, xkb_keysym_t sym, uint state,
		const char *text, int len, int event)
{
	const xkb_keysym_t *syms;
	xkb_keysym_t base;
	char trailer, utf8[4];
	uint mods = 0, num, key, shifted;
	int flags = tkittykbdflags(), all = flags & 8, i;

	if (!flags)
		return 0;

	if (event != KK_RELEASE) {
		for (i = 0; i < kittynheld && kittyheld[i] != keycode; i++)
			;
		if (i == kittynheld && kittynheld < LEN(kittyheld))
			kittyheld[kittynheld++] = keycode;
	} else if (!(flags & 2)) {
		return 1;
	}

	if (state & ShiftMask)   mods |= KK_SHIFT;
	if (state & Mod1Mask)    mods |= KK_ALT;
	if (state & ControlMask) mods |= KK_CTRL;
	if (state & Mod4Mask)    mods |= KK_SUPER;
	if (state & LockMask)    mods |= KK_CAPS;
	if (state & Mod2Mask)    mods |= KK_NUM;

	if (kittyfkey(sym, &num, &trailer) &&
	    (num >= 57441 || BETWEEN(num, 57358, 57360))) {
		/* modifier keys exist only with flag 8; the compositor sends
		 * the new modifier state after the key, so apply it here */
		if (!all)
			return 1;
		if (event == KK_RELEASE)
			mods &= ~kittymodbit(sym);
		else
			mods |= kittymodbit(sym);
	}

	/* text is sent as is unless every key is to be an escape code */
	if (!all && event != KK_RELEASE && len > 0 && (uchar)text[0] >= 32 &&
	    text[0] != 127 && !(mods & (KK_ALT | KK_CTRL | KK_SUPER))) {
		ttywrite(text, len, 1);
		return 1;
	}

	base = sym;
	if (!(flags & 9))
		sym = kittykpnormal(sym);

	if (kittyfkey(sym, &num, &trailer)) {
		if (trailer == 'u' && !all && !(mods & ~KK_LOCKS) &&
		    (num == 13 || num == 9 || num == 127)) {
			/* stay usable at a shell prompt after an application crash */
			if (event != KK_RELEASE)
				ttywrite(num == 13 ? "\r" : num == 9 ? "\t" : "\177", 1, 1);
			return 1;
		}
		if (num == 27 && !mods && !(flags & 9)) {
			if (event != KK_RELEASE)
				ttywrite("\033", 1, 1);
			return 1;
		}
		kittysend(num, trailer, mods, event, flags);
		return 1;
	}

	/* a text key is named by the unshifted symbol of the current layout */
	if (base == sym && xkb_keymap_key_get_syms_by_level(xw.keymap, keycode,
			xkb_state_key_get_layout(xw.xkbstate, keycode), 0, &syms) > 0)
		base = syms[0];
	else
		base = sym;
	key = xkb_keysym_to_utf32(xkb_keysym_to_lower(base));
	shifted = xkb_keysym_to_utf32(sym);
	if (!key && !(key = xkb_keysym_to_utf32(xkb_keysym_to_lower(sym))))
		return 1;

	if (!((flags & 2) && event != KK_PRESS)) {
		if (!mods && !all) {
			ttywrite(utf8, utf8encode(key, utf8), 1);
			return 1;
		}
		if (mods && !(flags & 9) && kittylegacy(key, shifted, mods))
			return 1;
	}
	kittysend(key, 'u', mods, event, flags);
	return 1;
}

static void
kittyrelease(xkb_keycode_t keycode)
{
	int i;

	for (i = 0; i < kittynheld && kittyheld[i] != keycode; i++)
		;
	if (i == kittynheld)
		return;
	kittyheld[i] = kittyheld[--kittynheld];
	if (xw.xkbstate && !IS_SET(MODE_KBDLOCK))
		kittykey(keycode, xkb_state_key_get_one_sym(xw.xkbstate, keycode),
				modstate(), NULL, 0, KK_RELEASE);
}

static void
kittyreset(void)
{
	kittynheld = 0;
}
