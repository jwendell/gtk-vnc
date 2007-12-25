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
#include "utils.h"
#include "vncmarshal.h"

#include <gtk/gtk.h>
#include <string.h>
#include <stdlib.h>
#include <gdk/gdkkeysyms.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>

#define VNC_DISPLAY_GET_PRIVATE(obj) \
      (G_TYPE_INSTANCE_GET_PRIVATE((obj), VNC_TYPE_DISPLAY, VncDisplayPrivate))

struct _VncDisplayPrivate
{
	int fd;
	char *host;
	char *port;
	GdkGC *gc;
	GdkImage *image;
	GdkCursor *null_cursor;
	GdkCursor *remote_cursor;

	struct gvnc_framebuffer fb;
	struct coroutine coroutine;
	struct gvnc *gvnc;

	gboolean in_pointer_grab;
	gboolean in_keyboard_grab;

	int button_mask;
	int last_x;
	int last_y;

	gboolean absolute;

	gboolean grab_pointer;
	gboolean grab_keyboard;
	gboolean local_pointer;
	gboolean read_only;
};

G_DEFINE_TYPE(VncDisplay, vnc_display, GTK_TYPE_DRAWING_AREA)

/* Signals */
typedef enum
{
 	VNC_POINTER_GRAB,
 	VNC_POINTER_UNGRAB,
 	VNC_KEYBOARD_GRAB,
 	VNC_KEYBOARD_UNGRAB,

	VNC_CONNECTED,
	VNC_INITIALIZED,
	VNC_DISCONNECTED,
 	VNC_AUTH_CREDENTIAL,

	VNC_DESKTOP_RESIZE,

	VNC_AUTH_FAILURE,
	VNC_AUTH_UNSUPPORTED,

	VNC_SERVER_CUT_TEXT,
	VNC_BELL,

	LAST_SIGNAL
} vnc_display_signals;

static guint signals[LAST_SIGNAL] = { 0, 0, 0, 0,
				      0, 0, 0, 0,
				      0, 0, 0, 0, 0,};
static GParamSpec *signalCredParam;

GtkWidget *vnc_display_new(void)
{
	return GTK_WIDGET(g_object_new(VNC_TYPE_DISPLAY, NULL));
}

static GdkCursor *create_null_cursor(void)
{
	GdkBitmap *image;
	gchar data[4] = {0};
	GdkColor fg = { 0, 0, 0, 0 };
	GdkCursor *cursor;

	image = gdk_bitmap_create_from_data(NULL, data, 1, 1);

	cursor = gdk_cursor_new_from_pixmap(GDK_PIXMAP(image),
					    GDK_PIXMAP(image),
					    &fg, &fg, 0, 0);
	gdk_bitmap_unref(image);

	return cursor;
}

static gboolean expose_event(GtkWidget *widget, GdkEventExpose *expose,
			     gpointer data G_GNUC_UNUSED)
{
	VncDisplay *obj = VNC_DISPLAY(widget);
	VncDisplayPrivate *priv = obj->priv;
	int x, y, w, h;
	GdkRectangle drawn;
	GdkRegion *clear, *copy;

	if (priv->image == NULL)
		return TRUE;

	x = MIN(expose->area.x, priv->fb.width);
	y = MIN(expose->area.y, priv->fb.height);
	w = MIN(expose->area.x + expose->area.width, priv->fb.width);
	h = MIN(expose->area.y + expose->area.height, priv->fb.height);
	w -= x;
	h -= y;

	drawn.x = x;
	drawn.y = y;
	drawn.width = w;
	drawn.height = h;

	clear = gdk_region_rectangle(&expose->area);
	copy = gdk_region_rectangle(&drawn);
	gdk_region_subtract(clear, copy);

	gdk_gc_set_clip_region(priv->gc, copy);
	gdk_draw_image(widget->window, priv->gc, priv->image,
		       x, y, x, y, w, h);

	gdk_gc_set_clip_region(priv->gc, clear);
	gdk_draw_rectangle(widget->window, priv->gc, TRUE, expose->area.x, expose->area.y,
			   expose->area.width, expose->area.height);

	gdk_region_destroy(clear);
	gdk_region_destroy(copy);

	return TRUE;
}

static void do_keyboard_grab(VncDisplay *obj, gboolean quiet)
{
	VncDisplayPrivate *priv = obj->priv;

	gdk_keyboard_grab(GTK_WIDGET(obj)->window,
			  FALSE,
			  GDK_CURRENT_TIME);
	priv->in_keyboard_grab = TRUE;
	if (!quiet)
		g_signal_emit(obj, signals[VNC_KEYBOARD_GRAB], 0);
}


static void do_keyboard_ungrab(VncDisplay *obj, gboolean quiet)
{
	VncDisplayPrivate *priv = obj->priv;

	gdk_keyboard_ungrab(GDK_CURRENT_TIME);
	priv->in_keyboard_grab = FALSE;
	if (!quiet)
		g_signal_emit(obj, signals[VNC_KEYBOARD_UNGRAB], 0);
}


