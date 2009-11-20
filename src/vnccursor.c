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

#include <config.h>

#include "vnccursor.h"

#include <string.h>

#define VNC_CURSOR_GET_PRIVATE(obj) \
      (G_TYPE_INSTANCE_GET_PRIVATE((obj), VNC_TYPE_CURSOR, VncCursorPrivate))

struct _VncCursorPrivate
{
	guint8 *data;
	guint16 hotx;
	guint16 hoty;
	guint16 width;
	guint16 height;
};

G_DEFINE_TYPE(VncCursor, vnc_cursor, G_TYPE_OBJECT)

/* Properties */
enum
{
	PROP_0,
	PROP_DATA,
	PROP_HOTX,
	PROP_HOTY,
	PROP_WIDTH,
	PROP_HEIGHT,
};

static void
vnc_cursor_get_property(GObject    *object,
			guint       prop_id,
			GValue     *value,
			GParamSpec *pspec)
{
	VncCursor *cursor = VNC_CURSOR (object);
	VncCursorPrivate *priv = cursor->priv;

	switch (prop_id) {
	case PROP_DATA:
		g_value_set_pointer(value, priv->data);
		break;

	case PROP_HOTX:
		g_value_set_int(value, priv->hotx);
		break;

	case PROP_HOTY:
		g_value_set_int(value, priv->hoty);
		break;

	case PROP_WIDTH:
		g_value_set_int(value, priv->width);
		break;

	case PROP_HEIGHT:
		g_value_set_int(value, priv->height);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
vnc_cursor_set_property(GObject      *object,
			guint         prop_id,
			const GValue *value,
			GParamSpec   *pspec)
{
	VncCursor *cursor = VNC_CURSOR (object);
	VncCursorPrivate *priv = cursor->priv;

	switch (prop_id) {
	case PROP_DATA:
		if (priv->data)
			g_free(priv->data);
		priv->data = g_value_get_pointer(value);
		break;

	case PROP_HOTX:
		priv->hotx = g_value_get_int(value);
		break;

	case PROP_HOTY:
		priv->hoty = g_value_get_int(value);
		break;

	case PROP_WIDTH:
		priv->width = g_value_get_int(value);
		break;

	case PROP_HEIGHT:
		priv->height = g_value_get_int(value);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

VncCursor *vnc_cursor_new(guint8 *data,
			  guint16 hotx, guint16 hoty,
			  guint16 width, guint16 height)
{
	return VNC_CURSOR(g_object_new(VNC_TYPE_CURSOR,
				       "data", data,
				       "hotx", hotx,
				       "hoty", hoty,
				       "width", width,
				       "height", height,
				       NULL));
}


static void vnc_cursor_finalize (GObject *obj)
{
	VncCursor *cursor = VNC_CURSOR (obj);
	VncCursorPrivate *priv = cursor->priv;

	g_free(priv->data);

	G_OBJECT_CLASS (vnc_cursor_parent_class)->finalize (obj);
}

static void vnc_cursor_class_init(VncCursorClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = vnc_cursor_finalize;
	object_class->get_property = vnc_cursor_get_property;
	object_class->set_property = vnc_cursor_set_property;

	g_object_class_install_property(object_class,
					PROP_DATA,
					g_param_spec_pointer("data",
							     "Cursor pixel data",
							     "Cursor pixel data in RGBA24 format",
							     G_PARAM_READWRITE |
							     G_PARAM_CONSTRUCT |
							     G_PARAM_STATIC_NAME |
							     G_PARAM_STATIC_NICK |
							     G_PARAM_STATIC_BLURB));
	g_object_class_install_property(object_class,
					PROP_HOTX,
					g_param_spec_int("hotx",
							 "Cursor x hotspot",
							 "Cursor x axis hotspot",
							 0, 1 << 15, 0,
							 G_PARAM_READWRITE |
							 G_PARAM_CONSTRUCT |
							 G_PARAM_STATIC_NAME |
							 G_PARAM_STATIC_NICK |
							 G_PARAM_STATIC_BLURB));
	g_object_class_install_property(object_class,
					PROP_HOTY,
					g_param_spec_int("hoty",
							 "Cursor y hotspot",
							 "Cursor y axis hotspot",
							 0, 1 << 15, 0,
							 G_PARAM_READWRITE |
							 G_PARAM_CONSTRUCT |
							 G_PARAM_STATIC_NAME |
							 G_PARAM_STATIC_NICK |
							 G_PARAM_STATIC_BLURB));
	g_object_class_install_property(object_class,
					PROP_WIDTH,
					g_param_spec_int("width",
							 "Cursor width",
							 "Cursor pixel data width",
							 0, 1 << 15, 0,
							 G_PARAM_READWRITE |
							 G_PARAM_CONSTRUCT |
							 G_PARAM_STATIC_NAME |
							 G_PARAM_STATIC_NICK |
							 G_PARAM_STATIC_BLURB));
	g_object_class_install_property(object_class,
					PROP_HEIGHT,
					g_param_spec_int("height",
							 "Cursor height",
							 "Cursor pixel data height",
							 0, 1 << 15, 0,
							 G_PARAM_READWRITE |
							 G_PARAM_CONSTRUCT |
							 G_PARAM_STATIC_NAME |
							 G_PARAM_STATIC_NICK |
							 G_PARAM_STATIC_BLURB));

	g_type_class_add_private(klass, sizeof(VncCursorPrivate));
}

static void vnc_cursor_init(VncCursor *cursor)
{
	VncCursorPrivate *priv;

	priv = cursor->priv = VNC_CURSOR_GET_PRIVATE(cursor);
	memset(priv, 0, sizeof(VncCursorPrivate));
}

const guint8 *vnc_cursor_get_data(VncCursor *cursor)
{
	VncCursorPrivate *priv = cursor->priv;

	return priv->data;
}

guint16 vnc_cursor_get_hotx(VncCursor *cursor)
{
	VncCursorPrivate *priv = cursor->priv;

	return priv->hotx;
}

guint16 vnc_cursor_get_hoty(VncCursor *cursor)
{
	VncCursorPrivate *priv = cursor->priv;

	return priv->hoty;
}


guint16 vnc_cursor_get_width(VncCursor *cursor)
{
	VncCursorPrivate *priv = cursor->priv;

	return priv->width;
}

guint16 vnc_cursor_get_height(VncCursor *cursor)
{
	VncCursorPrivate *priv = cursor->priv;

	return priv->height;
}


/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
