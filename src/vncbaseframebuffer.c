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

#include "vncbaseframebuffer.h"
#include "vncutil.h"

typedef void vnc_base_framebuffer_blt_func(VncBaseFramebufferPrivate *priv,
					   guint8 *src,
					   int rowstride,
					   guint16 x, guint16 y,
					   guint16 widht, guint16 width);
typedef void vnc_base_framebuffer_fill_func(VncBaseFramebufferPrivate *priv,
					    guint8 *src,
					    guint16 x, guint16 y,
					    guint16 widht, guint16 width);
typedef void vnc_base_framebuffer_set_pixel_at_func(VncBaseFramebufferPrivate *priv,
						    guint8 *src,
						    guint16 x, guint16 y);
typedef void vnc_base_framebuffer_rgb24_blt_func(VncBaseFramebufferPrivate *priv,
						 guint8 *src, int rowstride,
						 guint16 x, guint16 y,
						 guint16 width, guint16 height);


#define VNC_BASE_FRAMEBUFFER_GET_PRIVATE(obj)				\
	(G_TYPE_INSTANCE_GET_PRIVATE((obj), VNC_TYPE_BASE_FRAMEBUFFER, VncBaseFramebufferPrivate))

struct _VncBaseFramebufferPrivate {
	guint8 *buffer; /* Owned by caller, so no need to free */
	guint16 width;
	guint16 height;
	int rowstride;

	VncPixelFormat *localFormat;
	VncPixelFormat *remoteFormat;

	VncColorMap *colorMap;

	/* TRUE if the following derived data needs reinitializing */
	gboolean reinitRenderFuncs;

	/* Derived from above data */
	int rm, gm, bm;
        int rrs, grs, brs;
        int rls, gls, bls;

	/* TRUE if localFormat == remoteFormat */
        gboolean perfect_match;

	/* Render function impls for this local+remote format pair */
        vnc_base_framebuffer_set_pixel_at_func *set_pixel_at;
        vnc_base_framebuffer_fill_func *fill;
        vnc_base_framebuffer_blt_func *blt;
        vnc_base_framebuffer_rgb24_blt_func *rgb24_blt;
};

#define VNC_BASE_FRAMEBUFFER_AT(priv, x, y) \
	((priv)->buffer + ((y) * (priv)->rowstride) + ((x) * ((priv)->localFormat->bits_per_pixel/8)))


static void vnc_base_framebuffer_interface_init (gpointer g_iface,
                                                 gpointer iface_data);

G_DEFINE_TYPE_EXTENDED(VncBaseFramebuffer, vnc_base_framebuffer, G_TYPE_OBJECT, 0,
                       G_IMPLEMENT_INTERFACE(VNC_TYPE_FRAMEBUFFER, vnc_base_framebuffer_interface_init));


enum {
	PROP_0,
	PROP_BUFFER,
	PROP_WIDTH,
	PROP_HEIGHT,
	PROP_ROWSTRIDE,
	PROP_LOCAL_FORMAT,
	PROP_REMOTE_FORMAT,
	PROP_COLOR_MAP,
};


static void vnc_base_framebuffer_get_property(GObject *object,
					      guint prop_id,
					      GValue *value,
					      GParamSpec *pspec)
{
	VncBaseFramebuffer *framebuffer = VNC_BASE_FRAMEBUFFER(object);
	VncBaseFramebufferPrivate *priv = framebuffer->priv;

	switch (prop_id) {
	case PROP_BUFFER:
		g_value_set_pointer(value, priv->buffer);
		break;

	case PROP_WIDTH:
		g_value_set_int(value, priv->width);
		break;

	case PROP_HEIGHT:
		g_value_set_int(value, priv->height);
		break;

	case PROP_ROWSTRIDE:
		g_value_set_int(value, priv->rowstride);
		break;

	case PROP_LOCAL_FORMAT:
		g_value_set_boxed(value, priv->localFormat);
		break;

	case PROP_REMOTE_FORMAT:
		g_value_set_boxed(value, priv->remoteFormat);
		break;

	case PROP_COLOR_MAP:
		g_value_set_boxed(value, priv->colorMap);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
	}
}

