/*
 * GTK VNC Widget
 *
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

#ifndef VNC_CURSOR_H
#define VNC_CURSOR_H

#include <glib-object.h>

#include <vncutil.h>

G_BEGIN_DECLS

#define VNC_TYPE_CURSOR            (vnc_cursor_get_type())
#define VNC_CURSOR(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj), VNC_TYPE_CURSOR, VncCursor))
#define VNC_CURSOR_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass), VNC_TYPE_CURSOR, VncCursorClass))
#define VNC_IS_CURSOR(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj), VNC_TYPE_CURSOR))
#define VNC_IS_CURSOR_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), VNC_TYPE_CURSOR))
#define VNC_CURSOR_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS((obj), VNC_TYPE_CURSOR, VncCursorClass))


typedef struct _VncCursor VncCursor;
typedef struct _VncCursorClass VncCursorClass;
typedef struct _VncCursorPrivate VncCursorPrivate;

struct _VncCursor
{
	GObject parent;

	VncCursorPrivate *priv;

	/* Do not add fields to this struct */
};

struct _VncCursorClass
{
	GObjectClass parent_class;

	/*
	 * If adding fields to this struct, remove corresponding
	 * amount of padding to avoid changing overall struct size
	 */
	gpointer _vnc_reserved[VNC_PADDING];
};


GType vnc_cursor_get_type(void);
VncCursor *vnc_cursor_new(guint8 *rgba24data,
			  guint16 hotx, guint16 hoty,
			  guint16 width, guint16 height);

const guint8 *vnc_cursor_get_data(VncCursor *cursor);

guint16 vnc_cursor_get_hotx(VncCursor *cursor);
guint16 vnc_cursor_get_hoty(VncCursor *cursor);

guint16 vnc_cursor_get_width(VncCursor *cursor);
guint16 vnc_cursor_get_height(VncCursor *cursor);

G_END_DECLS

#endif /* VNC_CURSOR_H */
/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