static void do_pointer_grab(VncDisplay *obj, gboolean quiet)
{
	VncDisplayPrivate *priv = obj->priv;

	/* If we're not already grabbing keyboard, grab it now */
	if (!priv->grab_keyboard)
		do_keyboard_grab(obj, quiet);

	gdk_pointer_grab(GTK_WIDGET(obj)->window,
			 TRUE,
			 GDK_POINTER_MOTION_MASK |
			 GDK_BUTTON_PRESS_MASK |
			 GDK_BUTTON_RELEASE_MASK |
			 GDK_BUTTON_MOTION_MASK |
			 GDK_SCROLL_MASK,
			 GTK_WIDGET(obj)->window,
			 priv->remote_cursor ? priv->remote_cursor : priv->null_cursor,
			 GDK_CURRENT_TIME);
	priv->in_pointer_grab = TRUE;
	if (!quiet)
		g_signal_emit(obj, signals[VNC_POINTER_GRAB], 0);
}

static void do_pointer_ungrab(VncDisplay *obj, gboolean quiet)
{
	VncDisplayPrivate *priv = obj->priv;

	/* If we grabed keyboard upon pointer grab, then ungrab it now */
	if (!priv->grab_keyboard)
		do_keyboard_ungrab(obj, quiet);

	gdk_pointer_ungrab(GDK_CURRENT_TIME);
	priv->in_pointer_grab = FALSE;
	if (!quiet)
		g_signal_emit(obj, signals[VNC_POINTER_UNGRAB], 0);
}

static void do_pointer_hide(VncDisplay *obj)
{
	VncDisplayPrivate *priv = obj->priv;
	gdk_window_set_cursor(GTK_WIDGET(obj)->window,
			      priv->remote_cursor ? priv->remote_cursor : priv->null_cursor);
}

static void do_pointer_show(VncDisplay *obj)
{
	VncDisplayPrivate *priv = obj->priv;
	gdk_window_set_cursor(GTK_WIDGET(obj)->window,
			      priv->remote_cursor);
}


static gboolean button_event(GtkWidget *widget, GdkEventButton *button,
			     gpointer data G_GNUC_UNUSED)
{
	VncDisplayPrivate *priv = VNC_DISPLAY(widget)->priv;
	int n;

	if (priv->gvnc == NULL || !gvnc_is_initialized(priv->gvnc))
		return FALSE;

	if (priv->read_only)
		return FALSE;

	if ((priv->grab_pointer || !priv->absolute) &&
	    !priv->in_pointer_grab &&
	    button->button == 1 && button->type == GDK_BUTTON_PRESS)
		do_pointer_grab(VNC_DISPLAY(widget), FALSE);

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

	return FALSE;
}

static gboolean scroll_event(GtkWidget *widget, GdkEventScroll *scroll,
			     gpointer data G_GNUC_UNUSED)
{
	VncDisplayPrivate *priv = VNC_DISPLAY(widget)->priv;
	int mask;

	if (priv->gvnc == NULL || !gvnc_is_initialized(priv->gvnc))
		return FALSE;

	if (priv->read_only)
		return FALSE;

	if (scroll->direction == GDK_SCROLL_UP)
		mask = (1 << 3);
	else if (scroll->direction == GDK_SCROLL_DOWN)
		mask = (1 << 4);
	else if (scroll->direction == GDK_SCROLL_LEFT)
		mask = (1 << 5);
	else if (scroll->direction == GDK_SCROLL_RIGHT)
		mask = (1 << 6);
	else
		return FALSE;

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

	return FALSE;
}

