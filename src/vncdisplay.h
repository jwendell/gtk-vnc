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

#ifndef VNC_DISPLAY_H
#define VNC_DISPLAY_H

#include <gtk/gtkdrawingarea.h>
#include <glib.h>

G_BEGIN_DECLS

#define VNC_TYPE_DISPLAY            (vnc_display_get_type())
#define VNC_DISPLAY(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj), VNC_TYPE_DISPLAY, VncDisplay))
#define VNC_DISPLAY_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass), VNC_TYPE_DISPLAY, VncDisplayClass))
#define VNC_IS_DISPLAY(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj), VNC_TYPE_DISPLAY))
#define VNC_IS_DISPLAY_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), VNC_TYPE_DISPLAY))
#define VNC_DISPLAY_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS((obj), VNC_TYPE_DISPLAY, VncDisplayClass))


typedef struct _VncDisplay VncDisplay;
typedef struct _VncDisplayClass VncDisplayClass;
typedef struct _VncDisplayPrivate VncDisplayPrivate;

struct _VncDisplay
{
	GtkDrawingArea parent;

	VncDisplayPrivate *priv;
};

struct _VncDisplayClass
{
	GtkDrawingAreaClass parent_class;

	/* Signals */
	void		(* vnc_connected)	(VncDisplay *display);
	void		(* vnc_initialized)	(VncDisplay *display);
	void		(* vnc_disconnected)	(VncDisplay *display);
	void		(* vnc_auth_credential)	(VncDisplay *display, GValueArray *credList);
};

typedef enum
{
	VNC_DISPLAY_CREDENTIAL_PASSWORD,
	VNC_DISPLAY_CREDENTIAL_USERNAME,
	VNC_DISPLAY_CREDENTIAL_CLIENTNAME,
} VncDisplayCredential;

typedef enum
{
	VNC_DISPLAY_KEY_EVENT_PRESS = 1,
	VNC_DISPLAY_KEY_EVENT_RELEASE = 2,
	VNC_DISPLAY_KEY_EVENT_CLICK = 3,
} VncDisplayKeyEvent;

typedef enum
{
	VNC_DISPLAY_DEPTH_COLOR_DEFAULT = 0,
	VNC_DISPLAY_DEPTH_COLOR_FULL,
	VNC_DISPLAY_DEPTH_COLOR_MEDIUM,
	VNC_DISPLAY_DEPTH_COLOR_LOW,
	VNC_DISPLAY_DEPTH_COLOR_ULTRA_LOW
} VncDisplayDepthColor;

GType		vnc_display_get_type(void);
GtkWidget *	vnc_display_new(void);

gboolean	vnc_display_open_fd(VncDisplay *obj, int fd);
gboolean	vnc_display_open_host(VncDisplay *obj, const char *host, const char *port);
gboolean	vnc_display_is_open(VncDisplay *obj);
void		vnc_display_close(VncDisplay *obj);

void            vnc_display_send_keys(VncDisplay *obj, const guint *keyvals, int nkeyvals);
/* FIXME: can we just eliminate the old send_keys interface? */
void            vnc_display_send_keys_ex(VncDisplay *obj, const guint *keyvals,
					 int nkeyvals, VncDisplayKeyEvent kind);

void		vnc_display_send_pointer(VncDisplay *obj, gint x, gint y, int button_mask);

gboolean	vnc_display_set_credential(VncDisplay *obj, int type, const gchar *data);

void		vnc_display_set_pointer_local(VncDisplay *obj, gboolean enable);
gboolean	vnc_display_get_pointer_local(VncDisplay *obj);

void		vnc_display_set_pointer_grab(VncDisplay *obj, gboolean enable);
gboolean	vnc_display_get_pointer_grab(VncDisplay *obj);

void		vnc_display_set_keyboard_grab(VncDisplay *obj, gboolean enable);
gboolean	vnc_display_get_keyboard_grab(VncDisplay *obj);

void		vnc_display_set_read_only(VncDisplay *obj, gboolean enable);
gboolean	vnc_display_get_read_only(VncDisplay *obj);

GdkPixbuf *	vnc_display_get_pixbuf(VncDisplay *obj);

int		vnc_display_get_width(VncDisplay *obj);
int		vnc_display_get_height(VncDisplay *obj);
const char *	vnc_display_get_name(VncDisplay *obj);

void		vnc_display_client_cut_text(VncDisplay *obj, const gchar *text);

void		vnc_display_set_lossy_encoding(VncDisplay *obj, gboolean enable);
gboolean	vnc_display_get_lossy_encoding(VncDisplay *obj);

gboolean	vnc_display_set_scaling(VncDisplay *obj, gboolean enable);
gboolean	vnc_display_get_scaling(VncDisplay *obj);

void		vnc_display_set_force_size(VncDisplay *obj, gboolean enable);
gboolean	vnc_display_get_force_size(VncDisplay *obj);

void		vnc_display_set_shared_flag(VncDisplay *obj, gboolean shared);
gboolean	vnc_display_get_shared_flag(VncDisplay *obj);

void			vnc_display_set_depth(VncDisplay *obj, VncDisplayDepthColor depth);
VncDisplayDepthColor	vnc_display_get_depth(VncDisplay *obj);

void		vnc_display_force_grab(VncDisplay *obj, gboolean enable);

gboolean	vnc_display_is_pointer_absolute(VncDisplay *obj);

GOptionGroup *  vnc_display_get_option_group(void);
const GOptionEntry *  vnc_display_get_option_entries(void);

gboolean	vnc_display_request_update(VncDisplay *obj);

G_END_DECLS

#endif /* VNC_DISPLAY_H */
/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
