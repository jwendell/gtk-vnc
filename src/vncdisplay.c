/*
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  GTK VNC Widget
 */

#include "vncdisplay.h"
#include "coroutine.h"
#include "gvnc.h"

#include <gtk/gtk.h>
#include <string.h>
#include <gdk/gdkkeysyms.h>

#define VNC_DISPLAY_GET_PRIVATE(obj) \
      (G_TYPE_INSTANCE_GET_PRIVATE((obj), VNC_TYPE_DISPLAY, VncDisplayPrivate))

struct _VncDisplayPrivate
{
	GIOChannel *channel;
	GdkGC *gc;
	GdkImage *image;
	GdkCursor *null_cursor;

	struct framebuffer fb;
	struct coroutine coroutine;
	struct gvnc *gvnc;

	int in_grab;

	int button_mask;
	int last_x;
	int last_y;
	const char *password;

	int absolute;
};

GtkWidget *vnc_display_new(void)
{
	return GTK_WIDGET(g_object_new(VNC_TYPE_DISPLAY, NULL));
}

static GdkCursor *create_null_cursor(void)
{
    GdkBitmap *image;
    gchar data[4] = {0};
    GdkColor fg = { 0 };
    GdkCursor *cursor;

    image = gdk_bitmap_create_from_data(NULL, data, 1, 1);

    cursor = gdk_cursor_new_from_pixmap(GDK_PIXMAP(image),
					GDK_PIXMAP(image),
					&fg, &fg, 0, 0);
    gdk_bitmap_unref(image);

    return cursor;
}

static gboolean expose_event(GtkWidget *widget, GdkEventExpose *expose,
			     gpointer data)
{
	VncDisplay *obj = VNC_DISPLAY(widget);
	VncDisplayPrivate *priv = obj->priv;
	int x, y, w, h;

	if (priv->image == NULL)
		return TRUE;

	x = MIN(expose->area.x, priv->fb.width);
	y = MIN(expose->area.y, priv->fb.height);
	w = MIN(expose->area.x + expose->area.width, priv->fb.width);
	h = MIN(expose->area.y + expose->area.height, priv->fb.height);
	w -= x;
	h -= y;

	gdk_draw_image(widget->window,
		       priv->gc, 
		       priv->image,
		       x, y, x, y, w, h);

	return TRUE;
}

static void toggle_grab(VncDisplay *obj)
{
	VncDisplayPrivate *priv = obj->priv;
	VncDisplayClass *klass = VNC_DISPLAY_GET_CLASS(obj);

	if (priv->in_grab) {
		priv->in_grab = 0;
		gdk_keyboard_ungrab(GDK_CURRENT_TIME);
		gdk_pointer_ungrab(GDK_CURRENT_TIME);
		if (priv->absolute)
			gdk_window_set_cursor(GTK_WIDGET(obj)->window,
					      priv->null_cursor);
		else
			gdk_window_set_cursor(GTK_WIDGET(obj)->window,
					      NULL);
		priv->last_x = -1;
		priv->last_y = -1;
		g_signal_emit(obj, klass->leave_grab_event_id, 0);
	} else {
		priv->in_grab = 1;
		gdk_keyboard_grab(GTK_WIDGET(obj)->window,
				  FALSE,
				  GDK_CURRENT_TIME);
		gdk_pointer_grab(GTK_WIDGET(obj)->window,
				 TRUE,
				 GDK_POINTER_MOTION_MASK |
				 GDK_BUTTON_PRESS_MASK |
				 GDK_BUTTON_RELEASE_MASK |
				 GDK_BUTTON_MOTION_MASK |
				 GDK_SCROLL_MASK,
				 NULL,
				 priv->null_cursor,
				 GDK_CURRENT_TIME);
		g_signal_emit(obj, klass->enter_grab_event_id, 0);
	}
}

