/*
 * Copyright (C) 2008  Anthony Liguori <anthony@codemonkey.ws>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <gdk/gdkkeysyms.h>
#include "x_keymap.h"
#include "utils.h"
#include "vnc_keycodes.h"

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

static const guint8 x_keycode_to_pc_keycode_table[61] = {
	0xc7,      /*  97  Home   */
	0xc8,      /*  98  Up     */
	0xc9,      /*  99  PgUp   */
	0xcb,      /* 100  Left   */
	0x4c,      /* 101  KP-5   */
	0xcd,      /* 102  Right  */
	0xcf,      /* 103  End    */
	0xd0,      /* 104  Down   */
	0xd1,      /* 105  PgDn   */
	0xd2,      /* 106  Ins    */
	0xd3,      /* 107  Del    */
	0x9c,      /* 108  Enter  */
	0x9d,      /* 109  Ctrl-R */
	0,         /* 110  Pause  */
	0xb7,      /* 111  Print  */
	0xb5,      /* 112  Divide */
	0xb8,      /* 113  Alt-R  */
	0xc6,      /* 114  Break  */
	0,         /* 115 */
	0,         /* 116 */
	0,         /* 117 */
	0,         /* 118 */
	0,         /* 119 */
	0,         /* 120 */
	0,         /* 121 */
	0,         /* 122 */
	0,         /* 123 */
	0,         /* 124 */
	0,         /* 125 */
	0,         /* 126 */
	0,         /* 127 */
	0,         /* 128 */
	0x79,      /* 129 Henkan */
	0,         /* 130 */
	0x7b,      /* 131 Muhenkan */
	0,         /* 132 */
	0x7d,      /* 133 Yen */
	0,         /* 134 */
	0,         /* 135 */
	0x47,      /* 136 KP_7 */
	0x48,      /* 137 KP_8 */
	0x49,      /* 138 KP_9 */
	0x4b,      /* 139 KP_4 */
	0x4c,      /* 140 KP_5 */
	0x4d,      /* 141 KP_6 */
	0x4f,      /* 142 KP_1 */
	0x50,      /* 143 KP_2 */
	0x51,      /* 144 KP_3 */
	0x52,      /* 145 KP_0 */
	0x53,      /* 146 KP_. */
	0x47,      /* 147 KP_HOME */
	0x48,      /* 148 KP_UP */
	0x49,      /* 149 KP_PgUp */
	0x4b,      /* 150 KP_Left */
	0x4c,      /* 151 KP_ */
	0x4d,      /* 152 KP_Right */
	0x4f,      /* 153 KP_End */
	0x50,      /* 154 KP_Down */
	0x51,      /* 155 KP_PgDn */
	0x52,      /* 156 KP_Ins */
	0x53,      /* 157 KP_Del */
};

/* This table is generated based off the xfree86 -> scancode mapping above
 * and the keycode mappings in /usr/share/X11/xkb/keycodes/evdev
 * and  /usr/share/X11/xkb/keycodes/xfree86
 */

