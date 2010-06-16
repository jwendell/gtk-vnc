/*
 * GTK VNC Widget
 *
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 * Copyright (C) 2009-2010 Daniel P. Berrange <dan@berrange.com>
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

#ifndef VNC_COLOR_MAP_H
#define VNC_COLOR_MAP_H

#include <glib.h>
#include <glib-object.h>

G_BEGIN_DECLS

#define VNC_TYPE_COLOR_MAP            (vnc_color_map_get_type ())

typedef struct _VncColorMap VncColorMap;
typedef struct _VncColorMapEntry VncColorMapEntry;

struct _VncColorMap {
	guint16 offset;
	guint16 size;
	VncColorMapEntry *colors;
};

struct _VncColorMapEntry {
	guint16 red;
	guint16 green;
	guint16 blue;
};


GType vnc_color_map_get_type(void);

VncColorMap *vnc_color_map_new(guint16 offset, guint16 nentries);
VncColorMap *vnc_color_map_copy(VncColorMap *map);
void vnc_color_map_free(VncColorMap *map);

gboolean vnc_color_map_set(VncColorMap *map,
			   guint16 idx,
			   guint16 red,
			   guint16 green,
			   guint16 blue);

gboolean vnc_color_map_lookup(VncColorMap *map,
			      guint16 idx,
			      guint16 *red,
			      guint16 *green,
			      guint16 *blue);


G_END_DECLS

#endif /* VNC_COLOR_MAP_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
