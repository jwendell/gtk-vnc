/*
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  GTK VNC Widget
 */

#ifndef _VNC_DISPLAY_H_
#define _VNC_DISPLAY_H_

typedef struct _VncDisplay VncDisplay;
typedef struct _VncDisplayClass VncDisplayClass;
typedef struct _VncDisplayPrivate VncDisplayPrivate;

#include <gtk/gtkdrawingarea.h>
#include <glib.h>

#define VNC_TYPE_DISPLAY (vnc_display_get_type())

#define VNC_DISPLAY(obj) \
        (G_TYPE_CHECK_INSTANCE_CAST((obj), VNC_TYPE_DISPLAY, VncDisplay))

#define VNC_DISPLAY_CLASS(klass) \
        (G_TYPE_CHECK_CLASS_CAST((klass), VNC_TYPE_DISPLAY, VncDisplayClass))

#define VNC_IS_DISPLAY(obj) \
        (G_TYPE_CHECK_INSTANCE_TYPE((obj), VNC_TYPE_DISPLAY))

#define VNC_IS_DISPLAY_CLASS(klass) \
        (G_TYPE_CHECK_CLASS_TYPE((klass), VNC_TYPE_DISPLAY))

#define VNC_DISPLAY_GET_CLASS(obj) \
        (G_TYPE_INSTANCE_GET_CLASS((obj), VNC_TYPE_DISPLAY, VncDisplayClass))

struct _VncDisplay
{
	GtkDrawingArea parent;

	VncDisplayPrivate *priv;
};

struct _VncDisplayClass
{
	GtkDrawingAreaClass parent_class;

	/* Signals */
	void		(* vnc_initialized)	(VncDisplay *display);

	int enter_grab_event_id;
	int leave_grab_event_id;
};

G_BEGIN_DECLS

GType		vnc_display_get_type(void);
GtkWidget *	vnc_display_new(void);

void		vnc_display_open(VncDisplay *obj, int fd);

void		vnc_display_set_password(VncDisplay *obj, const gchar *password);

void		vnc_display_set_use_shm(VncDisplay *obj, gboolean enable);

int		vnc_display_get_width(VncDisplay *obj);
int		vnc_display_get_height(VncDisplay *obj);
const char *	vnc_display_get_host_name(VncDisplay *obj);

G_END_DECLS

#endif