static const guint8 evdev_keycode_to_pc_keycode[61] = {
	0,         /*  97 EVDEV - RO   ("Internet" Keyboards) */
	0,         /*  98 EVDEV - KATA (Katakana) */
	0,         /*  99 EVDEV - HIRA (Hiragana) */
	0x79,      /* 100 EVDEV - HENK (Henkan) */
	0x70,      /* 101 EVDEV - HKTG (Hiragana/Katakana toggle) */
	0x7b,      /* 102 EVDEV - MUHE (Muhenkan) */
	0,         /* 103 EVDEV - JPCM (KPJPComma) */
	0x9c,      /* 104 KPEN */
	0x9d,      /* 105 RCTL */
	0xb5,      /* 106 KPDV */
	0xb7,      /* 107 PRSC */
	0xb8,      /* 108 RALT */
	0,         /* 109 EVDEV - LNFD ("Internet" Keyboards) */
	0xc7,      /* 110 HOME */
	0xc8,      /* 111 UP */
	0xc9,      /* 112 PGUP */
	0xcb,      /* 113 LEFT */
	0xcd,      /* 114 RGHT */
	0xcf,      /* 115 END */
	0xd0,      /* 116 DOWN */
	0xd1,      /* 117 PGDN */
	0xd2,      /* 118 INS */
	0xd3,      /* 119 DELE */
	0,         /* 120 EVDEV - I120 ("Internet" Keyboards) */
	0,         /* 121 EVDEV - MUTE */
	0,         /* 122 EVDEV - VOL- */
	0,         /* 123 EVDEV - VOL+ */
	0,         /* 124 EVDEV - POWR */
	0,         /* 125 EVDEV - KPEQ */
	0,         /* 126 EVDEV - I126 ("Internet" Keyboards) */
	0,         /* 127 EVDEV - PAUS */
	0,         /* 128 EVDEV - ???? */
	0,         /* 129 EVDEV - I129 ("Internet" Keyboards) */
	0xf1,      /* 130 EVDEV - HNGL (Korean Hangul Latin toggle) */
	0xf2,      /* 131 EVDEV - HJCV (Korean Hangul Hanja toggle) */
	0x7d,      /* 132 AE13 (Yen)*/
	0xdb,      /* 133 EVDEV - LWIN */
	0xdc,      /* 134 EVDEV - RWIN */
	0xdd,      /* 135 EVDEV - MENU */
	0,         /* 136 EVDEV - STOP */
	0,         /* 137 EVDEV - AGAI */
	0,         /* 138 EVDEV - PROP */
	0,         /* 139 EVDEV - UNDO */
	0,         /* 140 EVDEV - FRNT */
	0,         /* 141 EVDEV - COPY */
	0,         /* 142 EVDEV - OPEN */
	0,         /* 143 EVDEV - PAST */
	0,         /* 144 EVDEV - FIND */
	0,         /* 145 EVDEV - CUT  */
	0,         /* 146 EVDEV - HELP */
	0,         /* 147 EVDEV - I147 */
	0,         /* 148 EVDEV - I148 */
	0,         /* 149 EVDEV - I149 */
	0,         /* 150 EVDEV - I150 */
	0,         /* 151 EVDEV - I151 */
	0,         /* 152 EVDEV - I152 */
	0,         /* 153 EVDEV - I153 */
	0,         /* 154 EVDEV - I154 */
	0,         /* 155 EVDEV - I156 */
	0,         /* 156 EVDEV - I157 */
	0,         /* 157 EVDEV - I158 */
};


/* keycode translation for sending ISO_Left_Send
 * to vncserver
 */
static struct {
	GdkKeymapKey *keys;
	gint n_keys;
	guint keyval;
} untranslated_keys[] = {{NULL, 0, GDK_Tab}};

static unsigned int ref_count_for_untranslated_keys = 0;

/* As best as I can tell, windows passes PC scan codes.  This code definitely
 * won't compile on windows so let's put in a guard anyway */

#ifndef WIN32
#include <gdk/gdkx.h>
#include <X11/XKBlib.h>
#include <stdbool.h>
#include <string.h>

#define STRPREFIX(a,b) (strncmp((a),(b),strlen((b))) == 0)

static gboolean check_for_evdev(void)
{
	XkbDescPtr desc;
	gboolean has_evdev = FALSE;
	const gchar *keycodes;

	desc = XkbGetKeyboard(GDK_DISPLAY(), XkbGBN_AllComponentsMask,
			      XkbUseCoreKbd);
	if (desc == NULL || desc->names == NULL)
		return FALSE;

	keycodes = gdk_x11_get_xatom_name(desc->names->keycodes);
	if (keycodes == NULL)
		g_warning("could not lookup keycode name\n");
	else if (STRPREFIX(keycodes, "evdev_"))
		has_evdev = TRUE;
	else if (!STRPREFIX(keycodes, "xfree86_"))
		g_warning("unknown keycodes `%s', please report to gtk-vnc-devel\n",
			  keycodes);

	XkbFreeClientMap(desc, XkbGBN_AllComponentsMask, True);

	return has_evdev;
}
#else
static gboolean check_for_evdev(void)
{
	return FALSE;
}
#endif

const guint8 const *x_keycode_to_pc_keycode_map(void)
{
	if (check_for_evdev()) {
		GVNC_DEBUG("Using evdev keycode mapping");
		return evdev_keycode_to_pc_keycode;
	} else {
		GVNC_DEBUG("Using xfree86 keycode mapping");
		return x_keycode_to_pc_keycode_table;
	}
}

guint16 x_keycode_to_pc_keycode(const guint8 const *keycode_map,
				guint16 keycode)
{
	if (keycode == GDK_Pause)
		return VKC_PAUSE;

	if (keycode < 9)
		return 0;

	if (keycode < 97)
		return keycode - 8; /* just an offset */

	if (keycode < 158)
		return keycode_map[keycode - 97];

	if (keycode == 208) /* Hiragana_Katakana */
		return 0x70;

	if (keycode == 211) /* backslash */
		return 0x73;

	return 0;
}

/* Set the keymap entries */
void x_keymap_set_keymap_entries()
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
void x_keymap_free_keymap_entries()
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
guint x_keymap_get_keyval_from_keycode(guint keycode, guint keyval)
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
