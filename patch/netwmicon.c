/*
 * Window icon on Wayland.
 *
 * There is no PNG decoding here any more: the compositor (and taskbars such
 * as waybar/noctalia) resolve the icon from the desktop entry that matches
 * our app_id ("st" -> st.desktop -> Icon=...).  This drops libgd and the
 * ~25 image/video codec libraries it pulled in.
 */
void
setnetwmicon(void)
{
	/* nothing to do: icon comes from the desktop entry via app_id */
}