static gboolean motion_event(GtkWidget *widget, GdkEventMotion *motion,
			     gpointer data G_GNUC_UNUSED)
{
	VncDisplayPrivate *priv = VNC_DISPLAY(widget)->priv;
	int dx, dy;

	if (priv->gvnc == NULL || !gvnc_is_initialized(priv->gvnc))
		return FALSE;

	if (!priv->absolute && !priv->in_pointer_grab)
		return FALSE;

	if (priv->read_only)
		return FALSE;

	if (!priv->absolute && priv->in_pointer_grab) {
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
			return FALSE;
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

	return FALSE;
}

static gboolean key_event(GtkWidget *widget, GdkEventKey *key,
			  gpointer data G_GNUC_UNUSED)
{
	VncDisplayPrivate *priv = VNC_DISPLAY(widget)->priv;
	guint keyval;
	gint group, level;
	GdkModifierType consumed;

	if (priv->gvnc == NULL || !gvnc_is_initialized(priv->gvnc))
		return TRUE;

	if (priv->read_only)
		return FALSE;

	/*
	 * Key handling in VNC is screwy. The event.keyval from GTK is
	 * interpreted relative to modifier state. This really messes
	 * up with VNC which has no concept of modifiers - it just sees
	 * key up & down events - the remote end interprets modifiers
	 * itself. So if we interpret at the client end you can end up
	 * with 'Alt' key press generating Alt_L, and key release generating
	 * ISO_Prev_Group. This really really confuses the VNC server
	 * with 'Alt' getting stuck on.
	 *
	 * So we have to redo GTK's  keycode -> keyval translation
	 * using only the SHIFT modifier which the RFB explicitly
	 * requires to be interpreted at client end.
	 *
	 * Arggggh.
	 */
	gdk_keymap_translate_keyboard_state(gdk_keymap_get_default(),
					    key->hardware_keycode,
					    key->state & (GDK_SHIFT_MASK | GDK_LOCK_MASK),
					    key->group,
					    &keyval,
					    &group,
					    &level,
					    &consumed);

	gvnc_key_event(priv->gvnc, key->type == GDK_KEY_PRESS ? 1 : 0, keyval);

	if (key->type == GDK_KEY_PRESS &&
	    ((keyval == GDK_Control_L && (key->state & GDK_MOD1_MASK)) ||
	     (keyval == GDK_Alt_L && (key->state & GDK_CONTROL_MASK)))) {
		if (priv->in_pointer_grab)
			do_pointer_ungrab(VNC_DISPLAY(widget), FALSE);
		else
			do_pointer_grab(VNC_DISPLAY(widget), FALSE);
	}

	return TRUE;
}

static gboolean enter_event(GtkWidget *widget, GdkEventCrossing *crossing,
                            gpointer data G_GNUC_UNUSED)
{
        VncDisplayPrivate *priv = VNC_DISPLAY(widget)->priv;

        if (priv->gvnc == NULL || !gvnc_is_initialized(priv->gvnc))
                return TRUE;

        if (crossing->mode != GDK_CROSSING_NORMAL)
                return TRUE;

        if (priv->grab_keyboard)
                do_keyboard_grab(VNC_DISPLAY(widget), FALSE);

        return TRUE;
}

static gboolean leave_event(GtkWidget *widget, GdkEventCrossing *crossing,
                            gpointer data G_GNUC_UNUSED)
{
        VncDisplayPrivate *priv = VNC_DISPLAY(widget)->priv;

        if (priv->gvnc == NULL || !gvnc_is_initialized(priv->gvnc))
                return TRUE;

        if (crossing->mode != GDK_CROSSING_NORMAL)
                return TRUE;

        if (priv->grab_keyboard)
                do_keyboard_ungrab(VNC_DISPLAY(widget), FALSE);

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

	if (priv->gvnc == NULL || !gvnc_is_initialized(priv->gvnc))
		return TRUE;

	if (priv->image)
		g_object_unref(priv->image);

	if (priv->gc == NULL) {
		priv->null_cursor = create_null_cursor();
		if (priv->local_pointer)
			do_pointer_show(obj);
		else
			do_pointer_hide(obj);
		priv->gc = gdk_gc_new(GTK_WIDGET(obj)->window);
	}

	visual = gdk_drawable_get_visual(GTK_WIDGET(obj)->window);
	
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
	priv->fb.data = (uint8_t *)priv->image->mem;

	gtk_widget_set_size_request(GTK_WIDGET(obj), width, height);

	gvnc_set_local(priv->gvnc, &priv->fb);

	g_signal_emit (G_OBJECT (obj),
		       signals[VNC_DESKTOP_RESIZE],
		       0,
		       width, height);

	return TRUE;
}

static gboolean on_pointer_type_change(void *opaque, int absolute)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);
	VncDisplayPrivate *priv = obj->priv;

	if (absolute && priv->in_pointer_grab && !priv->grab_pointer)
		do_pointer_ungrab(obj, FALSE);

	priv->absolute = absolute;
	return TRUE;
}

static gboolean on_auth_cred(void *opaque)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);
	GValueArray *credList;
	GValue username, password, clientname;

	memset(&username, 0, sizeof(username));
	memset(&password, 0, sizeof(password));
	memset(&clientname, 0, sizeof(clientname));

	credList = g_value_array_new(2);
	if (gvnc_wants_credential_username(obj->priv->gvnc)) {
		g_value_init(&username, G_PARAM_SPEC_VALUE_TYPE(signalCredParam));
		g_value_set_enum(&username, VNC_DISPLAY_CREDENTIAL_USERNAME);
		g_value_array_append(credList, &username);
	}
	if (gvnc_wants_credential_password(obj->priv->gvnc)) {
		g_value_init(&password, G_PARAM_SPEC_VALUE_TYPE(signalCredParam));
		g_value_set_enum(&password, VNC_DISPLAY_CREDENTIAL_PASSWORD);
		g_value_array_append(credList, &password);
	}
	if (gvnc_wants_credential_x509(obj->priv->gvnc)) {
		g_value_init(&clientname, G_PARAM_SPEC_VALUE_TYPE(signalCredParam));
		g_value_set_enum(&clientname, VNC_DISPLAY_CREDENTIAL_CLIENTNAME);
		g_value_array_append(credList, &clientname);
	}

	g_signal_emit (G_OBJECT (obj),
		       signals[VNC_AUTH_CREDENTIAL],
		       0,
		       credList);

	g_value_array_free(credList);

	return TRUE;
}

