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

#include <config.h>

#include "vncframebuffer.h"


guint16 vnc_framebuffer_get_width(VncFramebuffer *fb)
{
	return VNC_FRAMEBUFFER_GET_INTERFACE(fb)->get_width(fb);
}

guint16 vnc_framebuffer_get_height(VncFramebuffer *fb)
{
	return VNC_FRAMEBUFFER_GET_INTERFACE(fb)->get_height(fb);
}

int vnc_framebuffer_get_rowstride(VncFramebuffer *fb)
{
	return VNC_FRAMEBUFFER_GET_INTERFACE(fb)->get_rowstride(fb);
}

guint8 *vnc_framebuffer_get_buffer(VncFramebuffer *fb)
{
	return VNC_FRAMEBUFFER_GET_INTERFACE(fb)->get_buffer(fb);
}

const VncPixelFormat *vnc_framebuffer_get_local_format(VncFramebuffer *fb)
{
	return VNC_FRAMEBUFFER_GET_INTERFACE(fb)->get_local_format(fb);
}

const VncPixelFormat *vnc_framebuffer_get_remote_format(VncFramebuffer *fb)
{
	return VNC_FRAMEBUFFER_GET_INTERFACE(fb)->get_remote_format(fb);
}

gboolean vnc_framebuffer_perfect_format_match(VncFramebuffer *fb)
{
	return VNC_FRAMEBUFFER_GET_INTERFACE(fb)->perfect_format_match(fb);
}

void vnc_framebuffer_set_pixel_at(VncFramebuffer *fb,
				  guint8 *src,
				  guint16 x, guint16 y)
{
	VNC_FRAMEBUFFER_GET_INTERFACE(fb)->set_pixel_at(fb, src, x, y);
}

void vnc_framebuffer_fill(VncFramebuffer *fb,
			  guint8 *src,
			  guint16 x, guint16 y,
			  guint16 width, guint16 height)
{
	VNC_FRAMEBUFFER_GET_INTERFACE(fb)->fill(fb, src, x, y, width, height);
}

void vnc_framebuffer_copyrect(VncFramebuffer *fb,
			      guint16 srcx, guint16 srcy,
			      guint16 dstx, guint16 dsty,
			      guint16 width, guint16 height)
{
	VNC_FRAMEBUFFER_GET_INTERFACE(fb)->copyrect(fb, srcx, srcy, dstx, dsty, width, height);
}

void vnc_framebuffer_blt(VncFramebuffer *fb,
			 guint8 *src,
			 int rowstride,
			 guint16 x, guint16 y,
			 guint16 width, guint16 height)
{
	VNC_FRAMEBUFFER_GET_INTERFACE(fb)->blt(fb, src, rowstride, x, y, width, height);
}

void vnc_framebuffer_rgb24_blt(VncFramebuffer *fb,
			       guint8 *src,
			       int rowstride,
			       guint16 x, guint16 y,
			       guint16 width, guint16 height)
{
	VNC_FRAMEBUFFER_GET_INTERFACE(fb)->rgb24_blt(fb, src, rowstride, x, y, width, height);
}


void vnc_framebuffer_set_color_map(VncFramebuffer *fb,
				   VncColorMap *map)
{
	VNC_FRAMEBUFFER_GET_INTERFACE(fb)->set_color_map(fb, map);
}


GType
vnc_framebuffer_get_type (void)
{
	static GType framebuffer_type = 0;

	if (!framebuffer_type) {
		framebuffer_type =
			g_type_register_static_simple (G_TYPE_INTERFACE, "VncFramebuffer",
						       sizeof (VncFramebufferInterface),
						       NULL, 0, NULL, 0);

		g_type_interface_add_prerequisite (framebuffer_type, G_TYPE_OBJECT);
	}

	return framebuffer_type;
}


/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
