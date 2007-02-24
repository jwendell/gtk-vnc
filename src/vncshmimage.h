/*
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  GTK VNC Widget
 */

#ifndef _VNC_SHM_IMAGE_H_
#define _VNC_SHM_IMAGE_H_

typedef struct _VncShmImage VncShmImage;
typedef struct _VncShmImageClass VncShmImageClass;
typedef struct _VncShmImagePrivate VncShmImagePrivate;

#include <glib-object.h>
#include <gdk/gdk.h>

#define VNC_TYPE_SHM_IMAGE (vnc_shm_image_get_type())

#define VNC_SHM_IMAGE(obj) \
        (G_TYPE_CHECK_INSTANCE_CAST((obj), VNC_TYPE_SHM_IMAGE, VncShmImage))

#define VNC_SHM_IMAGE_CLASS(klass) \
        (G_TYPE_CHECK_CLASS_CAST((klass), VNC_TYPE_SHM_IMAGE, VncShmImageClass))

#define VNC_IS_SHM_IMAGE(obj) \
        (G_TYPE_CHECK_INSTANCE_TYPE((obj), VNC_TYPE_SHM_IMAGE))

#define VNC_IS_SHM_IMAGE_CLASS(klass) \
        (G_TYPE_CHECK_CLASS_TYPE((klass), VNC_TYPE_SHM_IMAGE))

#define VNC_SHM_IMAGE_GET_CLASS(obj) \
        (G_TYPE_INSTANCE_GET_CLASS((obj), VNC_TYPE_SHM_IMAGE, VncShmImageClass))

struct _VncShmImage
{
	GObject parent;
	VncShmImagePrivate *priv;

	gint shmid;
	gint depth;
	gpointer *pixels;
	gint bytes_per_line;
	gint bpp;
	gint width;
	gint height;
};

struct _VncShmImageClass
{
	GObjectClass parent;

	gboolean has_xshm;
};

G_BEGIN_DECLS

GType		vnc_shm_image_get_type(void);
VncShmImage *	vnc_shm_image_new(GdkVisual *visual, gint width, gint height, gboolean use_shm);
void		vnc_shm_image_draw(VncShmImage *obj,
				   GdkDrawable *drawable,
				   GdkGC *gc,
				   gint xsrc, gint ysrc,
				   gint xdest, gint ydest,
				   gint width, gint height);

G_END_DECLS

#endif