static void vnc_base_framebuffer_set_property(GObject *object,
					      guint prop_id,
					      const GValue *value,
					      GParamSpec *pspec)
{
	VncBaseFramebuffer *framebuffer = VNC_BASE_FRAMEBUFFER(object);
	VncBaseFramebufferPrivate *priv = framebuffer->priv;

	switch (prop_id){
	case PROP_BUFFER:
		priv->buffer = g_value_get_pointer(value);
		priv->reinitRenderFuncs = TRUE;
		break;

	case PROP_WIDTH:
		priv->width = g_value_get_int(value);
		priv->reinitRenderFuncs = TRUE;
		break;

	case PROP_HEIGHT:
		priv->height = g_value_get_int(value);
		priv->reinitRenderFuncs = TRUE;
		break;

	case PROP_ROWSTRIDE:
		priv->rowstride = g_value_get_int(value);
		priv->reinitRenderFuncs = TRUE;
		break;

	case PROP_LOCAL_FORMAT:
		if (priv->localFormat)
			vnc_pixel_format_free(priv->localFormat);
		priv->localFormat = g_value_dup_boxed(value);
		priv->reinitRenderFuncs = TRUE;
		break;

	case PROP_REMOTE_FORMAT:
		if (priv->remoteFormat)
			vnc_pixel_format_free(priv->remoteFormat);
		priv->remoteFormat = g_value_dup_boxed(value);
		priv->reinitRenderFuncs = TRUE;
		break;

	case PROP_COLOR_MAP:
		if (priv->colorMap)
			vnc_color_map_free(priv->colorMap);
		priv->colorMap = g_value_dup_boxed(value);
		priv->reinitRenderFuncs = TRUE;
		break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        }
}


static void vnc_base_framebuffer_finalize(GObject *object)
{
	VncBaseFramebuffer *ab = VNC_BASE_FRAMEBUFFER(object);
	VncBaseFramebufferPrivate *priv = ab->priv;

	VNC_DEBUG("Finalize VncBaseFramebuffer=%p", ab);

	if (priv->localFormat)
		vnc_pixel_format_free(priv->localFormat);
	if (priv->remoteFormat)
		vnc_pixel_format_free(priv->remoteFormat);
	if (priv->colorMap)
		vnc_color_map_free(priv->colorMap);

	G_OBJECT_CLASS(vnc_base_framebuffer_parent_class)->finalize (object);
}


static void vnc_base_framebuffer_class_init(VncBaseFramebufferClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = vnc_base_framebuffer_finalize;
	object_class->get_property = vnc_base_framebuffer_get_property;
	object_class->set_property = vnc_base_framebuffer_set_property;

	g_object_class_install_property(object_class,
					PROP_BUFFER,
					g_param_spec_pointer("buffer",
							     "The framebuffer",
							     "The framebuffer memory region",
							     G_PARAM_READABLE |
							     G_PARAM_WRITABLE |
							     G_PARAM_CONSTRUCT_ONLY |
							     G_PARAM_STATIC_NAME |
							     G_PARAM_STATIC_NICK |
							     G_PARAM_STATIC_BLURB));

	g_object_class_install_property(object_class,
					PROP_WIDTH,
					g_param_spec_int("width",
							 "Framebuffer width",
							 "Width of the framebuffer in pixels",
							 0, 1 << 16, 0,
							 G_PARAM_READABLE |
							 G_PARAM_WRITABLE |
							 G_PARAM_CONSTRUCT_ONLY |
							 G_PARAM_STATIC_NAME |
							 G_PARAM_STATIC_NICK |
							 G_PARAM_STATIC_BLURB));

	g_object_class_install_property(object_class,
					PROP_HEIGHT,
					g_param_spec_int("height",
							 "Framebuffer height",
							 "Height of the framebuffer in pixels",
							 0, 1 << 16, 0,
							 G_PARAM_READABLE |
							 G_PARAM_WRITABLE |
							 G_PARAM_CONSTRUCT_ONLY |
							 G_PARAM_STATIC_NAME |
							 G_PARAM_STATIC_NICK |
							 G_PARAM_STATIC_BLURB));

	g_object_class_install_property(object_class,
					PROP_ROWSTRIDE,
					g_param_spec_int("rowstride",
							 "Framebuffer rowstride",
							 "Size of one framebuffer line in bytes",
							 0, 1 << 30, 0,
							 G_PARAM_READABLE |
							 G_PARAM_WRITABLE |
							 G_PARAM_CONSTRUCT_ONLY |
							 G_PARAM_STATIC_NAME |
							 G_PARAM_STATIC_NICK |
							 G_PARAM_STATIC_BLURB));

	g_object_class_install_property(object_class,
					PROP_LOCAL_FORMAT,
					g_param_spec_boxed("local-format",
							   "Local pixel format",
							   "The local pixel format of the framebuffer",
							   VNC_TYPE_PIXEL_FORMAT,
							   G_PARAM_READABLE |
							   G_PARAM_WRITABLE |
							   G_PARAM_CONSTRUCT_ONLY |
							   G_PARAM_STATIC_NAME |
							   G_PARAM_STATIC_NICK |
							   G_PARAM_STATIC_BLURB));

	g_object_class_install_property(object_class,
					PROP_REMOTE_FORMAT,
					g_param_spec_boxed("remote-format",
							   "Remote pixel format",
							   "The remote pixel format of the framebuffer",
							   VNC_TYPE_PIXEL_FORMAT,
							   G_PARAM_READABLE |
							   G_PARAM_WRITABLE |
							   G_PARAM_CONSTRUCT_ONLY |
							   G_PARAM_STATIC_NAME |
							   G_PARAM_STATIC_NICK |
							   G_PARAM_STATIC_BLURB));

	g_object_class_install_property(object_class,
					PROP_COLOR_MAP,
					g_param_spec_boxed("color-map",
							   "Color map",
							   "The color map",
							   VNC_TYPE_COLOR_MAP,
							   G_PARAM_READABLE |
							   G_PARAM_WRITABLE |
							   G_PARAM_STATIC_NAME |
							   G_PARAM_STATIC_NICK |
							   G_PARAM_STATIC_BLURB));

	g_type_class_add_private(klass, sizeof(VncBaseFramebufferPrivate));
}


