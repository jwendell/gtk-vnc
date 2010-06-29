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

#ifndef VNC_UTIL_H
#define VNC_UTIL_H

#include <glib.h>

G_BEGIN_DECLS

void vnc_util_set_debug(gboolean enabled);
gboolean vnc_util_get_debug(void);

#define VNC_DEBUG(fmt, ...)						\
	do {								\
		if (G_UNLIKELY(vnc_util_get_debug()))			\
			g_debug(__FILE__ " " fmt, ## __VA_ARGS__);	\
	} while (0)

/* For normal VncXXXClass structs */
#define VNC_PADDING 4

/* For very extensible VncXXXClass structs */
#define VNC_PADDING_LARGE 20

G_END_DECLS

#endif /* VNC_UTIL_H */
/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
