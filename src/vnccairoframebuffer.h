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

#ifndef VNC_CAIRO_FRAMEBUFFER_H
#define VNC_CAIRO_FRAMEBUFFER_H

#include <gdk/gdk.h>

#include <vncbaseframebuffer.h>
#include <vncutil.h>

G_BEGIN_DECLS

#define VNC_TYPE_CAIRO_FRAMEBUFFER            (vnc_cairo_framebuffer_get_type ())
#define VNC_CAIRO_FRAMEBUFFER(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), VNC_TYPE_CAIRO_FRAMEBUFFER, VncCairoFramebuffer))
#define VNC_CAIRO_FRAMEBUFFER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), VNC_TYPE_CAIRO_FRAMEBUFFER, VncCairoFramebufferClass))
#define VNC_IS_CAIRO_FRAMEBUFFER(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), VNC_TYPE_CAIRO_FRAMEBUFFER))
#define VNC_IS_CAIRO_FRAMEBUFFER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), VNC_TYPE_CAIRO_FRAMEBUFFER))
#define VNC_CAIRO_FRAMEBUFFER_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), VNC_TYPE_CAIRO_FRAMEBUFFER, VncCairoFramebufferClass))


typedef struct _VncCairoFramebuffer VncCairoFramebuffer;
typedef struct _VncCairoFramebufferPrivate VncCairoFramebufferPrivate;
typedef struct _VncCairoFramebufferClass VncCairoFramebufferClass;

struct _VncCairoFramebuffer
{
	VncBaseFramebuffer parent;

	VncCairoFramebufferPrivate *priv;

	/* Do not add fields to this struct */
};

struct _VncCairoFramebufferClass
{
	VncBaseFramebufferClass parent_class;

	/*
	 * If adding fields to this struct, remove corresponding
	 * amount of padding to avoid changing overall struct size
	 */
	gpointer _vnc_reserved[VNC_PADDING];
};


GType vnc_cairo_framebuffer_get_type(void) G_GNUC_CONST;

VncCairoFramebuffer *vnc_cairo_framebuffer_new(guint16 width, guint16 height,
					       const VncPixelFormat *remoteFormat);

cairo_surface_t *vnc_cairo_framebuffer_get_surface(VncCairoFramebuffer *fb);


G_END_DECLS

#endif /* VNC_CAIRO_FRAMEBUFFER_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
