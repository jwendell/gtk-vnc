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

#ifndef VNC_PIXEL_FORMAT_H
#define VNC_PIXEL_FORMAT_H

#include <glib.h>
#include <glib-object.h>

G_BEGIN_DECLS

#define VNC_TYPE_PIXEL_FORMAT            (vnc_pixel_format_get_type ())

typedef struct _VncPixelFormat VncPixelFormat;

struct _VncPixelFormat {
	guint8 bits_per_pixel;
	guint8 depth;
	guint16 byte_order;
	guint8 true_color_flag;
	guint16 red_max;
	guint16 green_max;
	guint16 blue_max;
	guint8 red_shift;
	guint8 green_shift;
	guint8 blue_shift;

	/* Do not add fields to this struct */
};

GType vnc_pixel_format_get_type(void);

VncPixelFormat *vnc_pixel_format_new(void);
VncPixelFormat *vnc_pixel_format_copy(VncPixelFormat *format);
void vnc_pixel_format_free(VncPixelFormat *format);

G_END_DECLS

#endif /* VNC_PIXEL_FORMAT_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
