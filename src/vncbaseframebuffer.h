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

#ifndef VNC_BASE_FRAMEBUFFER_H
#define VNC_BASE_FRAMEBUFFER_H

#include <glib-object.h>

#include <vncframebuffer.h>

G_BEGIN_DECLS

#define VNC_TYPE_BASE_FRAMEBUFFER            (vnc_base_framebuffer_get_type ())
#define VNC_BASE_FRAMEBUFFER(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), VNC_TYPE_BASE_FRAMEBUFFER, VncBaseFramebuffer))
#define VNC_BASE_FRAMEBUFFER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), VNC_TYPE_BASE_FRAMEBUFFER, VncBaseFramebufferClass))
#define VNC_IS_BASE_FRAMEBUFFER(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), VNC_TYPE_BASE_FRAMEBUFFER))
#define VNC_IS_BASE_FRAMEBUFFER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), VNC_TYPE_BASE_FRAMEBUFFER))
#define VNC_BASE_FRAMEBUFFER_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), VNC_TYPE_BASE_FRAMEBUFFER, VncBAseFramebufferClass))


typedef struct _VncBaseFramebuffer VncBaseFramebuffer;
typedef struct _VncBaseFramebufferPrivate VncBaseFramebufferPrivate;
typedef struct _VncBaseFramebufferClass VncBaseFramebufferClass;

struct _VncBaseFramebuffer
{
	GObject parent;

	VncBaseFramebufferPrivate *priv;
};

struct _VncBaseFramebufferClass
{
	GObjectClass parent_class;

};


GType vnc_base_framebuffer_get_type(void) G_GNUC_CONST;

VncBaseFramebuffer *vnc_base_framebuffer_new(guint8 *buffer,
					     guint16 width,
					     guint16 height,
					     int rowstride,
					     const VncPixelFormat *localFormat,
					     const VncPixelFormat *remoteFormat);



G_END_DECLS

#endif /* VNC_BASE_FRAMEBUFFER_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