static gboolean button_event(GtkWidget *widget, GdkEventButton *button,
			     gpointer data)
{
	VncDisplayPrivate *priv = VNC_DISPLAY(widget)->priv;
	int n;

	if (priv->gvnc == NULL)
		return TRUE;

	if (!priv->absolute && !priv->in_grab &&
	    button->button == 1 && button->type == GDK_BUTTON_PRESS)
		toggle_grab(VNC_DISPLAY(widget));

	n = 1 << (button->button - 1);
	if (button->type == GDK_BUTTON_PRESS)
		priv->button_mask |= n;
	else if (button->type == GDK_BUTTON_RELEASE)
		priv->button_mask &= ~n;

	if (priv->absolute) {
		gvnc_pointer_event(priv->gvnc, priv->button_mask,
				   priv->last_x, priv->last_y);
	} else {
		gvnc_pointer_event(priv->gvnc, priv->button_mask,
				   0x7FFF, 0x7FFF);
	}		

	return TRUE;
}

static gboolean scroll_event(GtkWidget *widget, GdkEventScroll *scroll,
			     gpointer data)
{
	VncDisplayPrivate *priv = VNC_DISPLAY(widget)->priv;
	int mask;

	if (priv->gvnc == NULL)
		return TRUE;

	if (scroll->direction == GDK_SCROLL_UP)
		mask = (1 << 3);
	else if (scroll->direction == GDK_SCROLL_DOWN)
		mask = (1 << 4);
	else if (scroll->direction == GDK_SCROLL_LEFT)
		mask = (1 << 5);
	else if (scroll->direction == GDK_SCROLL_RIGHT)
		mask = (1 << 6);
	else
		return TRUE;

	if (priv->absolute) {
		gvnc_pointer_event(priv->gvnc, priv->button_mask | mask,
				   priv->last_x, priv->last_y);
		gvnc_pointer_event(priv->gvnc, priv->button_mask,
				   priv->last_x, priv->last_y);
	} else {
		gvnc_pointer_event(priv->gvnc, priv->button_mask | mask,
				   0x7FFF, 0x7FFF);
		gvnc_pointer_event(priv->gvnc, priv->button_mask,
				   0x7FFF, 0x7FFF);
	}		

	return TRUE;
}

static gboolean motion_event(GtkWidget *widget, GdkEventMotion *motion,
			     gpointer data)
{
	VncDisplayPrivate *priv = VNC_DISPLAY(widget)->priv;
	int dx, dy;

	if (priv->gvnc == NULL)
		return TRUE;

	if (!priv->absolute && !priv->in_grab)
		return TRUE;

	if (priv->in_grab) {
		GdkDrawable *drawable = GDK_DRAWABLE(widget->window);
		GdkDisplay *display = gdk_drawable_get_display(drawable);
		GdkScreen *screen = gdk_drawable_get_screen(drawable);
		int x = (int)motion->x_root;
		int y = (int)motion->y_root;

		if (x == 0) x += 200;
		if (y == 0) y += 200;
		if (x == (gdk_screen_get_width(screen) - 1)) x -= 200;
		if (y == (gdk_screen_get_height(screen) - 1)) y -= 200;

		if (x != (int)motion->x_root || y != (int)motion->y_root) {
			gdk_display_warp_pointer(display, screen, x, y);
			priv->last_x = -1;
			priv->last_y = -1;
			return TRUE;
		}
	}

	if (priv->last_x != -1) {
		if (priv->absolute) {
			dx = (int)motion->x;
			dy = (int)motion->y;
		} else {
			dx = (int)motion->x + 0x7FFF - priv->last_x;
			dy = (int)motion->y + 0x7FFF - priv->last_y;
		}

		gvnc_pointer_event(priv->gvnc, priv->button_mask, dx, dy);
	}

	priv->last_x = (int)motion->x;
	priv->last_y = (int)motion->y;

	return TRUE;
}

