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

#include "vnccolormap.h"

GType vnc_color_map_get_type(void)
{
	static GType color_map_type = 0;

	if (G_UNLIKELY(color_map_type == 0)) {
		color_map_type = g_boxed_type_register_static
			("VncColorMap",
			 (GBoxedCopyFunc)vnc_color_map_copy,
			 (GBoxedFreeFunc)vnc_color_map_free);
	}

	return color_map_type;
}


VncColorMap *vnc_color_map_new(guint16 offset, guint16 size)
{
	VncColorMap *map;

	map = g_slice_new0(VncColorMap);
	map->offset = offset;
	map->size = size;
	map->colors = g_new0(VncColorMapEntry, size);

	return map;
}


VncColorMap *vnc_color_map_copy(VncColorMap *srcMap)
{
	VncColorMap *map;

	map = g_slice_dup(VncColorMap, srcMap);
	map->colors = g_new0(VncColorMapEntry, srcMap->size);
	memcpy(map->colors, srcMap->colors,
	       sizeof(VncColorMapEntry) * map->size);

	return map;
}


void vnc_color_map_free(VncColorMap *map)
{
	g_slice_free(VncColorMap, map);
}


gboolean vnc_color_map_set(VncColorMap *map,
			   guint16 idx,
			   guint16 red,
			   guint16 green,
			   guint16 blue)
{
	if (idx >= (map->size + map->offset))
		return FALSE;

	map->colors[idx - map->offset].red = red;
	map->colors[idx - map->offset].green = green;
	map->colors[idx - map->offset].blue = blue;

	return TRUE;
}


gboolean vnc_color_map_lookup(VncColorMap *map,
			      guint16 idx,
			      guint16 *red,
			      guint16 *green,
			      guint16 *blue)
{
	if (idx >= (map->size + map->offset))
		return FALSE;

	*red = map->colors[idx - map->offset].red;
	*green = map->colors[idx - map->offset].green;
	*blue = map->colors[idx - map->offset].blue;

	return TRUE;
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
