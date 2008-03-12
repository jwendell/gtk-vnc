#ifndef _GTK_VNC_X_KEYMAP_H
#define _GTK_VNC_X_KEYMAP_H

#include <stdint.h>
#include <gdk/gdk.h>

uint8_t x_keycode_to_pc_keycode(int keycode);
void x_keymap_set_keymap_entries();
void x_keymap_free_keymap_entries();
guint x_keymap_get_keyval_from_keycode(guint keycode, guint keyval);

#endif