static gboolean key_event(GtkWidget *widget, GdkEventKey *key,
			  gpointer data)
{
	VncDisplayPrivate *priv = VNC_DISPLAY(widget)->priv;
	int down;

	if (priv->gvnc == NULL)
		return TRUE;
	
	if (key->type == GDK_KEY_PRESS)
		down = 1;
	else
		down = 0;

	gvnc_key_event(priv->gvnc, down, key->keyval);

	if (key->type == GDK_KEY_PRESS &&
	    ((key->keyval == GDK_Control_L && (key->state & GDK_MOD1_MASK)) ||
	     (key->keyval == GDK_Alt_L && (key->state & GDK_CONTROL_MASK))))
		toggle_grab(VNC_DISPLAY(widget));

	return TRUE;
}

static gboolean on_update(void *opaque, int x, int y, int w, int h)
{
	GtkWidget *obj = GTK_WIDGET(opaque);

	gtk_widget_queue_draw_area(obj, x, y, w, h);

	return TRUE;
}

static gboolean on_resize(void *opaque, int width, int height)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);
	VncDisplayPrivate *priv = obj->priv;
	GdkVisual *visual;
	int depth;

	if (priv->image)
		gdk_image_unref(priv->image);

	if (priv->gc == NULL) {
		priv->null_cursor = create_null_cursor();
		gdk_window_set_cursor(GTK_WIDGET(obj)->window,
				      priv->null_cursor);
		priv->gc = gdk_gc_new(GTK_WIDGET(obj)->window);
	}

	depth = gdk_drawable_get_depth(GTK_WIDGET(obj)->window);
	visual = gdk_visual_get_best_with_depth(depth);
	priv->image = gdk_image_new(GDK_IMAGE_FASTEST, visual, width, height);

	priv->fb.red_mask = visual->red_mask >> visual->red_shift;
	priv->fb.green_mask = visual->green_mask >> visual->green_shift;
	priv->fb.blue_mask = visual->blue_mask >> visual->blue_shift;
	priv->fb.red_shift = visual->red_shift;
	priv->fb.green_shift = visual->green_shift;
	priv->fb.blue_shift = visual->blue_shift;
	priv->fb.depth = priv->image->depth;
	priv->fb.bpp = priv->image->bpp;
	priv->fb.width = priv->image->width;
	priv->fb.height = priv->image->height;
	priv->fb.linesize = priv->image->bpl;
	priv->fb.data = priv->image->mem;

	gtk_widget_set_size_request(GTK_WIDGET(obj), width, height);

	gvnc_set_local(priv->gvnc, &priv->fb);
			
	return TRUE;
}

static gboolean on_pointer_type_change(void *opaque, int absolute)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);
	VncDisplayPrivate *priv = obj->priv;

	if (absolute) {
		if (priv->in_grab)
			toggle_grab(obj);
		gdk_window_set_cursor(GTK_WIDGET(obj)->window, priv->null_cursor);
	} else
		gdk_window_set_cursor(GTK_WIDGET(obj)->window, NULL);

	priv->absolute = absolute;
	return TRUE;
}

static void *vnc_coroutine(void *opaque)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);
	VncDisplayPrivate *priv = obj->priv;
	int32_t encodings[] = { -223, -257, 5, 1, 0 };
	int ret;
	struct vnc_ops ops = {
		.update = on_update,
		.resize = on_resize,
		.pointer_type_change = on_pointer_type_change,
		.user = obj,
	};

	priv->gvnc = gvnc_connect(priv->channel, FALSE, priv->password);
	if (priv->gvnc == NULL)
		return NULL;

	gvnc_set_encodings(priv->gvnc, 5, encodings);

	gvnc_set_vnc_ops(priv->gvnc, &ops);
	gvnc_framebuffer_update_request(priv->gvnc, 0, 0, 0, priv->fb.width, priv->fb.height);

	while (!(ret = gvnc_server_message(priv->gvnc))) {
		gvnc_framebuffer_update_request(priv->gvnc, 1, 0, 0,
						priv->fb.width, priv->fb.height);
	}

	return NULL;
}

