/*
 * GTK VNC Widget
 *
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.0 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifndef _GTK_VNC_X_KEYMAP_H
#define _GTK_VNC_X_KEYMAP_H

#include <gdk/gdk.h>

const guint8 const *x_keycode_to_pc_keycode_map(void);
guint16 x_keycode_to_pc_keycode(const guint8 *keycode_map,
				guint16 keycode);
void x_keymap_set_keymap_entries(void);
void x_keymap_free_keymap_entries(void);
guint x_keymap_get_keyval_from_keycode(guint keycode, guint keyval);

#endif
