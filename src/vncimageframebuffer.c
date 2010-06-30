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

#include <string.h>
#include <gtk/gtk.h>

#include "vncimageframebuffer.h"
#include "vncutil.h"

#define VNC_IMAGE_FRAMEBUFFER_GET_PRIVATE(obj)				\
	(G_TYPE_INSTANCE_GET_PRIVATE((obj), VNC_TYPE_IMAGE_FRAMEBUFFER, VncImageFramebufferPrivate))

struct _VncImageFramebufferPrivate {
	GdkImage *image;
};


G_DEFINE_TYPE(VncImageFramebuffer, vnc_image_framebuffer, VNC_TYPE_BASE_FRAMEBUFFER);


enum {
	PROP_0,
	PROP_IMAGE,
};


static void vnc_image_framebuffer_get_property(GObject *object,
					       guint prop_id,
					       GValue *value,
					       GParamSpec *pspec)
{
	VncImageFramebuffer *framebuffer = VNC_IMAGE_FRAMEBUFFER(object);
	VncImageFramebufferPrivate *priv = framebuffer->priv;

	switch (prop_id) {
	case PROP_IMAGE:
		g_value_set_object(value, priv->image);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
	}
}

static void vnc_image_framebuffer_set_property(GObject *object,
					       guint prop_id,
					       const GValue *value,
					       GParamSpec *pspec)
{
	VncImageFramebuffer *framebuffer = VNC_IMAGE_FRAMEBUFFER(object);
	VncImageFramebufferPrivate *priv = framebuffer->priv;

	switch (prop_id) {
	case PROP_IMAGE:
		if (priv->image)
			g_object_unref(G_OBJECT(priv->image));
		priv->image = g_value_get_object(value);
		g_object_ref(G_OBJECT(priv->image));
		break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        }
}

static void vnc_image_framebuffer_finalize (GObject *object)
{
	VncImageFramebuffer *fb = VNC_IMAGE_FRAMEBUFFER(object);
	VncImageFramebufferPrivate *priv = fb->priv;

	if (priv->image)
		g_object_unref(priv->image);

	G_OBJECT_CLASS(vnc_image_framebuffer_parent_class)->finalize (object);
}

static void vnc_image_framebuffer_class_init(VncImageFramebufferClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = vnc_image_framebuffer_finalize;
	object_class->get_property = vnc_image_framebuffer_get_property;
	object_class->set_property = vnc_image_framebuffer_set_property;

	g_object_class_install_property(object_class,
					PROP_IMAGE,
					g_param_spec_object("image",
							    "The client image",
							    "The client image for the framebuffer",
							    GDK_TYPE_IMAGE,
							    G_PARAM_READABLE |
							    G_PARAM_WRITABLE |
							    G_PARAM_CONSTRUCT_ONLY |
							    G_PARAM_STATIC_NAME |
							    G_PARAM_STATIC_NICK |
							    G_PARAM_STATIC_BLURB));

	g_type_class_add_private(klass, sizeof(VncImageFramebufferPrivate));
}


void vnc_image_framebuffer_init(VncImageFramebuffer *fb)
{
	VncImageFramebufferPrivate *priv;

	priv = fb->priv = VNC_IMAGE_FRAMEBUFFER_GET_PRIVATE(fb);

	memset(priv, 0, sizeof(*priv));
}


VncImageFramebuffer *vnc_image_framebuffer_new(GdkImage *image,
					       const VncPixelFormat *remoteFormat)
{
        VncPixelFormat localFormat;
        guint32 red_mask, green_mask, blue_mask;
        gint red_shift, green_shift, blue_shift, w, h;
        guint16 bpp, bpl, depth;
        gpointer pixels;
        GdkByteOrder byte_order;

	#if GTK_CHECK_VERSION (2, 21, 1)
	GdkVisual *visual = gdk_image_get_visual (image);
	gdk_visual_get_red_pixel_details (visual, &red_mask, &red_shift, NULL);
	gdk_visual_get_green_pixel_details (visual, &green_mask, &green_shift, NULL);
	gdk_visual_get_blue_pixel_details (visual, &blue_mask, &blue_shift, NULL);
	bpp = gdk_image_get_bytes_per_pixel (image);
	depth = gdk_image_get_depth (image);
	byte_order = gdk_image_get_byte_order (image);
	bpl = gdk_image_get_bytes_per_line (image);
	w = gdk_image_get_width (image);
	h = gdk_image_get_height (image);
	pixels = gdk_image_get_pixels (image);
	#else
	red_mask = image->visual->red_mask;
	red_shift = image->visual->red_shift;
	green_mask = image->visual->green_mask;
	green_shift = image->visual->green_shift;
	blue_mask = image->visual->blue_mask;
	blue_shift = image->visual->blue_shift;
	bpp = image->bpp;
	depth = image->depth;
	byte_order = image->byte_order;
	bpl = image->bpl;
	w = image->width;
	h = image->height;
	pixels = image->mem;
	#endif

	VNC_DEBUG("Visual mask: %3d %3d %3d\n      shift: %3d %3d %3d",
		   red_mask,
		   green_mask,
		   blue_mask,
		   red_shift,
		   green_shift,
		   blue_shift);

	localFormat.red_max = red_mask >> red_shift;
	localFormat.green_max = green_mask >> green_shift;
	localFormat.blue_max = blue_mask >> blue_shift;
	localFormat.red_shift = red_shift;
	localFormat.green_shift = green_shift;
	localFormat.blue_shift = blue_shift;
	localFormat.depth = depth;
	localFormat.bits_per_pixel = bpp * 8;
	localFormat.byte_order = byte_order == GDK_LSB_FIRST ? G_LITTLE_ENDIAN : G_BIG_ENDIAN;

	memset(pixels, 0, bpl * h);

	return VNC_IMAGE_FRAMEBUFFER(g_object_new(VNC_TYPE_IMAGE_FRAMEBUFFER,
						  "image", image,
						  "buffer", (guint8 *)pixels,
						  "width", w,
						  "height", h,
						  "rowstride", bpl,
						  "local-format", &localFormat,
						  "remote-format", remoteFormat,
						  NULL));
}


GdkImage *vnc_image_framebuffer_get_image(VncImageFramebuffer *fb)
{
	VncImageFramebufferPrivate *priv = fb->priv;

	return priv->image;
}


/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