static gboolean do_vnc_display_open(gpointer data)
{
	VncDisplay *obj = VNC_DISPLAY(data);
	struct coroutine *co;

	co = &obj->priv->coroutine;

	co->stack_size = 16 << 20;
	co->entry = vnc_coroutine;
	co->release = NULL;

	coroutine_init(co);
	yieldto(co, obj);

	return FALSE;
}

void vnc_display_open(VncDisplay *obj, int fd)
{
	GIOChannel *channel = g_io_channel_unix_new(fd);

	obj->priv->channel = channel;
	g_idle_add(do_vnc_display_open, obj);
}

static void vnc_display_class_init(VncDisplayClass *klass)
{
	g_type_class_add_private(klass, sizeof(VncDisplayPrivate));

	klass->enter_grab_event_id =
		g_signal_new("enter-grab-event",
			     G_TYPE_FROM_CLASS(klass),
			     G_SIGNAL_RUN_LAST | G_SIGNAL_NO_HOOKS,
			     0,
			     NULL,
			     NULL,
			     g_cclosure_marshal_VOID__VOID,
			     G_TYPE_NONE,
			     0);

	klass->leave_grab_event_id =
		g_signal_new("leave-grab-event",
			     G_TYPE_FROM_CLASS(klass),
			     G_SIGNAL_RUN_LAST | G_SIGNAL_NO_HOOKS,
			     0,
			     NULL,
			     NULL,
			     g_cclosure_marshal_VOID__VOID,
			     G_TYPE_NONE,
			     0);
}

static void vnc_display_init(GTypeInstance *instance, gpointer klass)
{
	GtkObject *obj = GTK_OBJECT(instance);
	GtkWidget *widget = GTK_WIDGET(instance);
	VncDisplay *display = VNC_DISPLAY(instance);

	gtk_signal_connect(obj, "expose-event",
			   GTK_SIGNAL_FUNC(expose_event), NULL);
	gtk_signal_connect(obj, "motion-notify-event",
			   GTK_SIGNAL_FUNC(motion_event), NULL);
	gtk_signal_connect(obj, "button-press-event",
			   GTK_SIGNAL_FUNC(button_event), NULL);
	gtk_signal_connect(obj, "button-release-event",
			   GTK_SIGNAL_FUNC(button_event), NULL);
	gtk_signal_connect(obj, "scroll-event",
			   GTK_SIGNAL_FUNC(scroll_event), NULL);
	gtk_signal_connect(obj, "key-press-event",
			   GTK_SIGNAL_FUNC(key_event), NULL);
	gtk_signal_connect(obj, "key-release-event",
			   GTK_SIGNAL_FUNC(key_event), NULL);

	GTK_WIDGET_SET_FLAGS(obj, GTK_CAN_FOCUS);

	gtk_widget_add_events(widget,
			      GDK_POINTER_MOTION_MASK |
			      GDK_BUTTON_PRESS_MASK |
			      GDK_BUTTON_RELEASE_MASK |
			      GDK_BUTTON_MOTION_MASK |
			      GDK_ENTER_NOTIFY_MASK |
			      GDK_LEAVE_NOTIFY_MASK |
			      GDK_SCROLL_MASK);
	gtk_widget_set_double_buffered(widget, FALSE);

	display->priv = VNC_DISPLAY_GET_PRIVATE(display);
	memset(display->priv, 0, sizeof(VncDisplayPrivate));
	display->priv->last_x = -1;
	display->priv->last_y = -1;
	display->priv->absolute = 1;
}

void vnc_display_set_password(VncDisplay *obj, const gchar *password)
{
	obj->priv->password = password;
}

GType vnc_display_get_type(void)
{
	static GType type;

	if (type == 0) {
		static const GTypeInfo info = {
			sizeof(VncDisplayClass),
			NULL,
			NULL,
			(GClassInitFunc)vnc_display_class_init,
			NULL,
			NULL,
			sizeof(VncDisplay),
			0,
			vnc_display_init,
		};

		type = g_type_register_static(GTK_TYPE_DRAWING_AREA,
					      "VncDisplay",
					      &info,
					      0);
	}

	return type;
}