void vnc_base_framebuffer_init(VncBaseFramebuffer *fb)
{
	VncBaseFramebufferPrivate *priv;

	priv = fb->priv = VNC_BASE_FRAMEBUFFER_GET_PRIVATE(fb);

	memset(priv, 0, sizeof(*priv));
	priv->reinitRenderFuncs = TRUE;

	priv->localFormat = vnc_pixel_format_new();
	priv->remoteFormat = vnc_pixel_format_new();
}


VncBaseFramebuffer *vnc_base_framebuffer_new(guint8 *buffer,
					     guint16 width,
					     guint16 height,
					     int rowstride,
					     const VncPixelFormat *localFormat,
					     const VncPixelFormat *remoteFormat)
{
	return VNC_BASE_FRAMEBUFFER(g_object_new(VNC_TYPE_BASE_FRAMEBUFFER,
						 "buffer", buffer,
						 "width", width,
						 "height", height,
						 "rowstride", rowstride,
						 "local-format", localFormat,
						 "remote-format", remoteFormat,
						 NULL));
}


static guint16 vnc_base_framebuffer_get_width(VncFramebuffer *iface)
{
	VncBaseFramebuffer *fb = VNC_BASE_FRAMEBUFFER(iface);
	VncBaseFramebufferPrivate *priv = fb->priv;

	return priv->width;
}


static guint16 vnc_base_framebuffer_get_height(VncFramebuffer *iface)
{
	VncBaseFramebuffer *fb = VNC_BASE_FRAMEBUFFER(iface);
	VncBaseFramebufferPrivate *priv = fb->priv;

	return priv->height;
}


static int vnc_base_framebuffer_get_rowstride(VncFramebuffer *iface)
{
	VncBaseFramebuffer *fb = VNC_BASE_FRAMEBUFFER(iface);
	VncBaseFramebufferPrivate *priv = fb->priv;

	return priv->rowstride;
}


static guint8 *vnc_base_framebuffer_get_buffer(VncFramebuffer *iface)
{
	VncBaseFramebuffer *fb = VNC_BASE_FRAMEBUFFER(iface);
	VncBaseFramebufferPrivate *priv = fb->priv;

	return priv->buffer;
}


static const VncPixelFormat *vnc_base_framebuffer_get_local_format(VncFramebuffer *iface)
{
	VncBaseFramebuffer *fb = VNC_BASE_FRAMEBUFFER(iface);
	VncBaseFramebufferPrivate *priv = fb->priv;

	return priv->localFormat;
}


static const VncPixelFormat *vnc_base_framebuffer_get_remote_format(VncFramebuffer *iface)
{
	VncBaseFramebuffer *fb = VNC_BASE_FRAMEBUFFER(iface);
	VncBaseFramebufferPrivate *priv = fb->priv;

	return priv->remoteFormat;
}


static gboolean vnc_base_framebuffer_perfect_format_match(VncFramebuffer *iface)
{
	VncBaseFramebuffer *fb = VNC_BASE_FRAMEBUFFER(iface);
	VncBaseFramebufferPrivate *priv = fb->priv;

	return priv->perfect_match;
}


static guint8 vnc_base_framebuffer_swap_img_8(VncBaseFramebufferPrivate *priv G_GNUC_UNUSED, guint8 pixel)
{
	return pixel;
}


static guint8 vnc_base_framebuffer_swap_rfb_8(VncBaseFramebufferPrivate *priv G_GNUC_UNUSED, guint8 pixel)
{
	return pixel;
}


/* local host native format -> X server image format */
static guint16 vnc_base_framebuffer_swap_img_16(VncBaseFramebufferPrivate *priv, guint16 pixel)
{
	if (G_BYTE_ORDER != priv->localFormat->byte_order)
		return  (((pixel >> 8) & 0xFF) << 0) |
			(((pixel >> 0) & 0xFF) << 8);
	else
		return pixel;
}


/* VNC server RFB  format ->  local host native format */
static guint16 vnc_base_framebuffer_swap_rfb_16(VncBaseFramebufferPrivate *priv, guint16 pixel)
{
	if (priv->remoteFormat->byte_order != G_BYTE_ORDER)
		return  (((pixel >> 8) & 0xFF) << 0) |
			(((pixel >> 0) & 0xFF) << 8);
	else
		return pixel;
}


