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

#include "vnccairoframebuffer.h"
#include "vncutil.h"

#define VNC_CAIRO_FRAMEBUFFER_GET_PRIVATE(obj)				\
	(G_TYPE_INSTANCE_GET_PRIVATE((obj), VNC_TYPE_CAIRO_FRAMEBUFFER, VncCairoFramebufferPrivate))

struct _VncCairoFramebufferPrivate {
	cairo_surface_t *surface;
};


G_DEFINE_TYPE(VncCairoFramebuffer, vnc_cairo_framebuffer, VNC_TYPE_BASE_FRAMEBUFFER);


enum {
	PROP_0,
	PROP_SURFACE,
};


static void vnc_cairo_framebuffer_get_property(GObject *object,
					       guint prop_id,
					       GValue *value,
					       GParamSpec *pspec)
{
	VncCairoFramebuffer *framebuffer = VNC_CAIRO_FRAMEBUFFER(object);
	VncCairoFramebufferPrivate *priv = framebuffer->priv;

	switch (prop_id) {
	case PROP_SURFACE:
		g_value_set_pointer(value, priv->surface);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
	}
}

static void vnc_cairo_framebuffer_set_property(GObject *object,
					       guint prop_id,
					       const GValue *value,
					       GParamSpec *pspec)
{
	VncCairoFramebuffer *framebuffer = VNC_CAIRO_FRAMEBUFFER(object);
	VncCairoFramebufferPrivate *priv = framebuffer->priv;

	switch (prop_id) {
	case PROP_SURFACE:
		priv->surface = g_value_get_pointer(value);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
	}
}

static void vnc_cairo_framebuffer_finalize (GObject *object)
{
	VncCairoFramebuffer *fb = VNC_CAIRO_FRAMEBUFFER(object);
	VncCairoFramebufferPrivate *priv = fb->priv;

	if (priv->surface)
		cairo_surface_destroy(priv->surface);

	G_OBJECT_CLASS(vnc_cairo_framebuffer_parent_class)->finalize (object);
}

static void vnc_cairo_framebuffer_class_init(VncCairoFramebufferClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = vnc_cairo_framebuffer_finalize;
	object_class->get_property = vnc_cairo_framebuffer_get_property;
	object_class->set_property = vnc_cairo_framebuffer_set_property;

	g_object_class_install_property(object_class,
					PROP_SURFACE,
					g_param_spec_pointer("surface",
							     "The cairo surface",
							     "The cairo surface for the framebuffer",
							     G_PARAM_READABLE |
							     G_PARAM_WRITABLE |
							     G_PARAM_CONSTRUCT_ONLY |
							     G_PARAM_STATIC_NAME |
							     G_PARAM_STATIC_NICK |
							     G_PARAM_STATIC_BLURB));

	g_type_class_add_private(klass, sizeof(VncCairoFramebufferPrivate));
}


void vnc_cairo_framebuffer_init(VncCairoFramebuffer *fb)
{
	VncCairoFramebufferPrivate *priv;

	priv = fb->priv = VNC_CAIRO_FRAMEBUFFER_GET_PRIVATE(fb);

	memset(priv, 0, sizeof(*priv));
}


VncCairoFramebuffer *vnc_cairo_framebuffer_new(guint16 width, guint16 height,
					       const VncPixelFormat *remoteFormat)
{
	VncPixelFormat localFormat;
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
	guint8 *pixels;

	VNC_DEBUG("Surface %dx%d", width, height);

	localFormat.red_max = 255;
	localFormat.green_max = 255;
	localFormat.blue_max = 255;
	localFormat.red_shift = 16;
	localFormat.green_shift = 8;
	localFormat.blue_shift = 0;
	localFormat.depth = 32;
	localFormat.bits_per_pixel = 32;
	/* XXX is cairo native endian ? */
	localFormat.byte_order = G_LITTLE_ENDIAN;

	pixels = cairo_image_surface_get_data(surface);

	memset(pixels, 0, width * height * 4);

	return VNC_CAIRO_FRAMEBUFFER(g_object_new(VNC_TYPE_CAIRO_FRAMEBUFFER,
						  "surface", surface,
						  "buffer", pixels,
						  "width", width,
						  "height", height,
						  "rowstride", cairo_image_surface_get_stride(surface),
						  "local-format", &localFormat,
						  "remote-format", remoteFormat,
						  NULL));
}


cairo_surface_t *vnc_cairo_framebuffer_get_surface(VncCairoFramebuffer *fb)
{
	VncCairoFramebufferPrivate *priv = fb->priv;

	return priv->surface;
}


/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