static gboolean on_auth_type(void *opaque, unsigned int ntype, unsigned int *types)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);
	VncDisplayPrivate *priv = obj->priv;

	/*
	 * XXX lame - we should have some prioritization. That
	 * said most servers only support 1 auth type at any time
	 */
	if (ntype)
		gvnc_set_auth_type(priv->gvnc, types[0]);

	return TRUE;
}

static gboolean on_auth_subtype(void *opaque, unsigned int ntype, unsigned int *types)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);
	VncDisplayPrivate *priv = obj->priv;

	/*
	 * XXX lame - we should have some prioritization. That
	 * said most servers only support 1 auth type at any time
	 */
	if (ntype)
		gvnc_set_auth_subtype(priv->gvnc, types[0]);

	return TRUE;
}

static gboolean on_auth_failure(void *opaque, const char *msg)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);

	g_signal_emit (G_OBJECT (obj),
		       signals[VNC_AUTH_FAILURE],
		       0,
		       msg);

	return TRUE;
}

static gboolean on_auth_unsupported(void *opaque, unsigned int auth_type)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);

	g_signal_emit (G_OBJECT (obj),
		       signals[VNC_AUTH_UNSUPPORTED],
		       0,
		       auth_type);

	return TRUE;
}

static gboolean on_server_cut_text(void *opaque, const void* text, size_t len)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);
	GString *str = g_string_new_len ((const gchar *)text, len);

	g_signal_emit (G_OBJECT (obj),
		       signals[VNC_SERVER_CUT_TEXT],
		       0,
		       str->str);

	g_string_free (str, TRUE);
	return TRUE;
}

static gboolean on_bell(void *opaque)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);

	g_signal_emit (G_OBJECT (obj),
		       signals[VNC_BELL],
		       0);

	return TRUE;
}

static gboolean on_local_cursor(void *opaque, int x, int y, int width, int height, uint8_t *image)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);
	VncDisplayPrivate *priv = obj->priv;

	if (priv->remote_cursor) {
		gdk_cursor_unref(priv->remote_cursor);
		priv->remote_cursor = NULL;
	}

	if (width && height) {
		GdkDisplay *display = gdk_drawable_get_display(GDK_DRAWABLE(GTK_WIDGET(obj)->window));
		GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(image, GDK_COLORSPACE_RGB,
							     TRUE, 8, width, height,
							     width * 4, NULL, NULL);
		priv->remote_cursor = gdk_cursor_new_from_pixbuf(display,
								 pixbuf,
								 x, y);
		gdk_pixbuf_unref(pixbuf);
	}

	if (priv->in_pointer_grab) {
		do_pointer_ungrab(obj, TRUE);
		do_pointer_grab(obj, TRUE);
	} else {
		do_pointer_hide(obj);
	}

	return TRUE;
}

static const struct gvnc_ops vnc_display_ops = {
	.auth_cred = on_auth_cred,
	.auth_type = on_auth_type,
	.auth_subtype = on_auth_subtype,
	.auth_failure = on_auth_failure,
	.update = on_update,
	.resize = on_resize,
	.pointer_type_change = on_pointer_type_change,
	.local_cursor = on_local_cursor,
	.auth_unsupported = on_auth_unsupported,
	.server_cut_text = on_server_cut_text,
	.bell = on_bell
};

static void *vnc_coroutine(void *opaque)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);
	VncDisplayPrivate *priv = obj->priv;
	int32_t encodings[] = { GVNC_ENCODING_DESKTOP_RESIZE,
				GVNC_ENCODING_RICH_CURSOR,
				GVNC_ENCODING_XCURSOR,
				GVNC_ENCODING_POINTER_CHANGE,
				GVNC_ENCODING_HEXTILE,
				GVNC_ENCODING_RRE,
				GVNC_ENCODING_COPY_RECT,
				GVNC_ENCODING_RAW };

	int ret;

	if (priv->gvnc == NULL || gvnc_is_open(priv->gvnc)) {
		g_object_unref(G_OBJECT(obj));
		return NULL;
	}

	GVNC_DEBUG("Started background coroutine\n");
	if (priv->fd != -1) {
		if (!gvnc_open_fd(priv->gvnc, priv->fd))
			goto cleanup;
	} else {
		if (!gvnc_open_host(priv->gvnc, priv->host, priv->port))
			goto cleanup;
	}

	g_signal_emit (G_OBJECT (obj),
		       signals[VNC_CONNECTED],
		       0);

	GVNC_DEBUG("Protocol initialization\n");
	if (!gvnc_initialize(priv->gvnc, FALSE))
		goto cleanup;

	g_signal_emit (G_OBJECT (obj),
		       signals[VNC_INITIALIZED],
		       0);

	if (!gvnc_set_encodings(priv->gvnc, ARRAY_SIZE(encodings), encodings))
		goto cleanup;

	if (!gvnc_framebuffer_update_request(priv->gvnc, 0, 0, 0, priv->fb.width, priv->fb.height))
		goto cleanup;

	GVNC_DEBUG("Running main loop\n");
	while ((ret = gvnc_server_message(priv->gvnc))) {
		if (!gvnc_framebuffer_update_request(priv->gvnc, 1, 0, 0,
						     priv->fb.width, priv->fb.height))
			goto cleanup;
	}

 cleanup:
	GVNC_DEBUG("Doing final VNC cleanup\n");
	gvnc_close(priv->gvnc);
	g_signal_emit (G_OBJECT (obj),
		       signals[VNC_DISCONNECTED],
		       0);
	g_object_unref(G_OBJECT(obj));
	/* Co-routine exits now - the VncDisplay object may no longer exist,
	   so don't do anything else now unless you like SEGVs */
	return NULL;
}