/* local host native format -> X server image format */
static guint32 vnc_base_framebuffer_swap_img_32(VncBaseFramebufferPrivate *priv, guint32 pixel)
{
	if (G_BYTE_ORDER != priv->localFormat->byte_order)
		return  (((pixel >> 24) & 0xFF) <<  0) |
			(((pixel >> 16) & 0xFF) <<  8) |
			(((pixel >>  8) & 0xFF) << 16) |
			(((pixel >>  0) & 0xFF) << 24);
	else
		return pixel;
}


/* local host native format -> X server image format */
static guint32 vnc_base_framebuffer_swap_img_64(VncBaseFramebufferPrivate *priv, guint64 pixel)
{
	if (G_BYTE_ORDER != priv->localFormat->byte_order)
		return  (((pixel >> 56) & 0xFF) <<  0) |
			(((pixel >> 48) & 0xFF) <<  8) |
			(((pixel >> 40) & 0xFF) << 16) |
			(((pixel >> 32) & 0xFF) << 24) |
			(((pixel >> 24) & 0xFF) << 32) |
			(((pixel >> 16) & 0xFF) << 40) |
			(((pixel >>  8) & 0xFF) << 48) |
			(((pixel >>  0) & 0xFF) << 56);
	else
		return pixel;
}


/* VNC server RFB  format ->  local host native format */
static guint32 vnc_base_framebuffer_swap_rfb_32(VncBaseFramebufferPrivate *priv, guint32 pixel)
{
	if (priv->remoteFormat->byte_order != G_BYTE_ORDER)
		return  (((pixel >> 24) & 0xFF) <<  0) |
			(((pixel >> 16) & 0xFF) <<  8) |
			(((pixel >>  8) & 0xFF) << 16) |
			(((pixel >>  0) & 0xFF) << 24);
	else
		return pixel;
}

/* VNC server RFB  format ->  local host native format */
static guint32 vnc_base_framebuffer_swap_rfb_64(VncBaseFramebufferPrivate *priv, guint64 pixel)
{
	if (priv->remoteFormat->byte_order != G_BYTE_ORDER)
		return  (((pixel >> 56) & 0xFF) <<  0) |
			(((pixel >> 48) & 0xFF) <<  8) |
			(((pixel >> 40) & 0xFF) << 16) |
			(((pixel >> 32) & 0xFF) << 24) |
			(((pixel >> 24) & 0xFF) << 32) |
			(((pixel >> 16) & 0xFF) << 40) |
			(((pixel >>  8) & 0xFF) << 48) |
			(((pixel >>  0) & 0xFF) << 56);
	else
		return pixel;
}

#define SRC 8
#define DST 8
#include "vncbaseframebufferblt.h"
#undef SRC
#undef DST

#define SRC 8
#define DST 16
#include "vncbaseframebufferblt.h"
#undef SRC
#undef DST

#define SRC 8
#define DST 32
#include "vncbaseframebufferblt.h"
#undef SRC
#undef DST

#define SRC 8
#define DST 64
#include "vncbaseframebufferblt.h"
#undef SRC
#undef DST


#define SRC 16
#define DST 8
#include "vncbaseframebufferblt.h"
#undef SRC
#undef DST

#define SRC 16
#define DST 16
#include "vncbaseframebufferblt.h"
#undef SRC
#undef DST

#define SRC 16
#define DST 32
#include "vncbaseframebufferblt.h"
#undef SRC
#undef DST

#define SRC 16
#define DST 64
#include "vncbaseframebufferblt.h"
#undef SRC
#undef DST


#define SRC 32
#define DST 8
#include "vncbaseframebufferblt.h"
#undef SRC
#undef DST

#define SRC 32
#define DST 16
#include "vncbaseframebufferblt.h"
#undef SRC
#undef DST

#define SRC 32
#define DST 32
#include "vncbaseframebufferblt.h"
#undef SRC
#undef DST

#define SRC 32
#define DST 64
#include "vncbaseframebufferblt.h"
#undef SRC
#undef DST



#define SRC 64
#define DST 8
#include "vncbaseframebufferblt.h"
#undef SRC
#undef DST

#define SRC 64
#define DST 16
#include "vncbaseframebufferblt.h"
#undef SRC
#undef DST

#define SRC 64
#define DST 32
#include "vncbaseframebufferblt.h"
#undef SRC
#undef DST

#define SRC 64
#define DST 64
#include "vncbaseframebufferblt.h"
#undef SRC
#undef DST


#define COLORMAP
#define SRC 8
#define DST 8
#include "vncbaseframebufferblt.h"
#undef SRC
#undef DST

#define SRC 8
#define DST 16
#include "vncbaseframebufferblt.h"
#undef SRC
#undef DST

#define SRC 8
#define DST 32
#include "vncbaseframebufferblt.h"
#undef SRC
#undef DST

