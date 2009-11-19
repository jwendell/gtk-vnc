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

#ifndef VNC_IMAGE_FRAMEBUFFER_H
#define VNC_IMAGE_FRAMEBUFFER_H

#include <gdk/gdk.h>

#include <vncbaseframebuffer.h>

G_BEGIN_DECLS

#define VNC_TYPE_IMAGE_FRAMEBUFFER            (vnc_image_framebuffer_get_type ())
#define VNC_IMAGE_FRAMEBUFFER(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), VNC_TYPE_IMAGE_FRAMEBUFFER, VncImageFramebuffer))
#define VNC_IMAGE_FRAMEBUFFER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), VNC_TYPE_IMAGE_FRAMEBUFFER, VncImageFramebufferClass))
#define VNC_IS_IMAGE_FRAMEBUFFER(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), VNC_TYPE_IMAGE_FRAMEBUFFER))
#define VNC_IS_IMAGE_FRAMEBUFFER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), VNC_TYPE_IMAGE_FRAMEBUFFER))
#define VNC_IMAGE_FRAMEBUFFER_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), VNC_TYPE_IMAGE_FRAMEBUFFER, VncImageFramebufferClass))


typedef struct _VncImageFramebuffer VncImageFramebuffer;
typedef struct _VncImageFramebufferPrivate VncImageFramebufferPrivate;
typedef struct _VncImageFramebufferClass VncImageFramebufferClass;

struct _VncImageFramebuffer
{
	VncBaseFramebuffer parent;

	VncImageFramebufferPrivate *priv;
};

struct _VncImageFramebufferClass
{
	VncBaseFramebufferClass parent_class;

};


GType vnc_image_framebuffer_get_type(void) G_GNUC_CONST;

VncImageFramebuffer *vnc_image_framebuffer_new(GdkImage *image,
					       const VncPixelFormat *remoteFormat);

GdkImage *vnc_image_framebuffer_get_image(VncImageFramebuffer *fb);


G_END_DECLS

#endif /* VNC_IMAGE_FRAMEBUFFER_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
