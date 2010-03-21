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

#ifndef VNC_FRAMEBUFFER_H
#define VNC_FRAMEBUFFER_H

#include <glib-object.h>

#include <vncpixelformat.h>

G_BEGIN_DECLS

#define VNC_TYPE_FRAMEBUFFER            (vnc_framebuffer_get_type ())
#define VNC_FRAMEBUFFER(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), VNC_TYPE_FRAMEBUFFER, VncFramebuffer))
#define VNC_IS_FRAMEBUFFER(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), VNC_TYPE_FRAMEBUFFER))
#define VNC_FRAMEBUFFER_GET_INTERFACE(inst) (G_TYPE_INSTANCE_GET_INTERFACE ((inst), VNC_TYPE_FRAMEBUFFER, VncFramebufferInterface))


typedef struct _VncFramebuffer VncFramebuffer; /* Dummy object */
typedef struct _VncFramebufferInterface VncFramebufferInterface;

struct _VncFramebufferInterface {
	GTypeInterface parent;

	guint16 (*get_width)(VncFramebuffer *fb);
	guint16 (*get_height)(VncFramebuffer *fb);
	int (*get_rowstride)(VncFramebuffer *fb);
	guint8 *(*get_buffer)(VncFramebuffer *fb);
	const VncPixelFormat *(*get_local_format)(VncFramebuffer *fb);
	const VncPixelFormat *(*get_remote_format)(VncFramebuffer *fb);
	gboolean (*perfect_format_match)(VncFramebuffer *fb);

	void (*set_pixel_at)(VncFramebuffer *fb,
			     guint8 *src, /* One remote pixel */
			     guint16 x, guint16 y);
	void (*fill)(VncFramebuffer *fb,
		     guint8 *src, /* One remote pixel */
		     guint16 x, guint16 y,
		     guint16 width, guint16 height);
	void (*copyrect)(VncFramebuffer *fb,
			 guint16 srcx, guint16 srcy,
			 guint16 dstx, guint16 dsty,
			 guint16 width, guint16 height);
	void (*blt)(VncFramebuffer *fb,
		    guint8 *src, /* Remote pixel array */
		    int rowstride,
		    guint16 x, guint16 y,
		    guint16 width, guint16 height);
	void (*rgb24_blt)(VncFramebuffer *fb,
			  guint8 *src, /* rgb24 pixel array */
			  int rowstride,
			  guint16 x, guint16 y,
			  guint16 width, guint16 height);

};

GType vnc_framebuffer_get_type(void) G_GNUC_CONST;


guint16 vnc_framebuffer_get_width(VncFramebuffer *fb);
guint16 vnc_framebuffer_get_height(VncFramebuffer *fb);
int vnc_framebuffer_get_rowstride(VncFramebuffer *fb);

guint8 *vnc_framebuffer_get_buffer(VncFramebuffer *fb);

const VncPixelFormat *vnc_framebuffer_get_local_format(VncFramebuffer *fb);
const VncPixelFormat *vnc_framebuffer_get_remote_format(VncFramebuffer *fb);
gboolean vnc_framebuffer_perfect_format_match(VncFramebuffer *fb);

void vnc_framebuffer_set_pixel_at(VncFramebuffer *fb,
				  guint8 *src, /* One remote pixel */
				  guint16 x, guint16 y);

void vnc_framebuffer_fill(VncFramebuffer *fb,
			  guint8 *src, /* One remote pixel */
			  guint16 x, guint16 y,
			  guint16 width, guint16 height);

void vnc_framebuffer_copyrect(VncFramebuffer *fb,
			      guint16 srcx, guint16 srcy,
			      guint16 dstx, guint16 dsty,
			      guint16 width, guint16 height);

void vnc_framebuffer_blt(VncFramebuffer *fb,
			 guint8 *src, /* Remote pixel array */
			 int rowstride,
			 guint16 x, guint16 y,
			 guint16 width, guint16 height);

void vnc_framebuffer_rgb24_blt(VncFramebuffer *fb,
			       guint8 *src, /* rgb24 pixel array */
			       int rowstride,
			       guint16 x, guint16 y,
			       guint16 width, guint16 height);



G_END_DECLS

#endif /* VNC_FRAMEBUFFER_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