#define SRC 8
#define DST 64
#include "vncbaseframebufferblt.h"
#undef SRC
#undef DST
#undef COLORMAP



#define COLORMAP
#define SRC 16
#define DST 8
#include "vncbaseframebufferblt.h"
#undef SRC
#undef DST

#define SRC 16
#define DST 16
#include "vncbaseframebufferblt.h"
#undef SRC
#undef DST

#define SRC 16
#define DST 32
#include "vncbaseframebufferblt.h"
#undef SRC
#undef DST

#define SRC 16
#define DST 64
#include "vncbaseframebufferblt.h"
#undef SRC
#undef DST
#undef COLORMAP

static vnc_base_framebuffer_set_pixel_at_func *vnc_base_framebuffer_set_pixel_at_table[6][4] = {
        { (vnc_base_framebuffer_set_pixel_at_func *)vnc_base_framebuffer_set_pixel_at_8x8,
          (vnc_base_framebuffer_set_pixel_at_func *)vnc_base_framebuffer_set_pixel_at_8x16,
          (vnc_base_framebuffer_set_pixel_at_func *)vnc_base_framebuffer_set_pixel_at_8x32,
          (vnc_base_framebuffer_set_pixel_at_func *)vnc_base_framebuffer_set_pixel_at_8x64 },
        { (vnc_base_framebuffer_set_pixel_at_func *)vnc_base_framebuffer_set_pixel_at_16x8,
          (vnc_base_framebuffer_set_pixel_at_func *)vnc_base_framebuffer_set_pixel_at_16x16,
          (vnc_base_framebuffer_set_pixel_at_func *)vnc_base_framebuffer_set_pixel_at_16x32,
          (vnc_base_framebuffer_set_pixel_at_func *)vnc_base_framebuffer_set_pixel_at_16x64 },
        { (vnc_base_framebuffer_set_pixel_at_func *)vnc_base_framebuffer_set_pixel_at_32x8,
          (vnc_base_framebuffer_set_pixel_at_func *)vnc_base_framebuffer_set_pixel_at_32x16,
          (vnc_base_framebuffer_set_pixel_at_func *)vnc_base_framebuffer_set_pixel_at_32x32,
          (vnc_base_framebuffer_set_pixel_at_func *)vnc_base_framebuffer_set_pixel_at_32x64 },
        { (vnc_base_framebuffer_set_pixel_at_func *)vnc_base_framebuffer_set_pixel_at_64x8,
          (vnc_base_framebuffer_set_pixel_at_func *)vnc_base_framebuffer_set_pixel_at_64x16,
          (vnc_base_framebuffer_set_pixel_at_func *)vnc_base_framebuffer_set_pixel_at_64x32,
          (vnc_base_framebuffer_set_pixel_at_func *)vnc_base_framebuffer_set_pixel_at_64x64 },
        { (vnc_base_framebuffer_set_pixel_at_func *)vnc_base_framebuffer_set_pixel_at_cmap8x8,
          (vnc_base_framebuffer_set_pixel_at_func *)vnc_base_framebuffer_set_pixel_at_cmap8x16,
          (vnc_base_framebuffer_set_pixel_at_func *)vnc_base_framebuffer_set_pixel_at_cmap8x32,
          (vnc_base_framebuffer_set_pixel_at_func *)vnc_base_framebuffer_set_pixel_at_cmap8x64 },
        { (vnc_base_framebuffer_set_pixel_at_func *)vnc_base_framebuffer_set_pixel_at_cmap16x8,
          (vnc_base_framebuffer_set_pixel_at_func *)vnc_base_framebuffer_set_pixel_at_cmap16x16,
          (vnc_base_framebuffer_set_pixel_at_func *)vnc_base_framebuffer_set_pixel_at_cmap16x32,
          (vnc_base_framebuffer_set_pixel_at_func *)vnc_base_framebuffer_set_pixel_at_cmap16x64 },
};