static gboolean do_vnc_display_open(gpointer data)
{
	VncDisplay *obj = VNC_DISPLAY(data);
	struct coroutine *co;

	if (obj->priv->gvnc == NULL || gvnc_is_open(obj->priv->gvnc)) {
		g_object_unref(G_OBJECT(obj));
		return FALSE;
	}

	co = &obj->priv->coroutine;

	co->stack_size = 16 << 20;
	co->entry = vnc_coroutine;
	co->release = NULL;

	coroutine_init(co);
	coroutine_yieldto(co, obj);

	return FALSE;
}

gboolean vnc_display_open_fd(VncDisplay *obj, int fd)
{
	if (obj->priv->gvnc == NULL || gvnc_is_open(obj->priv->gvnc))
		return FALSE;

	obj->priv->fd = fd;
	obj->priv->host = NULL;
	obj->priv->port = NULL;

	g_object_ref(G_OBJECT(obj)); /* Unref'd when co-routine exits */
	g_idle_add(do_vnc_display_open, obj);

	return TRUE;
}

gboolean vnc_display_open_host(VncDisplay *obj, const char *host, const char *port)
{
	if (obj->priv->gvnc == NULL || gvnc_is_open(obj->priv->gvnc))
		return FALSE;

	obj->priv->host = strdup(host);
	if (!obj->priv->host) {
		return FALSE;
	}
	obj->priv->port = strdup(port);
	if (!obj->priv->port) {
		free(obj->priv->host);
		obj->priv->host = NULL;
		return FALSE;
	}

	g_object_ref(G_OBJECT(obj)); /* Unref'd when co-routine exits */
	g_idle_add(do_vnc_display_open, obj);
	return TRUE;
}

gboolean vnc_display_is_open(VncDisplay *obj)
{
	if (obj->priv->gvnc == NULL)
		return FALSE;
	return gvnc_is_open(obj->priv->gvnc);
}

void vnc_display_close(VncDisplay *obj)
{
	if (obj->priv->gvnc == NULL)
		return;

	if (gvnc_is_open(obj->priv->gvnc)) {
		GVNC_DEBUG("Requesting graceful shutdown of connection\n");
		gvnc_shutdown(obj->priv->gvnc);
	}
}


void vnc_display_send_keys(VncDisplay *obj, const guint *keyvals, int nkeyvals)
{
	vnc_display_send_keys_ex(obj, keyvals,
				 nkeyvals, VNC_DISPLAY_KEY_EVENT_CLICK);
}

void vnc_display_send_keys_ex(VncDisplay *obj, const guint *keyvals,
			      int nkeyvals, VncDisplayKeyEvent kind)
{
	int i;
	if (obj->priv->gvnc == NULL || !gvnc_is_open(obj->priv->gvnc))
		return;

	if (kind & VNC_DISPLAY_KEY_EVENT_PRESS) {
		for (i = 0 ; i < nkeyvals ; i++)
			gvnc_key_event(obj->priv->gvnc, 1, keyvals[i]);
	}

	if (kind & VNC_DISPLAY_KEY_EVENT_RELEASE) {
		for (i = (nkeyvals-1) ; i >= 0 ; i--)
			gvnc_key_event(obj->priv->gvnc, 0, keyvals[i]);
	}
}

void vnc_display_send_pointer(VncDisplay *obj, gint x, gint y, int button_mask)
{
	VncDisplayPrivate *priv = obj->priv;

	if (priv->gvnc == NULL || !gvnc_is_open(obj->priv->gvnc))
		return;

	if (priv->absolute) {
		priv->button_mask = button_mask;
		priv->last_x = x;
		priv->last_y = y;
		gvnc_pointer_event(priv->gvnc, priv->button_mask, x, y);
	}
}

static void vnc_display_destroy (GtkObject *obj)
{
	VncDisplay *display = VNC_DISPLAY (obj);
	GVNC_DEBUG("Requesting that VNC close\n");
	vnc_display_close(display);
	GTK_OBJECT_CLASS (vnc_display_parent_class)->destroy (obj);
}


