/*
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  GTK VNC Widget
 */

#include "vncshmimage.h"

#include <string.h>

#include <sys/ipc.h>
#include <sys/shm.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>

#include <gdk/gdkx.h>

#define VNC_SHM_IMAGE_GET_PRIVATE(obj) \
      (G_TYPE_INSTANCE_GET_PRIVATE((obj), VNC_TYPE_SHM_IMAGE, VncShmImagePrivate))

struct _VncShmImagePrivate
{
	XShmSegmentInfo *x_shm_info;
	XImage *ximage;
	GdkImage *image;
	Display *xdisplay;
};

static VncShmImage *vnc_shm_image_initialize(VncShmImage *obj, GdkVisual *visual, gint width, gint height, gboolean use_shm)
{
	VncShmImagePrivate *priv = obj->priv;
	VncShmImageClass *klass = VNC_SHM_IMAGE_GET_CLASS(obj);

	priv->x_shm_info = g_new(XShmSegmentInfo, 1);
	priv->x_shm_info->shmid = -1;
	priv->x_shm_info->shmaddr = (char *)-1;

	priv->xdisplay = gdk_x11_display_get_xdisplay(gdk_display_get_default());

	priv->ximage = NULL;
	priv->x_shm_info->shmid = -1;
	priv->x_shm_info->shmaddr = (char *)-1;

	if (!klass->has_xshm || !use_shm)
		goto error;

	priv->ximage = XShmCreateImage(priv->xdisplay,
				       GDK_VISUAL_XVISUAL(visual),
				       visual->depth,
				       ZPixmap,
				       NULL,
				       priv->x_shm_info,
				       width, height);
	if (priv->ximage == NULL)
		goto error;

	priv->x_shm_info->shmid = shmget(IPC_PRIVATE,
					 priv->ximage->bytes_per_line * priv->ximage->height,
					 IPC_CREAT | 0600);
	if (priv->x_shm_info->shmid == -1)
		goto error;

	priv->x_shm_info->shmaddr = shmat(priv->x_shm_info->shmid, NULL, 0);
	priv->ximage->data = priv->x_shm_info->shmaddr;

	if (priv->x_shm_info->shmaddr == (char *)-1)
		goto error;

	gdk_error_trap_push();

	XShmAttach(priv->xdisplay, priv->x_shm_info);
	XSync(priv->xdisplay, False);

	if (gdk_error_trap_pop())
		goto error;

	obj->shmid = priv->x_shm_info->shmid;
	obj->depth = priv->ximage->depth;
	obj->bpp = priv->ximage->bits_per_pixel / 8;
	obj->pixels = (gpointer)priv->x_shm_info->shmaddr;
	obj->bytes_per_line = priv->ximage->bytes_per_line;
	obj->width = width;
	obj->height = height;

	return obj;

error:
	if (priv->x_shm_info->shmaddr != (char *)-1)
		shmdt(priv->x_shm_info->shmaddr);

	if (priv->x_shm_info->shmid != -1)
		shmctl(priv->x_shm_info->shmid, IPC_RMID, NULL);

	if (priv->ximage)
		XDestroyImage(priv->ximage);

	g_free(priv->x_shm_info);
	priv->x_shm_info = 0;

	priv->image = gdk_image_new(GDK_IMAGE_FASTEST, visual, width, height);

	obj->shmid = -1;
	obj->depth = priv->image->depth;
	obj->bpp = priv->image->bpp;
	obj->pixels = priv->image->mem;
	obj->bytes_per_line = priv->image->bpl;
	obj->width = width;
	obj->height =  height;

	return obj;
}

static void vnc_shm_image_finalize(GObject *obj)
{
	VncShmImagePrivate *priv = VNC_SHM_IMAGE(obj)->priv;

	if (!priv->image) {
		XShmDetach(priv->xdisplay, priv->x_shm_info);
		XDestroyImage(priv->ximage);
		shmdt(priv->x_shm_info->shmaddr);
		g_free(priv->x_shm_info);
	} else
		gdk_image_unref(priv->image);
}

VncShmImage *vnc_shm_image_new(GdkVisual *visual, gint width, gint height, gboolean use_shm)
{
	VncShmImage *obj;

	obj = VNC_SHM_IMAGE(g_object_new(VNC_TYPE_SHM_IMAGE, NULL));
	if (!obj)
		return NULL;

	return vnc_shm_image_initialize(obj, visual, width, height, use_shm);
}

static void vnc_shm_image_class_init(VncShmImageClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
	Bool pixmaps;
	int major, minor;
	Display *xdisplay = gdk_x11_display_get_xdisplay(gdk_display_get_default());

	if (XShmQueryExtension(xdisplay) &&
	    XShmQueryVersion(xdisplay, &major, &minor, &pixmaps))
		klass->has_xshm = TRUE;
	else
		klass->has_xshm = FALSE;

	gobject_class->finalize = vnc_shm_image_finalize;

	g_type_class_add_private(klass, sizeof(VncShmImagePrivate));
}

static void vnc_shm_image_init(GTypeInstance *instance, gpointer klass)
{
	VncShmImage *obj = VNC_SHM_IMAGE(instance);

	obj->priv = VNC_SHM_IMAGE_GET_PRIVATE(obj);
	memset(obj->priv, 0, sizeof(VncShmImagePrivate));
}

void vnc_shm_image_draw(VncShmImage *obj,
			GdkDrawable *drawable,
			GdkGC *gc,
			gint xsrc, gint ysrc,
			gint xdest, gint ydest,
			gint width, gint height)
{
	VncShmImagePrivate *priv = obj->priv;

	if (priv->image)
		gdk_draw_image(drawable,
			       gc,
			       priv->image,
			       xsrc, ysrc,
			       xdest, ydest,
			       width, height);
	else 
		XShmPutImage(GDK_DRAWABLE_XDISPLAY(drawable), 
			     GDK_DRAWABLE_XID(drawable),
			     GDK_GC_XGC(gc),
			     priv->ximage, 
			     xsrc, ysrc, xdest, ydest,
			     width, height, False);
}

GType vnc_shm_image_get_type(void)
{
	static GType type;

	if (type == 0) {
		static const GTypeInfo info = {
			sizeof(VncShmImageClass),
			NULL,
			NULL,
			(GClassInitFunc)vnc_shm_image_class_init,
			NULL,
			NULL,
			sizeof(VncShmImage),
			0,
			vnc_shm_image_init,
		};

		type = g_type_register_static(G_TYPE_OBJECT,
					      "VncShmImage",
					      &info,
					      0);
	}

	return type;
}

