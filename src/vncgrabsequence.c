/*
 * GTK VNC Widget
 *
 * Copyright (C) 2010 Daniel P. Berrange <dan@berrange.com>
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

#include <config.h>

#include <string.h>
#include <gdk/gdk.h>

#include "vncgrabsequence.h"

GType vnc_grab_sequence_get_type(void)
{
	static GType grab_sequence_type = 0;

	if (G_UNLIKELY(grab_sequence_type == 0)) {
		grab_sequence_type = g_boxed_type_register_static
			("VncGrabSequence",
			 (GBoxedCopyFunc)vnc_grab_sequence_copy,
			 (GBoxedFreeFunc)vnc_grab_sequence_free);
	}

	return grab_sequence_type;
}


VncGrabSequence *vnc_grab_sequence_new(guint nkeysyms, guint *keysyms)
{
	VncGrabSequence *sequence;

	sequence = g_slice_new0(VncGrabSequence);
	sequence->nkeysyms = nkeysyms;
	sequence->keysyms = g_new0(guint, nkeysyms);
	memcpy(sequence->keysyms, keysyms, sizeof(guint)*nkeysyms);

	return sequence;
}


VncGrabSequence *vnc_grab_sequence_new_from_string(const gchar *str)
{
	gchar **keysymstr;
	int i;
	VncGrabSequence *sequence;

	sequence = g_slice_new0(VncGrabSequence);

	keysymstr = g_strsplit(str, "+", 5);

	sequence->nkeysyms = 0;
	while (keysymstr[sequence->nkeysyms])
		sequence->nkeysyms++;

	sequence->keysyms = g_new0(guint, sequence->nkeysyms);
	for (i = 0 ; i < sequence->nkeysyms ; i++)
		sequence->keysyms[i] =
			(guint)gdk_keyval_from_name(keysymstr[i]);
	
	return sequence;

}


VncGrabSequence *vnc_grab_sequence_copy(VncGrabSequence *srcSequence)
{
	VncGrabSequence *sequence;

	sequence = g_slice_dup(VncGrabSequence, srcSequence);
	sequence->keysyms = g_new0(guint, srcSequence->nkeysyms);
	memcpy(sequence->keysyms, srcSequence->keysyms,
	       sizeof(guint) * sequence->nkeysyms);

	return sequence;
}


void vnc_grab_sequence_free(VncGrabSequence *sequence)
{
	g_slice_free(VncGrabSequence, sequence);
}


gchar *vnc_grab_sequence_as_string(VncGrabSequence *sequence)
{
	GString *str = g_string_new("");
	int i;

	for (i = 0 ; i < sequence->nkeysyms ; i++) {
		if (i > 0)
			g_string_append_c(str, '+');
		g_string_append(str, gdk_keyval_name(sequence->keysyms[i]));
	}

	return g_string_free(str, FALSE);

}


/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