static void vnc_display_finalize (GObject *obj)
{
	VncDisplay *display = VNC_DISPLAY (obj);
	GVNC_DEBUG("Releasing VNC widget\n");
	if (gvnc_is_open(display->priv->gvnc)) {
		g_warning("VNC widget finalized before the connection finished shutting down");
	}
	gvnc_free(display->priv->gvnc);
	display->priv->gvnc = NULL;
	G_OBJECT_CLASS (vnc_display_parent_class)->finalize (obj);
}

static void vnc_display_class_init(VncDisplayClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GtkObjectClass *gtkobject_class = GTK_OBJECT_CLASS (klass);

	object_class->finalize = vnc_display_finalize;
	gtkobject_class->destroy = vnc_display_destroy;

	signalCredParam = g_param_spec_enum("credential",
					    "credential",
					    "credential",
					    vnc_display_credential_get_type(),
					    0,
					    G_PARAM_READABLE);

	signals[VNC_CONNECTED] =
		g_signal_new ("vnc-connected",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (VncDisplayClass, vnc_connected),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE,
			      0);

	signals[VNC_INITIALIZED] =
		g_signal_new ("vnc-initialized",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (VncDisplayClass, vnc_initialized),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE,
			      0);

	signals[VNC_DISCONNECTED] =
		g_signal_new ("vnc-disconnected",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (VncDisplayClass, vnc_disconnected),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE,
			      0);

	signals[VNC_AUTH_CREDENTIAL] =
		g_signal_new ("vnc-auth-credential",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (VncDisplayClass, vnc_auth_credential),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__PARAM,
			      G_TYPE_NONE,
			      1,
			      G_TYPE_VALUE_ARRAY);


	signals[VNC_POINTER_GRAB] =
		g_signal_new("vnc-pointer-grab",
			     G_TYPE_FROM_CLASS(klass),
			     G_SIGNAL_RUN_LAST | G_SIGNAL_NO_HOOKS,
			     0,
			     NULL,
			     NULL,
			     g_cclosure_marshal_VOID__VOID,
			     G_TYPE_NONE,
			     0);

	signals[VNC_POINTER_UNGRAB] =
		g_signal_new("vnc-pointer-ungrab",
			     G_TYPE_FROM_CLASS(klass),
			     G_SIGNAL_RUN_LAST | G_SIGNAL_NO_HOOKS,
			     0,
			     NULL,
			     NULL,
			     g_cclosure_marshal_VOID__VOID,
			     G_TYPE_NONE,
			     0);

	signals[VNC_KEYBOARD_GRAB] =
		g_signal_new("vnc-keyboard-grab",
			     G_TYPE_FROM_CLASS(klass),
			     G_SIGNAL_RUN_LAST | G_SIGNAL_NO_HOOKS,
			     0,
			     NULL,
			     NULL,
			     g_cclosure_marshal_VOID__VOID,
			     G_TYPE_NONE,
			     0);

	signals[VNC_KEYBOARD_UNGRAB] =
		g_signal_new("vnc-keyboard-ungrab",
			     G_TYPE_FROM_CLASS(klass),
			     G_SIGNAL_RUN_LAST | G_SIGNAL_NO_HOOKS,
			     0,
			     NULL,
			     NULL,
			     g_cclosure_marshal_VOID__VOID,
			     G_TYPE_NONE,
			     0);


	signals[VNC_DESKTOP_RESIZE] =
		g_signal_new("vnc-desktop-resize",
			     G_TYPE_FROM_CLASS(klass),
			     G_SIGNAL_RUN_LAST | G_SIGNAL_NO_HOOKS,
			     0,
			     NULL,
			     NULL,
			     g_cclosure_user_marshal_VOID__INT_INT,
			     G_TYPE_NONE,
			     2,
			     G_TYPE_INT, G_TYPE_INT);

	signals[VNC_AUTH_FAILURE] =
		g_signal_new("vnc-auth-failure",
			     G_TYPE_FROM_CLASS(klass),
			     G_SIGNAL_RUN_LAST | G_SIGNAL_NO_HOOKS,
			     0,
			     NULL,
			     NULL,
			     g_cclosure_marshal_VOID__STRING,
			     G_TYPE_NONE,
			     1,
			     G_TYPE_STRING);

	signals[VNC_AUTH_UNSUPPORTED] =
		g_signal_new("vnc-auth-unsupported",
			     G_TYPE_FROM_CLASS(klass),
			     G_SIGNAL_RUN_LAST | G_SIGNAL_NO_HOOKS,
			     0,
			     NULL,
			     NULL,
			     g_cclosure_marshal_VOID__UINT,
			     G_TYPE_NONE,
			     1,
			     G_TYPE_UINT);

	signals[VNC_SERVER_CUT_TEXT] =
		g_signal_new("vnc-server-cut-text",
			     G_TYPE_FROM_CLASS(klass),
			     G_SIGNAL_RUN_LAST | G_SIGNAL_NO_HOOKS,
			     0,
			     NULL,
			     NULL,
			     g_cclosure_marshal_VOID__STRING,
			     G_TYPE_NONE,
			     1,
			     G_TYPE_STRING);

	signals[VNC_BELL] =
		g_signal_new("vnc-bell",
			     G_TYPE_FROM_CLASS(klass),
			     G_SIGNAL_RUN_LAST | G_SIGNAL_NO_HOOKS,
			     0,
			     NULL,
			     NULL,
			     g_cclosure_marshal_VOID__VOID,
			     G_TYPE_NONE,
			     0);

	g_type_class_add_private(klass, sizeof(VncDisplayPrivate));
}