static vnc_base_framebuffer_fill_func *vnc_base_framebuffer_fill_table[6][4] = {
        { (vnc_base_framebuffer_fill_func *)vnc_base_framebuffer_fill_8x8,
          (vnc_base_framebuffer_fill_func *)vnc_base_framebuffer_fill_8x16,
          (vnc_base_framebuffer_fill_func *)vnc_base_framebuffer_fill_8x32,
          (vnc_base_framebuffer_fill_func *)vnc_base_framebuffer_fill_8x64 },
        { (vnc_base_framebuffer_fill_func *)vnc_base_framebuffer_fill_16x8,
          (vnc_base_framebuffer_fill_func *)vnc_base_framebuffer_fill_16x16,
          (vnc_base_framebuffer_fill_func *)vnc_base_framebuffer_fill_16x32,
          (vnc_base_framebuffer_fill_func *)vnc_base_framebuffer_fill_16x64 },
        { (vnc_base_framebuffer_fill_func *)vnc_base_framebuffer_fill_32x8,
          (vnc_base_framebuffer_fill_func *)vnc_base_framebuffer_fill_32x16,
          (vnc_base_framebuffer_fill_func *)vnc_base_framebuffer_fill_32x32,
          (vnc_base_framebuffer_fill_func *)vnc_base_framebuffer_fill_32x64 },
        { (vnc_base_framebuffer_fill_func *)vnc_base_framebuffer_fill_64x8,
          (vnc_base_framebuffer_fill_func *)vnc_base_framebuffer_fill_64x16,
          (vnc_base_framebuffer_fill_func *)vnc_base_framebuffer_fill_64x32,
          (vnc_base_framebuffer_fill_func *)vnc_base_framebuffer_fill_64x64 },
        { (vnc_base_framebuffer_fill_func *)vnc_base_framebuffer_fill_cmap8x8,
          (vnc_base_framebuffer_fill_func *)vnc_base_framebuffer_fill_cmap8x16,
          (vnc_base_framebuffer_fill_func *)vnc_base_framebuffer_fill_cmap8x32,
          (vnc_base_framebuffer_fill_func *)vnc_base_framebuffer_fill_cmap8x64 },
        { (vnc_base_framebuffer_fill_func *)vnc_base_framebuffer_fill_cmap16x8,
          (vnc_base_framebuffer_fill_func *)vnc_base_framebuffer_fill_cmap16x16,
          (vnc_base_framebuffer_fill_func *)vnc_base_framebuffer_fill_cmap16x32,
          (vnc_base_framebuffer_fill_func *)vnc_base_framebuffer_fill_cmap16x64 },
};

static vnc_base_framebuffer_fill_func *vnc_base_framebuffer_fill_fast_table[4] = {
	(vnc_base_framebuffer_fill_func *)vnc_base_framebuffer_fill_fast_8x8,
	(vnc_base_framebuffer_fill_func *)vnc_base_framebuffer_fill_fast_16x16,
	(vnc_base_framebuffer_fill_func *)vnc_base_framebuffer_fill_fast_32x32,
	(vnc_base_framebuffer_fill_func *)vnc_base_framebuffer_fill_fast_64x64,
};

static vnc_base_framebuffer_blt_func *vnc_base_framebuffer_blt_table[6][4] = {
        {  vnc_base_framebuffer_blt_8x8,
	   vnc_base_framebuffer_blt_8x16,
	   vnc_base_framebuffer_blt_8x32,
	   vnc_base_framebuffer_blt_8x64 },
        { vnc_base_framebuffer_blt_16x8,
	  vnc_base_framebuffer_blt_16x16,
	  vnc_base_framebuffer_blt_16x32,
	  vnc_base_framebuffer_blt_16x64 },
        { vnc_base_framebuffer_blt_32x8,
	  vnc_base_framebuffer_blt_32x16,
	  vnc_base_framebuffer_blt_32x32,
	  vnc_base_framebuffer_blt_32x64 },
        { vnc_base_framebuffer_blt_64x8,
	  vnc_base_framebuffer_blt_64x16,
	  vnc_base_framebuffer_blt_64x32,
	  vnc_base_framebuffer_blt_64x64 },
        { vnc_base_framebuffer_blt_cmap8x8,
	  vnc_base_framebuffer_blt_cmap8x16,
	  vnc_base_framebuffer_blt_cmap8x32,
	  vnc_base_framebuffer_blt_cmap8x64 },
        { vnc_base_framebuffer_blt_cmap16x8,
	  vnc_base_framebuffer_blt_cmap16x16,
	  vnc_base_framebuffer_blt_cmap16x32,
	  vnc_base_framebuffer_blt_cmap16x64 },
};

static vnc_base_framebuffer_rgb24_blt_func *vnc_base_framebuffer_rgb24_blt_table[4] = {
        (vnc_base_framebuffer_rgb24_blt_func *)vnc_base_framebuffer_rgb24_blt_32x8,
        (vnc_base_framebuffer_rgb24_blt_func *)vnc_base_framebuffer_rgb24_blt_32x16,
        (vnc_base_framebuffer_rgb24_blt_func *)vnc_base_framebuffer_rgb24_blt_32x32,
        (vnc_base_framebuffer_rgb24_blt_func *)vnc_base_framebuffer_rgb24_blt_32x64,
};


/* a fast blit for the perfect match scenario */
static void vnc_base_framebuffer_blt_fast(VncBaseFramebufferPrivate *priv,
					  guint8 *src, int rowstride,
					  guint16 x, guint16 y,
					  guint16 width, guint16 height)
{
        guint8 *dst = VNC_BASE_FRAMEBUFFER_AT(priv, x, y);
        guint16 i;
        for (i = 0; i < height; i++) {
                memcpy(dst, src, width * (priv->localFormat->bits_per_pixel / 8));
                dst += priv->rowstride;
                src += rowstride;
        }
}


