/*
 * Copyright (C) 2008  Anthony Liguori <anthony@codemonkey.ws>
 * Copyright (C) 2009-2010 Daniel P. Berrange <dan@berrange.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>
#include "vncdisplaykeymap.h"
#include "vncutil.h"

/*
 * This table is taken from QEMU x_keymap.c, under the terms:
 *
 * Copyright (c) 2003 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/* keycode translation for sending ISO_Left_Send
 * to vncserver
 */
static struct {
	GdkKeymapKey *keys;
	gint n_keys;
	guint keyval;
} untranslated_keys[] = {{NULL, 0, GDK_KEY_Tab}};

static unsigned int ref_count_for_untranslated_keys = 0;

#if defined(GDK_WINDOWING_X11)
#include <gdk/gdkx.h>
#include <X11/XKBlib.h>
#include <stdbool.h>
#include <string.h>

/* Xorg Linux + kbd (offset + mangled XT keycodes) */
#include "vncdisplaykeymap_xorgkbd2rfb.c"
/* Xorg Linux + evdev (offset evdev keycodes) */
#include "vncdisplaykeymap_xorgevdev2rfb.c"
/* Xorg OS-X aka XQuartz (offset OS-X keycodes) */
#include "vncdisplaykeymap_xorgxquartz2rfb.c"
/* Xorg Cygwin aka XWin (offset + mangled XT keycodes) */
#include "vncdisplaykeymap_xorgxwin2rfb.c"

#define STRPREFIX(a,b) (strncmp((a),(b),strlen((b))) == 0)

static gboolean check_for_xwin(void)
{
	char *vendor = ServerVendor(GDK_DISPLAY_XDISPLAY(gdk_display_get_default()));

	if (strstr(vendor, "Cygwin/X"))
		return TRUE;

	return FALSE;
}

static gboolean check_for_xquartz(void)
{
	int nextensions;
	int i;
	gboolean match = FALSE;
	char **extensions = XListExtensions(GDK_DISPLAY_XDISPLAY(gdk_display_get_default()), &nextensions);
	for (i = 0 ; extensions != NULL && i < nextensions ; i++) {
		if (strcmp(extensions[i], "Apple-WM") == 0 ||
		    strcmp(extensions[i], "Apple-DRI") == 0)
			match = TRUE;
	}
	if (extensions)
		XFreeExtensionList(extensions);

	return match;
}

const guint16 const *vnc_display_keymap_gdk2rfb_table(size_t *maplen)
{
	XkbDescPtr desc;
	const gchar *keycodes = NULL;

	/* There is no easy way to determine what X11 server
	 * and platform & keyboard driver is in use. Thus we
	 * do best guess heuristics.
	 *
	 * This will need more work for people with other
	 * X servers..... patches welcomed.
	 */

	desc = XkbGetKeyboard(GDK_DISPLAY_XDISPLAY(gdk_display_get_default()), XkbGBN_AllComponentsMask,
			      XkbUseCoreKbd);
	if (desc) {
		if (desc->names) {
			keycodes = gdk_x11_get_xatom_name(desc->names->keycodes);
			if (!keycodes)
				g_warning("could not lookup keycode name");
		}
		XkbFreeClientMap(desc, XkbGBN_AllComponentsMask, True);
	}

	if (check_for_xwin()) {
		VNC_DEBUG("Using xwin keycode mapping");
		*maplen = G_N_ELEMENTS(keymap_xorgxwin2rfb);
		return keymap_xorgxwin2rfb;
	} else if (check_for_xquartz()) {
		VNC_DEBUG("Using xquartz keycode mapping");
		*maplen = G_N_ELEMENTS(keymap_xorgxquartz2rfb);
		return keymap_xorgxquartz2rfb;
	} else if (keycodes && STRPREFIX(keycodes, "evdev_")) {
		VNC_DEBUG("Using evdev keycode mapping");
		*maplen = G_N_ELEMENTS(keymap_xorgevdev2rfb);
		return keymap_xorgevdev2rfb;
	} else if (keycodes && STRPREFIX(keycodes, "xfree86_")) {
		VNC_DEBUG("Using xfree86 keycode mapping");
		*maplen = G_N_ELEMENTS(keymap_xorgkbd2rfb);
		return keymap_xorgkbd2rfb;
	} else {
               g_warning("Unknown keycode mapping '%s'.\n"
			 "Please report to gtk-vnc-list@gnome.org\n"
                         "including the following information:\n"
                         "\n"
                         "  - Operating system\n"
                         "  - GTK build\n"
                         "  - X11 Server\n"
                         "  - xprop -root\n"
                         "  - xdpyinfo\n",
			 keycodes);
               return NULL;
	}
}

#elif defined(GDK_WINDOWING_WIN32)
/* Win32 native virtual keycodes */
#include "vncdisplaykeymap_win322rfb.c"

const guint16 const *vnc_display_keymap_gdk2rfb_table(size_t *maplen)
{
	VNC_DEBUG("Using Win32 virtual keycode mapping");
	*maplen = sizeof(keymap_win322rfb);
	return keymap_win322rfb;
}

#elif defined(GDK_WINDOWING_QUARTZ)
/* OS-X native keycodes */
#include "vncdisplaykeymap_osx2rfb.c"

const guint16 const *vnc_display_keymap_gdk2rfb_table(size_t *maplen)
{
	VNC_DEBUG("Using OS-X virtual keycode mapping");
	*maplen = sizeof(keymap_osx2rfb);
	return keymap_osx2rfb;
}


#else

const guint16 const *vnc_display_keymap_gdk2rfb_table(size_t *maplen)
{
	g_warning("Unsupported GDK Windowing platform.\n"
		  "Please report to gtk-vnc-list@gnome.org\n"
		  "including the following information:\n"
		  "\n"
		  "  - Operating system\n"
		  "  - GTK Windowing system build\n");
	return NULL;
}
#endif

guint16 vnc_display_keymap_gdk2rfb(const guint16 const *keycode_map,
				   size_t keycode_maplen,
				   guint16 keycode)
{
	if (!keycode_map)
		return 0;
	if (keycode >= keycode_maplen)
		return 0;
	return keycode_map[keycode];
}

/* Set the keymap entries */
void vnc_display_keyval_set_entries(void)
{
	size_t i;
	if (ref_count_for_untranslated_keys == 0)
		for (i = 0; i < sizeof(untranslated_keys) / sizeof(untranslated_keys[0]); i++)
			gdk_keymap_get_entries_for_keyval(gdk_keymap_get_default(),
							  untranslated_keys[i].keyval,
							  &untranslated_keys[i].keys,
							  &untranslated_keys[i].n_keys);
	ref_count_for_untranslated_keys++;
}

/* Free the keymap entries */
void vnc_display_keyval_free_entries(void)
{
	size_t i;

	if (ref_count_for_untranslated_keys == 0)
		return;

	ref_count_for_untranslated_keys--;
	if (ref_count_for_untranslated_keys == 0)
		for (i = 0; i < sizeof(untranslated_keys) / sizeof(untranslated_keys[0]); i++)
			g_free(untranslated_keys[i].keys);

}

/* Get the keyval from the keycode without the level. */
guint vnc_display_keyval_from_keycode(guint keycode, guint keyval)
{
	size_t i;
	for (i = 0; i < sizeof(untranslated_keys) / sizeof(untranslated_keys[0]); i++) {
		if (keycode == untranslated_keys[i].keys[0].keycode) {
			return untranslated_keys[i].keyval;
		}
	}

	return keyval;
}
/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