static void vnc_display_init(VncDisplay *display)
{
	GtkObject *obj = GTK_OBJECT(display);
	GtkWidget *widget = GTK_WIDGET(display);

	g_signal_connect(obj, "expose-event",
			 G_CALLBACK(expose_event), NULL);
	g_signal_connect(obj, "motion-notify-event",
			 G_CALLBACK(motion_event), NULL);
	g_signal_connect(obj, "button-press-event",
			 G_CALLBACK(button_event), NULL);
	g_signal_connect(obj, "button-release-event",
			 G_CALLBACK(button_event), NULL);
	g_signal_connect(obj, "scroll-event",
			 G_CALLBACK(scroll_event), NULL);
	g_signal_connect(obj, "key-press-event",
			 G_CALLBACK(key_event), NULL);
	g_signal_connect(obj, "key-release-event",
			 G_CALLBACK(key_event), NULL);
	g_signal_connect(obj, "enter-notify-event",
			 G_CALLBACK(enter_event), NULL);
	g_signal_connect(obj, "leave-notify-event",
			 G_CALLBACK(leave_event), NULL);

	GTK_WIDGET_SET_FLAGS(obj, GTK_CAN_FOCUS);

	gtk_widget_add_events(widget,
			      GDK_POINTER_MOTION_MASK |
			      GDK_BUTTON_PRESS_MASK |
			      GDK_BUTTON_RELEASE_MASK |
			      GDK_BUTTON_MOTION_MASK |
			      GDK_ENTER_NOTIFY_MASK |
			      GDK_LEAVE_NOTIFY_MASK |
			      GDK_SCROLL_MASK |
			      GDK_KEY_PRESS_MASK);
	gtk_widget_set_double_buffered(widget, FALSE);

	display->priv = VNC_DISPLAY_GET_PRIVATE(display);
	memset(display->priv, 0, sizeof(VncDisplayPrivate));
	display->priv->last_x = -1;
	display->priv->last_y = -1;
	display->priv->absolute = 1;
	display->priv->fd = -1;

	display->priv->gvnc = gvnc_new(&vnc_display_ops, obj);
}

static int vnc_display_best_path(char *buf,
				 int buflen,
				 const char *basedir,
				 const char *basefile,
				 char **dirs,
				 unsigned int ndirs)
{
	unsigned int i;
	for (i = 0 ; i < ndirs ; i++) {
		struct stat sb;
		snprintf(buf, buflen-1, "%s/%s/%s", dirs[i], basedir, basefile);
		buf[buflen-1] = '\0';
		if (stat(buf, &sb) == 0)
			return 0;
	}
	return -1;
}

static int vnc_display_set_x509_credential(VncDisplay *obj, const char *name)
{
	char sysdir[PATH_MAX], userdir[PATH_MAX];
	struct passwd *pw;
	char file[PATH_MAX];
	char *dirs[] = { sysdir, userdir };

	strncpy(sysdir, SYSCONFDIR "/pki", PATH_MAX-1);
	sysdir[PATH_MAX-1] = '\0';

	if (!(pw = getpwuid(getuid())))
		return TRUE;

	snprintf(userdir, PATH_MAX-1, "%s/.pki", pw->pw_dir);
	userdir[PATH_MAX-1] = '\0';

	if (vnc_display_best_path(file, PATH_MAX, "CA", "cacert.pem", dirs, 2) < 0)
		return TRUE;
	gvnc_set_credential_x509_cacert(obj->priv->gvnc, file);

	/* Don't mind failures of CRL */
	if (vnc_display_best_path(file, PATH_MAX, "CA", "cacrl.pem", dirs, 2) == 0)
		gvnc_set_credential_x509_cacert(obj->priv->gvnc, file);

	/* Set client key & cert if we have them. Server will reject auth
	 * if it decides it requires them*/
	if (vnc_display_best_path(file, PATH_MAX, name, "private/clientkey.pem", dirs, 2) == 0)
		gvnc_set_credential_x509_key(obj->priv->gvnc, file);
	if (vnc_display_best_path(file, PATH_MAX, name, "clientcert.pem", dirs, 2) == 0)
		gvnc_set_credential_x509_cert(obj->priv->gvnc, file);

	return FALSE;
}