static void vnc_base_framebuffer_reinit_render_funcs(VncBaseFramebuffer *fb)
{
	VncBaseFramebufferPrivate *priv = fb->priv;
	int i, j, n;
	int depth;

	if (!priv->reinitRenderFuncs)
		return;

	if (!priv->remoteFormat->true_color_flag) {
		priv->remoteFormat->red_max = ~(guint16)0;
		priv->remoteFormat->green_max = ~(guint16)0;
		priv->remoteFormat->blue_max = ~(guint16)0;
		priv->remoteFormat->red_shift = 32;
		priv->remoteFormat->green_shift = 16;
		priv->remoteFormat->blue_shift = 0;
		priv->remoteFormat->byte_order = G_BYTE_ORDER;
	}

	if (priv->remoteFormat->true_color_flag &&
	    priv->localFormat->bits_per_pixel == priv->remoteFormat->bits_per_pixel &&
	    priv->localFormat->red_max == priv->remoteFormat->red_max &&
	    priv->localFormat->green_max == priv->remoteFormat->green_max &&
	    priv->localFormat->blue_max == priv->remoteFormat->blue_max &&
	    priv->localFormat->red_shift == priv->remoteFormat->red_shift &&
	    priv->localFormat->green_shift == priv->remoteFormat->green_shift &&
	    priv->localFormat->blue_shift == priv->remoteFormat->blue_shift &&
	    priv->localFormat->byte_order == G_BYTE_ORDER &&
	    priv->remoteFormat->byte_order == G_BYTE_ORDER)
		priv->perfect_match = TRUE;
	else
		priv->perfect_match = FALSE;

	depth = priv->remoteFormat->depth;
	if (depth == 32)
		depth = 24;

	priv->rm = priv->localFormat->red_max & priv->remoteFormat->red_max;
	priv->gm = priv->localFormat->green_max & priv->remoteFormat->green_max;
	priv->bm = priv->localFormat->blue_max & priv->remoteFormat->blue_max;
	VNC_DEBUG("Mask local: %3d %3d %3d\n"
		   "    remote: %3d %3d %3d\n"
		   "    merged: %3d %3d %3d",
		   priv->localFormat->red_max, priv->localFormat->green_max, priv->localFormat->blue_max,
		   priv->remoteFormat->red_max, priv->remoteFormat->green_max, priv->remoteFormat->blue_max,
		   priv->rm, priv->gm, priv->bm);

	/* Setup shifts assuming matched bpp (but not necessarily match rgb order)*/
	priv->rrs = priv->remoteFormat->red_shift;
	priv->grs = priv->remoteFormat->green_shift;
	priv->brs = priv->remoteFormat->blue_shift;

	priv->rls = priv->localFormat->red_shift;
	priv->gls = priv->localFormat->green_shift;
	priv->bls = priv->localFormat->blue_shift;

	/* This adjusts for remote having more bpp than local */
	for (n = priv->remoteFormat->red_max; n > priv->localFormat->red_max ; n>>= 1)
		priv->rrs++;
	for (n = priv->remoteFormat->green_max; n > priv->localFormat->green_max ; n>>= 1)
		priv->grs++;
	for (n = priv->remoteFormat->blue_max; n > priv->localFormat->blue_max ; n>>= 1)
		priv->brs++;

	/* This adjusts for remote having less bpp than remote */
	for (n = priv->localFormat->red_max ; n > priv->remoteFormat->red_max ; n>>= 1)
		priv->rls++;
	for (n = priv->localFormat->green_max ; n > priv->remoteFormat->green_max ; n>>= 1)
		priv->gls++;
	for (n = priv->localFormat->blue_max ; n > priv->remoteFormat->blue_max ; n>>= 1)
		priv->bls++;
	VNC_DEBUG("Pixel shifts\n   right: %3d %3d %3d\n    left: %3d %3d %3d",
		   priv->rrs, priv->grs, priv->brs,
		   priv->rls, priv->gls, priv->bls);

	i = priv->remoteFormat->bits_per_pixel / 8;
	j = priv->localFormat->bits_per_pixel / 8;
	if (i == 4) i = 3;
	if (j == 4) j = 3;
	if (i > 4) i = 4; /* XXX hardcoding int64 */
	if (j > 4) j = 4; /* XXX hardcoding int64 */
	if (!priv->remoteFormat->true_color_flag) {
		if (priv->remoteFormat->bits_per_pixel == 8)
			i = 5; /* 8bpp colourmap */
		else
			i = 6; /* 16bpp colourmap */
		VNC_DEBUG("BPP i %d %d", priv->remoteFormat->bits_per_pixel, i);
	}

	priv->set_pixel_at = vnc_base_framebuffer_set_pixel_at_table[i - 1][j - 1];

	if (priv->perfect_match)
		priv->fill = vnc_base_framebuffer_fill_fast_table[i - 1];
	else
		priv->fill = vnc_base_framebuffer_fill_table[i - 1][j - 1];

	if (priv->perfect_match)
		priv->blt = vnc_base_framebuffer_blt_fast;
	else
		priv->blt = vnc_base_framebuffer_blt_table[i - 1][j - 1];

	priv->rgb24_blt = vnc_base_framebuffer_rgb24_blt_table[i - 1];

	priv->reinitRenderFuncs = FALSE;
}


