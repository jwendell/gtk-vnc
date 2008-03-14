/*
 * QEMU SDL display driver
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

/*
 * Adapted for gtk-vnc from QEMU x_keymap.c revision 1.3 (on 20080113)
 *
 * Copyright (C) 2008  Anthony Liguori <anthony@codemonkey.ws>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "x_keymap.h"
#include <gdk/gdkkeysyms.h>

static const uint8_t x_keycode_to_pc_keycode_table[115] = {
   0xc7,      /*  97  Home   */
   0xc8,      /*  98  Up     */
   0xc9,      /*  99  PgUp   */
   0xcb,      /* 100  Left   */
   0x4c,        /* 101  KP-5   */
   0xcd,      /* 102  Right  */
   0xcf,      /* 103  End    */
   0xd0,      /* 104  Down   */
   0xd1,      /* 105  PgDn   */
   0xd2,      /* 106  Ins    */
   0xd3,      /* 107  Del    */
   0x9c,      /* 108  Enter  */
   0x9d,      /* 109  Ctrl-R */
   0x0,       /* 110  Pause  */
   0xb7,      /* 111  Print  */
   0xb5,      /* 112  Divide */
   0xb8,      /* 113  Alt-R  */
   0xc6,      /* 114  Break  */
   0x0,         /* 115 */
   0x0,         /* 116 */
   0x0,         /* 117 */
   0x0,         /* 118 */
   0x0,         /* 119 */
   0x0,         /* 120 */
   0x0,         /* 121 */
   0x0,         /* 122 */
   0x0,         /* 123 */
   0x0,         /* 124 */
   0x0,         /* 125 */
   0x0,         /* 126 */
   0x0,         /* 127 */
   0x0,         /* 128 */
   0x79,         /* 129 Henkan */
   0x0,         /* 130 */
   0x7b,         /* 131 Muhenkan */
   0x0,         /* 132 */
   0x7d,         /* 133 Yen */
   0x0,         /* 134 */
   0x0,         /* 135 */
   0x47,         /* 136 KP_7 */
   0x48,         /* 137 KP_8 */
   0x49,         /* 138 KP_9 */
   0x4b,         /* 139 KP_4 */
   0x4c,         /* 140 KP_5 */
   0x4d,         /* 141 KP_6 */
   0x4f,         /* 142 KP_1 */
   0x50,         /* 143 KP_2 */
   0x51,         /* 144 KP_3 */
   0x52,         /* 145 KP_0 */
   0x53,         /* 146 KP_. */
   0x47,         /* 147 KP_HOME */
   0x48,         /* 148 KP_UP */
   0x49,         /* 149 KP_PgUp */
   0x4b,         /* 150 KP_Left */
   0x4c,         /* 151 KP_ */
   0x4d,         /* 152 KP_Right */
   0x4f,         /* 153 KP_End */
   0x50,         /* 154 KP_Down */
   0x51,         /* 155 KP_PgDn */
   0x52,         /* 156 KP_Ins */
   0x53,         /* 157 KP_Del */
   0x0,         /* 158 */
   0x0,         /* 159 */
   0x0,         /* 160 */
   0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,         /* 170 */
   0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,         /* 180 */
   0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,         /* 190 */
   0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,         /* 200 */
   0x0,         /* 201 */
   0x0,         /* 202 */
   0x0,         /* 203 */
   0x0,         /* 204 */
   0x0,         /* 205 */
   0x0,         /* 206 */
   0x0,         /* 207 */
   0x70,         /* 208 Hiragana_Katakana */
   0x0,         /* 209 */
   0x0,         /* 210 */
   0x73,         /* 211 backslash */
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

/* FIXME N.B. on Windows, gtk probably returns PC scan codes */

uint8_t x_keycode_to_pc_keycode(int keycode)
{
	if (keycode < 9)
		keycode = 0;
	else if (keycode < 97)
		keycode -= 8; /* just an offset */
	else if (keycode < 212)
		keycode = x_keycode_to_pc_keycode_table[keycode - 97];
	else
		keycode = 0;

	return keycode;
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