gboolean vnc_display_set_credential(VncDisplay *obj, int type, const gchar *data)
{
	switch (type) {
	case VNC_DISPLAY_CREDENTIAL_PASSWORD:
		if (gvnc_set_credential_password(obj->priv->gvnc, data))
			return FALSE;
		return TRUE;

	case VNC_DISPLAY_CREDENTIAL_USERNAME:
		if (gvnc_set_credential_username(obj->priv->gvnc, data))
			return FALSE;
		return TRUE;

	case VNC_DISPLAY_CREDENTIAL_CLIENTNAME:
		return vnc_display_set_x509_credential(obj, data);
	}

	return FALSE;
}

void vnc_display_set_pointer_local(VncDisplay *obj, gboolean enable)
{
	if (obj->priv->gc) {
		if (enable)
			do_pointer_show(obj);
		else
			do_pointer_hide(obj);
	}
	obj->priv->local_pointer = enable;
}

void vnc_display_set_pointer_grab(VncDisplay *obj, gboolean enable)
{
	VncDisplayPrivate *priv = obj->priv;

	priv->grab_pointer = enable;
	if (!enable && priv->absolute && priv->in_pointer_grab)
		do_pointer_ungrab(obj, FALSE);
	if (enable && priv->absolute && !priv->in_pointer_grab)
		do_pointer_grab(obj, FALSE);
}

void vnc_display_set_keyboard_grab(VncDisplay *obj, gboolean enable)
{
	VncDisplayPrivate *priv = obj->priv;

	priv->grab_keyboard = enable;
	if (!enable && priv->in_keyboard_grab && !priv->in_pointer_grab)
		do_keyboard_ungrab(obj, FALSE);
	if (enable && !priv->in_keyboard_grab)
		do_keyboard_grab(obj, FALSE);

}

void vnc_display_set_read_only(VncDisplay *obj, gboolean enable)
{
	obj->priv->read_only = enable;
}

GType vnc_display_credential_get_type(void)
{
	static GType etype = 0;

	if (etype == 0) {
		static const GEnumValue values[] = {
			{ VNC_DISPLAY_CREDENTIAL_PASSWORD, "VNC_DISPLAY_CREDENTIAL_PASSWORD", "password" },
			{ VNC_DISPLAY_CREDENTIAL_USERNAME, "VNC_DISPLAY_CREDENTIAL_USERNAME", "username" },
			{ VNC_DISPLAY_CREDENTIAL_CLIENTNAME, "VNC_DISPLAY_CREDENTIAL_CLIENTNAME", "clientname" },
			{ 0, NULL, NULL }
		};
		etype = g_enum_register_static ("VncDisplayCredentialType", values );
	}

	return etype;
}

GType vnc_display_key_event_get_type(void)
{
	static GType etype = 0;

	if (etype == 0) {
		static const GEnumValue values[] = {
			{ VNC_DISPLAY_KEY_EVENT_PRESS, "VNC_DISPLAY_KEY_EVENT_PRESS", "press" },
			{ VNC_DISPLAY_KEY_EVENT_RELEASE, "VNC_DISPLAY_KEY_EVENT_RELEASE", "release" },
			{ VNC_DISPLAY_KEY_EVENT_CLICK, "VNC_DISPLAY_KEY_EVENT_CLICK", "click" },
			{ 0, NULL, NULL }
		};
		etype = g_enum_register_static ("VncDisplayKeyEvents", values );
	}

	return etype;
}

GdkPixbuf *vnc_display_get_pixbuf(VncDisplay *obj)
{
	VncDisplayPrivate *priv = obj->priv;
	GdkPixbuf *pixbuf;

	if (!priv->gvnc ||
	    !gvnc_is_initialized(priv->gvnc))
		return NULL;

	pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8,
				priv->image->width, priv->image->height);

	if (!gdk_pixbuf_get_from_image(pixbuf,
				       priv->image,
				       gdk_colormap_get_system(),
				       0, 0, 0, 0,
				       priv->image->width,
				       priv->image->height))
		return NULL;

	return pixbuf;
}


int vnc_display_get_width(VncDisplay *obj)
{
	g_return_val_if_fail (VNC_IS_DISPLAY (obj), -1);

	return gvnc_get_width (obj->priv->gvnc);
}

int vnc_display_get_height(VncDisplay *obj)
{
	g_return_val_if_fail (VNC_IS_DISPLAY (obj), -1);

	return gvnc_get_height (obj->priv->gvnc);
}

const char * vnc_display_get_name(VncDisplay *obj)
{
	g_return_val_if_fail (VNC_IS_DISPLAY (obj), NULL);

	return gvnc_get_name (obj->priv->gvnc);
}

void vnc_display_client_cut_text(VncDisplay *obj, const gchar *text)
{
	g_return_if_fail (VNC_IS_DISPLAY (obj));

	gvnc_client_cut_text(obj->priv->gvnc, text, strlen (text));
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