static void vnc_base_framebuffer_set_pixel_at(VncFramebuffer *iface,
					      guint8 *src,
					      guint16 x, guint16 y)
{
	VncBaseFramebuffer *fb = VNC_BASE_FRAMEBUFFER(iface);
	VncBaseFramebufferPrivate *priv = fb->priv;

	vnc_base_framebuffer_reinit_render_funcs(fb);

	priv->set_pixel_at(priv, src, x, y);
}


static void vnc_base_framebuffer_fill(VncFramebuffer *iface,
				      guint8 *src,
				      guint16 x, guint16 y,
				      guint16 width, guint16 height)
{
	VncBaseFramebuffer *fb = VNC_BASE_FRAMEBUFFER(iface);
	VncBaseFramebufferPrivate *priv = fb->priv;

	vnc_base_framebuffer_reinit_render_funcs(fb);

	priv->fill(priv, src, x, y, width, height);
}


static void vnc_base_framebuffer_copyrect(VncFramebuffer *iface,
					  guint16 srcx, guint16 srcy,
					  guint16 dstx, guint16 dsty,
					  guint16 width, guint16 height)
{
	VncBaseFramebuffer *fb = VNC_BASE_FRAMEBUFFER(iface);
	VncBaseFramebufferPrivate *priv = fb->priv;
	guint8 *dst, *src;
	int rowstride = priv->rowstride;
	int i;

	vnc_base_framebuffer_reinit_render_funcs(fb);

	if (srcy < dsty) {
		rowstride = -rowstride;
		srcy += (height - 1);
		dsty += (height - 1);
	}

	dst = VNC_BASE_FRAMEBUFFER_AT(priv, dstx, dsty);
	src = VNC_BASE_FRAMEBUFFER_AT(priv, srcx, srcy);
	for (i = 0; i < height; i++) {
		memmove(dst, src, width * (priv->localFormat->bits_per_pixel  / 8));
		dst += rowstride;
		src += rowstride;
	}
}


static void vnc_base_framebuffer_blt(VncFramebuffer *iface,
				     guint8 *src,
				     int rowstride,
				     guint16 x, guint16 y,
				     guint16 width, guint16 height)
{
	VncBaseFramebuffer *fb = VNC_BASE_FRAMEBUFFER(iface);
	VncBaseFramebufferPrivate *priv = fb->priv;

	vnc_base_framebuffer_reinit_render_funcs(fb);

	priv->blt(priv, src, rowstride, x, y, width, height);
}


static void vnc_base_framebuffer_rgb24_blt(VncFramebuffer *iface,
					   guint8 *src,
					   int rowstride,
					   guint16 x, guint16 y,
					   guint16 width, guint16 height)
{
	VncBaseFramebuffer *fb = VNC_BASE_FRAMEBUFFER(iface);
	VncBaseFramebufferPrivate *priv = fb->priv;

	vnc_base_framebuffer_reinit_render_funcs(fb);

	priv->rgb24_blt(priv, src, rowstride, x, y, width, height);
}


static void vnc_base_framebuffer_set_color_map(VncFramebuffer *iface,
					       VncColorMap *map)
{
	VncBaseFramebuffer *fb = VNC_BASE_FRAMEBUFFER(iface);
	VncBaseFramebufferPrivate *priv = fb->priv;

	if (priv->colorMap)
		vnc_color_map_free(priv->colorMap);
	priv->colorMap = vnc_color_map_copy(map);
}


static void vnc_base_framebuffer_interface_init(gpointer g_iface,
						gpointer iface_data G_GNUC_UNUSED)
{
    VncFramebufferInterface *iface = g_iface;

    iface->get_width = vnc_base_framebuffer_get_width;
    iface->get_height = vnc_base_framebuffer_get_height;
    iface->get_rowstride = vnc_base_framebuffer_get_rowstride;
    iface->get_buffer = vnc_base_framebuffer_get_buffer;
    iface->get_local_format = vnc_base_framebuffer_get_local_format;
    iface->get_remote_format = vnc_base_framebuffer_get_remote_format;
    iface->perfect_format_match = vnc_base_framebuffer_perfect_format_match;

    iface->set_pixel_at = vnc_base_framebuffer_set_pixel_at;
    iface->fill = vnc_base_framebuffer_fill;
    iface->copyrect = vnc_base_framebuffer_copyrect;
    iface->blt = vnc_base_framebuffer_blt;
    iface->rgb24_blt = vnc_base_framebuffer_rgb24_blt;
    iface->set_color_map = vnc_base_framebuffer_set_color_map;
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
