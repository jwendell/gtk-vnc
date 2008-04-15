/*
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2 or
 * later as published by the Free Software Foundation.
 *
 *  GTK VNC Widget
 */

#include "vncdisplay.h"
#include "coroutine.h"
#include "gvnc.h"
#include "utils.h"
#include "vncmarshal.h"
#include "config.h"
#include "x_keymap.h"

#include <gtk/gtk.h>
#include <string.h>
#include <stdlib.h>
#include <gdk/gdkkeysyms.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>

#if WITH_GTKGLEXT
#include <gtk/gtkgl.h>
#include <GL/gl.h>
#endif

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

#if WITH_GTKGLEXT
	int gl_enabled;
	GdkGLConfig *gl_config;
	GdkGLDrawable *gl_drawable;
	GdkGLContext *gl_context;
	uint8_t *gl_tex_data;
	int gl_texture_width;
	int gl_texture_height;
	int gl_width;
	int gl_height;
	GLuint gl_tex;
#endif

	struct gvnc_framebuffer fb;
	struct coroutine coroutine;
	struct gvnc *gvnc;

	guint open_id;

	gboolean in_pointer_grab;
	gboolean in_keyboard_grab;

	guint down_keyval[16];
	guint down_scancode[16];

	int button_mask;
	int last_x;
	int last_y;

	gboolean absolute;

	gboolean grab_pointer;
	gboolean grab_keyboard;
	gboolean local_pointer;
	gboolean read_only;
	gboolean allow_lossy;
	gboolean allow_scaling;
};

/* Delayed signal emission.
 *
 * We want signals to be delivered in the system coroutine.  This helps avoid
 * confusing applications.  This is particularly important when using
 * GThread based coroutines since GTK gets very upset if a signal handler is
 * run in a different thread from the main loop if that signal handler isn't
 * written to use explicit locking.
 */
struct signal_data
{
	VncDisplay *obj;
	struct coroutine *caller;

	int signum;
	GValueArray *cred_list;
	int width;
	int height;
	const char *msg;
	unsigned int auth_type;
	GString *str;
};

G_DEFINE_TYPE(VncDisplay, vnc_display, GTK_TYPE_DRAWING_AREA)

/* Properties */
enum
{
  PROP_0,
  PROP_POINTER_LOCAL,
  PROP_POINTER_GRAB,
  PROP_KEYBOARD_GRAB,
  PROP_READ_ONLY,
  PROP_WIDTH,
  PROP_HEIGHT,
  PROP_NAME,
  PROP_LOSSY_ENCODING,
  PROP_SCALING
};

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
gboolean debug_enabled = FALSE;

static const GOptionEntry gtk_vnc_args[] =
{
  { "gtk-vnc-debug", 0, 0, G_OPTION_ARG_NONE, &debug_enabled, "Enables debug output", 0 },
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, 0 }
};


static void
vnc_display_get_property (GObject    *object,
			  guint       prop_id,
			  GValue     *value,
			  GParamSpec *pspec)
{
  VncDisplay *vnc = VNC_DISPLAY (object);

  switch (prop_id)
    {
      case PROP_POINTER_LOCAL:
        g_value_set_boolean (value, vnc->priv->local_pointer);
	break;
      case PROP_POINTER_GRAB:
        g_value_set_boolean (value, vnc->priv->grab_pointer);
	break;
      case PROP_KEYBOARD_GRAB:
        g_value_set_boolean (value, vnc->priv->grab_keyboard);
	break;
      case PROP_READ_ONLY:
        g_value_set_boolean (value, vnc->priv->read_only);
	break;
      case PROP_WIDTH:
        g_value_set_int (value, vnc_display_get_width (vnc));
	break;
      case PROP_HEIGHT:
        g_value_set_int (value, vnc_display_get_height (vnc));
	break;
      case PROP_NAME:
        g_value_set_string (value, vnc_display_get_name (vnc));
	break;
      case PROP_LOSSY_ENCODING:
        g_value_set_boolean (value, vnc->priv->allow_lossy);
	break;
      case PROP_SCALING:
        g_value_set_boolean (value, vnc->priv->allow_scaling);
	break;
      default:
	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
	break;			
    }
}

static void
vnc_display_set_property (GObject      *object,
			  guint         prop_id,
			  const GValue *value,
			  GParamSpec   *pspec)
{
  VncDisplay *vnc = VNC_DISPLAY (object);

  switch (prop_id)
    {
      case PROP_POINTER_LOCAL:
        vnc_display_set_pointer_local (vnc, g_value_get_boolean (value));
        break;
      case PROP_POINTER_GRAB:
        vnc_display_set_pointer_grab (vnc, g_value_get_boolean (value));
        break;
      case PROP_KEYBOARD_GRAB:
        vnc_display_set_keyboard_grab (vnc, g_value_get_boolean (value));
        break;
      case PROP_READ_ONLY:
        vnc_display_set_read_only (vnc, g_value_get_boolean (value));
        break;
      case PROP_LOSSY_ENCODING:
        vnc_display_set_lossy_encoding (vnc, g_value_get_boolean (value));
        break;
      case PROP_SCALING:
        vnc_display_set_scaling (vnc, g_value_get_boolean (value));
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;			
    }
}

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

	GVNC_DEBUG("Expose %dx%d @ %d,%d\n",
		   expose->area.x,
		   expose->area.y,
		   expose->area.width,
		   expose->area.height);

	if (priv->image == NULL) {
#if WITH_GTKGLEXT
		if (priv->gl_tex_data == NULL)
#endif
		{
			GdkGC *gc = gdk_gc_new(widget->window);
			gdk_draw_rectangle(widget->window, gc, TRUE,
					   expose->area.x, expose->area.y,
					   expose->area.width,
					   expose->area.height);
			g_object_unref(gc);
			return TRUE;
		}
	}

#if WITH_GTKGLEXT
	if (priv->gl_enabled) {
		float rx, ry;
		int wx = 0, wy = 0;
		int ww = priv->gl_width, wh = priv->gl_height;
		double scale_x, scale_y;

		scale_x = (double)priv->gl_width / priv->fb.width;
		scale_y = (double)priv->gl_height / priv->fb.height;

		x = expose->area.x / scale_x;
		y = expose->area.y / scale_y;
		w = expose->area.width / scale_x;
		h = expose->area.height / scale_y;

		y -= 5;
		h += 10;
		if (y < 0)
			y = 0;

		x -= 5;
		w += 10;
		if (x < 0)
			x = 0;

		x = MIN(x, priv->fb.width);
		y = MIN(y, priv->fb.height);
		w = MIN(x + w, priv->fb.width);
		h = MIN(y + h, priv->fb.height);
		w -= x;
		h -= y;

		gdk_gl_drawable_gl_begin(priv->gl_drawable, priv->gl_context);
		glBindTexture(GL_TEXTURE_2D, priv->gl_tex);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, priv->fb.width);
		glTexSubImage2D(GL_TEXTURE_2D, 0,
				x, y, w, h,
				GL_BGRA_EXT,
				GL_UNSIGNED_BYTE,
				priv->gl_tex_data +
				y * 4 * priv->fb.width +
				x * 4);
		rx = (float)priv->fb.width  / priv->gl_texture_width;
		ry = (float)priv->fb.height / priv->gl_texture_height;
		
		glEnable(GL_TEXTURE_2D);
		glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
		glBegin(GL_QUADS);
		glTexCoord2f(0,ry);  glVertex3f(wx, wy, 0);
		glTexCoord2f(0,0);  glVertex3f(wx, wy+wh, 0);
		glTexCoord2f(rx,0);  glVertex3f(wx+ww, wy+wh, 0);
		glTexCoord2f(rx,ry);  glVertex3f(wx+ww, wy, 0);
		glEnd();		
		glDisable(GL_TEXTURE_2D);
		glFlush();
		gdk_gl_drawable_gl_end(priv->gl_drawable);
	} else
#endif
	{
		int mx = 0, my = 0;
		int ww, wh;

		gdk_drawable_get_size(widget->window, &ww, &wh);
		if (ww > priv->fb.width)
			mx = (ww - priv->fb.width) / 2;
		if (wh > priv->fb.height)
			my = (wh - priv->fb.height) / 2;

		x = MIN(expose->area.x - mx, priv->fb.width);
		y = MIN(expose->area.y - my, priv->fb.height);
		w = MIN(expose->area.x + expose->area.width - mx, priv->fb.width);
		h = MIN(expose->area.y + expose->area.height - my, priv->fb.height);
		x = MAX(0, x);
		y = MAX(0, y);
		w = MAX(0, w);
		h = MAX(0, h);

		w -= x;
		h -= y;

		drawn.x = x + mx;
		drawn.y = y + my;
		drawn.width = w;
		drawn.height = h;

		clear = gdk_region_rectangle(&expose->area);
		copy = gdk_region_rectangle(&drawn);
		gdk_region_subtract(clear, copy);

		gdk_gc_set_clip_region(priv->gc, copy);
		gdk_draw_image(widget->window, priv->gc, priv->image,
			       x, y, x + mx, y + my, w, h);

		gdk_gc_set_clip_region(priv->gc, clear);
		gdk_draw_rectangle(widget->window, priv->gc, TRUE, expose->area.x, expose->area.y,
				   expose->area.width, expose->area.height);

		gdk_region_destroy(clear);
		gdk_region_destroy(copy);
	}

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

	/* If we grabbed keyboard upon pointer grab, then ungrab it now */
	if (!priv->grab_keyboard)
		do_keyboard_ungrab(obj, quiet);

	gdk_pointer_ungrab(GDK_CURRENT_TIME);
	priv->in_pointer_grab = FALSE;

	if (priv->absolute)
		do_pointer_hide(obj);

	if (!quiet)
		g_signal_emit(obj, signals[VNC_POINTER_UNGRAB], 0);
}

void vnc_display_force_grab(VncDisplay *obj, gboolean enable)
{
	if (enable)
		do_pointer_grab(obj, FALSE);
	else
		do_pointer_ungrab(obj, FALSE);
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

	gtk_widget_grab_focus (widget);

	if (priv->grab_pointer && !priv->absolute && !priv->in_pointer_grab &&
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

#if WITH_GTKGLEXT
	if (priv->gl_enabled) {
		motion->x *= priv->fb.width;
		motion->x /= priv->gl_width;
		motion->y *= priv->fb.height;
		motion->y /= priv->gl_height;
	} else
#endif
	{
		int ww, wh;
		int mw = 0, mh = 0;

		gdk_drawable_get_size(widget->window, &ww, &wh);
		if (ww > priv->fb.width)
			mw = (ww - priv->fb.width) / 2;
		if (wh > priv->fb.height)
			mh = (wh - priv->fb.height) / 2;

		motion->x -= mw;
		motion->y -= mh;

		if (motion->x < 0 || motion->x >= priv->fb.width ||
		    motion->y < 0 || motion->y >= priv->fb.height)
			return FALSE;
	}

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
		return FALSE;

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

	keyval = x_keymap_get_keyval_from_keycode(key->hardware_keycode, keyval);

	/*
	 * More VNC suckiness with key state & modifiers in particular
	 *
	 * Because VNC has no concept of modifiers, we have to track what keys are
	 * pressed and when the widget looses focus send fake key up events for all
	 * keys current held down. This is because upon gaining focus any keys held
	 * down are no longer likely to be down. This would thus result in keys
	 * being 'stuck on' in the remote server. eg upon Alt-Tab to switch window
	 * focus you'd never see key up for the Alt or Tab keys without this :-(
	 *
	 * This is mostly a problem with modifier keys, but its best to just track
	 * all key presses regardless. There's a limit to how many keys a user can
	 * press at once due to a max of 10 fingers (normally :-), so down_key_vals
	 * is only storing upto 16 for now. Should be plenty...
	 *
	 * Arggggh.
	 */
	if (key->type == GDK_KEY_PRESS) {
		int i;
		for (i = 0 ; i < (int)(sizeof(priv->down_keyval)/sizeof(priv->down_keyval[0])) ; i++) {
			if (priv->down_scancode[i] == 0) {
				priv->down_keyval[i] = keyval;
				priv->down_scancode[i] = key->hardware_keycode;
				/* Send the actual key event we're dealing with */
				gvnc_key_event(priv->gvnc, 1, keyval, key->hardware_keycode);
				break;
			} else if (priv->down_scancode[i] == key->hardware_keycode) {
				/* Got an press when we're already pressed ! Why ... ?
				 *
				 * Well, GTK merges sequential press+release pairs of the same
				 * key so instead of press+release,press+release,press+release
				 * we only get press+press+press+press+press+release. This
				 * really annoys some VNC servers, so we have to un-merge
				 * them into a sensible stream of press+release pairs
				 */
				/* Fake an up event for the previous down event */
				gvnc_key_event(priv->gvnc, 0, keyval, key->hardware_keycode);
				/* Now send our actual ldown event */
				gvnc_key_event(priv->gvnc, 1, keyval, key->hardware_keycode);
				break;
			}
		}
	} else {
		int i;
		for (i = 0 ; i < (int)(sizeof(priv->down_keyval)/sizeof(priv->down_keyval[0])) ; i++) {
			/* We were pressed, and now we're released, so... */
			if (priv->down_scancode[i] == key->hardware_keycode) {
				priv->down_keyval[i] = 0;
				priv->down_scancode[i] = 0;
				/* ..send the key release event we're dealing with */
				gvnc_key_event(priv->gvnc, 0, keyval, key->hardware_keycode);
				break;
			}
		}
	}

	if (key->type == GDK_KEY_PRESS &&
	    ((keyval == GDK_Control_L && (key->state & GDK_MOD1_MASK)) ||
	     (keyval == GDK_Alt_L && (key->state & GDK_CONTROL_MASK)))) {
		if (priv->in_pointer_grab)
			do_pointer_ungrab(VNC_DISPLAY(widget), FALSE);
		else
			do_pointer_grab(VNC_DISPLAY(widget), FALSE);
	}

	return FALSE;
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


static gboolean focus_event(GtkWidget *widget, GdkEventFocus *focus G_GNUC_UNUSED,
                            gpointer data G_GNUC_UNUSED)
{
        VncDisplayPrivate *priv = VNC_DISPLAY(widget)->priv;
	int i;

        if (priv->gvnc == NULL || !gvnc_is_initialized(priv->gvnc))
                return TRUE;

	for (i = 0 ; i < (int)(sizeof(priv->down_keyval)/sizeof(priv->down_keyval[0])) ; i++) {
		/* We are currently pressed so... */
		if (priv->down_scancode[i] != 0) {
			/* ..send the fake key release event to match */
			gvnc_key_event(priv->gvnc, 0,
				       priv->down_keyval[i], priv->down_scancode[i]);
			priv->down_keyval[i] = 0;
			priv->down_scancode[i] = 0;
		}
	}

        return TRUE;
}

#if WITH_GTKGLEXT
static void realize_event(GtkWidget *widget, gpointer data G_GNUC_UNUSED)
{
	VncDisplayPrivate *priv = VNC_DISPLAY(widget)->priv;

	if (priv->gl_config == NULL)
		return;

	priv->gl_drawable = gtk_widget_get_gl_drawable(widget);
	priv->gl_context = gtk_widget_get_gl_context(widget);
}
#endif

static gboolean on_update(void *opaque, int x, int y, int w, int h)
{
	GtkWidget *widget = GTK_WIDGET(opaque);
	VncDisplay *obj = VNC_DISPLAY(widget);
	VncDisplayPrivate *priv = obj->priv;

#if WITH_GTKGLEXT
	if (priv->gl_enabled) {
		double scale_x, scale_y;

		scale_x = (double)priv->gl_width / priv->fb.width;
		scale_y = (double)priv->gl_height / priv->fb.height;

		x *= scale_x;
		y *= scale_y;
		w *= scale_x;
		h *= scale_y;
	} else
#endif
	{
		int ww, wh;
		int mw = 0, mh = 0;

		gdk_drawable_get_size(widget->window, &ww, &wh);
		if (ww > priv->fb.width)
			mw = (ww - priv->fb.width) / 2;
		if (wh > priv->fb.height)
			mh = (wh - priv->fb.height) / 2;

		x += mw;
		y += mh;
	}

	gtk_widget_queue_draw_area(widget, x, y, w, h);

	return TRUE;
}

static void setup_gdk_image(VncDisplay *obj, gint width, gint height)
{
	VncDisplayPrivate *priv = obj->priv;
	GdkVisual *visual;

	visual = gdk_drawable_get_visual(GTK_WIDGET(obj)->window);

	priv->image = gdk_image_new(GDK_IMAGE_FASTEST, visual, width, height);
	GVNC_DEBUG("Visual mask: %3d %3d %3d\n      shift: %3d %3d %3d\n",
		   visual->red_mask,
		   visual->green_mask,
		   visual->blue_mask,
		   visual->red_shift,
		   visual->green_shift,
		   visual->blue_shift);

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
	priv->fb.byte_order = priv->image->byte_order == GDK_LSB_FIRST ? G_LITTLE_ENDIAN : G_BIG_ENDIAN;

	gtk_widget_set_size_request(GTK_WIDGET(obj), width, height);
}

#if WITH_GTKGLEXT
static int pow_of_2(int value)
{
	int i;
	for (i = 0; value >= (1 << i); i++);
	return (1 << i);
}

static void setup_gl_image(VncDisplay *obj, gint width, gint height)
{
	VncDisplayPrivate *priv = VNC_DISPLAY(obj)->priv;
	void *dummy;

	priv->gl_texture_width = pow_of_2(width);
	priv->gl_texture_height = pow_of_2(height);

	gdk_gl_drawable_gl_begin(priv->gl_drawable, priv->gl_context);

	glGenTextures(1, &priv->gl_tex);
	glBindTexture(GL_TEXTURE_2D, priv->gl_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

	dummy = g_malloc(priv->gl_texture_width*priv->gl_texture_height*4);
	memset(dummy, 0, priv->gl_texture_width*priv->gl_texture_height*4);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
		     priv->gl_texture_width, priv->gl_texture_height, 0,
		     GL_RGB, GL_UNSIGNED_BYTE,
		     dummy);
	g_free(dummy);
	
	gdk_gl_drawable_gl_end(priv->gl_drawable);

	priv->gl_tex_data = g_malloc(width * height * 4);

	priv->fb.red_mask = 0xFF;
	priv->fb.green_mask = 0xFF;
	priv->fb.blue_mask = 0xFF;
	priv->fb.red_shift = 16;
	priv->fb.green_shift = 8;
	priv->fb.blue_shift = 0;
	priv->fb.depth = 32;
	priv->fb.bpp = 4;
	priv->fb.width = width;
	priv->fb.height = height;
	priv->fb.linesize = priv->fb.width * priv->fb.bpp;
	priv->fb.data = (uint8_t *)priv->gl_tex_data;
}
#endif

static gboolean emit_signal_auth_cred(gpointer opaque)
{
	struct signal_data *s = opaque;

	switch (s->signum) {
	case VNC_AUTH_CREDENTIAL:
		g_signal_emit(G_OBJECT(s->obj),
			      signals[VNC_AUTH_CREDENTIAL],
			      0,
			      s->cred_list);
		break;
	case VNC_DESKTOP_RESIZE:
		g_signal_emit(G_OBJECT(s->obj),
			      signals[VNC_DESKTOP_RESIZE],
			      0,
			      s->width, s->height);
		break;
	case VNC_AUTH_FAILURE:
		g_signal_emit(G_OBJECT(s->obj),
			      signals[VNC_AUTH_FAILURE],
			      0,
			      s->msg);
		break;
	case VNC_AUTH_UNSUPPORTED:
		g_signal_emit(G_OBJECT(s->obj),
			      signals[VNC_AUTH_UNSUPPORTED],
			      0,
			      s->auth_type);
		break;
	case VNC_SERVER_CUT_TEXT:
		g_signal_emit(G_OBJECT(s->obj),
			      signals[VNC_SERVER_CUT_TEXT],
			      0,
			      s->str->str);
		break;
	case VNC_BELL:
	case VNC_CONNECTED:
	case VNC_INITIALIZED:
	case VNC_DISCONNECTED:
		g_signal_emit(G_OBJECT(s->obj),
			      signals[s->signum],
			      0);
		break;
	}

	coroutine_yieldto(s->caller, NULL);
	
	return FALSE;
}

/* This function should be used to emit signals from gvnc callbacks */
static void emit_signal_delayed(VncDisplay *obj, int signum,
				struct signal_data *data)
{
	data->obj = obj;
	data->caller = coroutine_self();
	data->signum = signum;
	g_idle_add(emit_signal_auth_cred, data);
	coroutine_yield(NULL);
}

static gboolean do_resize(void *opaque, int width, int height, gboolean quiet)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);
	VncDisplayPrivate *priv = obj->priv;
	struct signal_data s;

	if (priv->gvnc == NULL || !gvnc_is_initialized(priv->gvnc))
		return TRUE;

	if (priv->image) {
		g_object_unref(priv->image);
		priv->image = NULL;
	}

#if WITH_GTKGLEXT
	if (priv->gl_tex_data) {
		gdk_gl_drawable_gl_begin(priv->gl_drawable,
					 priv->gl_context);
		glDeleteTextures(1, &priv->gl_tex);
		gdk_gl_drawable_gl_end(priv->gl_drawable);
		g_free(priv->gl_tex_data);
		priv->gl_tex_data = NULL;
	}
#endif

	if (priv->gc == NULL) {
		priv->null_cursor = create_null_cursor();
		if (priv->local_pointer)
			do_pointer_show(obj);
		else if (priv->in_pointer_grab || priv->absolute)
			do_pointer_hide(obj);
		priv->gc = gdk_gc_new(GTK_WIDGET(obj)->window);
	}

#if WITH_GTKGLEXT
	if (priv->gl_enabled)
		setup_gl_image(obj, width, height);
	else
#endif
		setup_gdk_image(obj, width, height);

	gvnc_set_local(priv->gvnc, &priv->fb);

	if (!quiet) {
		s.width = width;
		s.height = height;
		emit_signal_delayed(obj, VNC_DESKTOP_RESIZE, &s);
	}

	return TRUE;
}

static gboolean on_resize(void *opaque, int width, int height)
{
	return do_resize(opaque, width, height, FALSE);
}

static gboolean on_pixel_format(void *opaque, 
	struct gvnc_pixel_format *fmt G_GNUC_UNUSED)
{
        VncDisplay *obj = VNC_DISPLAY(opaque);
        VncDisplayPrivate *priv = obj->priv;

        return do_resize(opaque, priv->fb.width, priv->fb.height, TRUE);
}

#if WITH_GTKGLEXT
static void build_gl_image_from_gdk(uint32_t *data, GdkImage *image)
{
	GdkVisual *visual;
	int i, j;
	uint8_t *row;

	visual = image->visual;
	row = image->mem;
	for (j = 0; j < image->height; j++) {
		uint8_t *src = row;
		for (i = 0; i < image->width; i++) {
			uint32_t pixel = 0;
			switch (image->bpp) {
			case 1:
				pixel = *(uint8_t *)src;
				break;
			case 2:
				pixel = *(uint16_t *)src;
				break;
			case 4:
				pixel = *(uint32_t *)src;
				break;
			}
			*data = ((pixel & visual->red_mask) >> visual->red_shift) << (24 - visual->red_prec) |
				((pixel & visual->green_mask) >> visual->green_shift) << (16 - visual->green_prec) |
				((pixel & visual->blue_mask) >> visual->blue_shift) << (8 - visual->blue_prec);
			src += image->bpp;
			data++;
		}
		row += image->bpl;

	}
}

static void build_gdk_image_from_gl(GdkImage *image, uint32_t *data)
{
	GdkVisual *visual;
	int i, j;
	uint8_t *row;

	visual = image->visual;
	row = image->mem;
	for (j = 0; j < image->height; j++) {
		uint8_t *dst = row;
		for (i = 0; i < image->width; i++) {
			uint32_t pixel;

			pixel = (((*data >> (24 - visual->red_prec)) << visual->red_shift) & visual->red_mask) |
				(((*data >> (16 - visual->green_prec)) << visual->green_shift) & visual->green_mask) |
				(((*data >> (8 - visual->blue_prec)) << visual->blue_shift) & visual->blue_mask);

			switch (image->bpp) {
			case 1:
				*(uint8_t *)dst = pixel;
				break;
			case 2:
				*(uint16_t *)dst = pixel;
				break;
			case 4:
				*(uint32_t *)dst = pixel;
				break;
			}
			dst += image->bpp;
			data++;
		}
		row += image->bpl;
	}
}

static void scale_display(VncDisplay *obj, gint width, gint height)
{
	VncDisplayPrivate *priv = VNC_DISPLAY(obj)->priv;

	if (priv->gl_drawable == NULL)
		return;

	if (priv->gl_enabled == 0) {
		GdkImage *image;

		priv->gl_enabled = 1;

		image = priv->image;
		priv->image = NULL;
	
		do_resize(obj, priv->fb.width, priv->fb.height, TRUE);
		build_gl_image_from_gdk((uint32_t *)priv->fb.data, image);

		g_object_unref(image);
	}

	priv->gl_width = width;
	priv->gl_height = height;

	gdk_gl_drawable_gl_begin(priv->gl_drawable, priv->gl_context);
	glClearColor (0.0, 0.0, 0.0, 0.0);
	glShadeModel(GL_FLAT);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    
	glViewport(0, 0, priv->gl_width, priv->gl_height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0, priv->gl_width, 0.0, priv->gl_height, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	gdk_gl_drawable_gl_end(priv->gl_drawable);
}

static void rescale_display(VncDisplay *obj, gint width, gint height)
{
	VncDisplayPrivate *priv = obj->priv;

	if (priv->allow_scaling && 
	    (priv->fb.width != width ||
	     priv->fb.height != height))
		scale_display(obj, width, height);
	else if (priv->gl_enabled) {
		void *data;
		priv->gl_enabled = 0;

		data = priv->gl_tex_data;
		priv->gl_tex_data = NULL;

		do_resize(GTK_WIDGET(obj), priv->fb.width, priv->fb.height, TRUE);

		build_gdk_image_from_gl(priv->image, (uint32_t *)data);
		gdk_gl_drawable_gl_begin(priv->gl_drawable,
					 priv->gl_context);
		glDeleteTextures(1, &priv->gl_tex);
		gdk_gl_drawable_gl_end(priv->gl_drawable);
		g_free(data);
	}
}

static gboolean configure_event(GtkWidget *widget, GdkEventConfigure *configure,
				gpointer data G_GNUC_UNUSED)
{
	VncDisplay *obj = VNC_DISPLAY(widget);
	VncDisplayPrivate *priv = obj->priv;

	if (priv->fb.data == NULL)
		return FALSE;

	rescale_display(VNC_DISPLAY(widget),
			configure->width, configure->height);
	
	return FALSE;
}
#endif

static gboolean on_pointer_type_change(void *opaque, int absolute)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);
	VncDisplayPrivate *priv = obj->priv;

	if (absolute && priv->in_pointer_grab && !priv->grab_pointer)
		do_pointer_ungrab(obj, FALSE);

	priv->absolute = absolute;

	if (!priv->in_pointer_grab && !priv->absolute)
		do_pointer_show(obj);

	return TRUE;
}

static gboolean on_auth_cred(void *opaque)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);
	GValueArray *cred_list;
	GValue username, password, clientname;
	struct signal_data s;

	memset(&username, 0, sizeof(username));
	memset(&password, 0, sizeof(password));
	memset(&clientname, 0, sizeof(clientname));

	cred_list = g_value_array_new(0);
	if (gvnc_wants_credential_username(obj->priv->gvnc)) {
		g_value_init(&username, G_PARAM_SPEC_VALUE_TYPE(signalCredParam));
		g_value_set_enum(&username, VNC_DISPLAY_CREDENTIAL_USERNAME);
		cred_list = g_value_array_append(cred_list, &username);
	}
	if (gvnc_wants_credential_password(obj->priv->gvnc)) {
		g_value_init(&password, G_PARAM_SPEC_VALUE_TYPE(signalCredParam));
		g_value_set_enum(&password, VNC_DISPLAY_CREDENTIAL_PASSWORD);
		cred_list = g_value_array_append(cred_list, &password);
	}
	if (gvnc_wants_credential_x509(obj->priv->gvnc)) {
		g_value_init(&clientname, G_PARAM_SPEC_VALUE_TYPE(signalCredParam));
		g_value_set_enum(&clientname, VNC_DISPLAY_CREDENTIAL_CLIENTNAME);
		cred_list = g_value_array_append(cred_list, &clientname);
	}

	s.cred_list = cred_list;
	emit_signal_delayed(obj, VNC_AUTH_CREDENTIAL, &s);

	g_value_array_free(cred_list);

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
	struct signal_data s;

	s.msg = msg;
	emit_signal_delayed(obj, VNC_AUTH_FAILURE, &s);

	return TRUE;
}

static gboolean on_auth_unsupported(void *opaque, unsigned int auth_type)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);
	struct signal_data s;

	s.auth_type = auth_type;
	emit_signal_delayed(obj, VNC_AUTH_UNSUPPORTED, &s);

	return TRUE;
}

static gboolean on_server_cut_text(void *opaque, const void* text, size_t len)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);
	GString *str = g_string_new_len ((const gchar *)text, len);
	struct signal_data s;

	s.str = str;
	emit_signal_delayed(obj, VNC_SERVER_CUT_TEXT, &s);

	g_string_free (str, TRUE);
	return TRUE;
}

static gboolean on_bell(void *opaque)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);
	struct signal_data s;

	emit_signal_delayed(obj, VNC_BELL, &s);

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
	} else if (priv->absolute) {
		do_pointer_hide(obj);
	}

	return TRUE;
}

static gboolean check_pixbuf_support(const char *name)
{
	GSList *list, *i;

	list = gdk_pixbuf_get_formats();

	for (i = list; i; i = i->next) {
		GdkPixbufFormat *fmt = i->data;
		if (!strcmp(gdk_pixbuf_format_get_name(fmt), name))
			break;
	}

	g_slist_free(list);

	return !!(i);
}

static gboolean on_render_jpeg(void *opaque G_GNUC_UNUSED,
			       rgb24_render_func *render, void *render_opaque,
			       int x, int y, int w, int h,
			       uint8_t *data, int size)
{
	GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
	GdkPixbuf *p;
	uint8_t *pixels;

	if (!gdk_pixbuf_loader_write(loader, data, size, NULL))
		return FALSE;

	gdk_pixbuf_loader_close(loader, NULL);

	p = g_object_ref(gdk_pixbuf_loader_get_pixbuf(loader));
	g_object_unref(loader);

	pixels = gdk_pixbuf_get_pixels(p);

	render(render_opaque, x, y, w, h,
	       gdk_pixbuf_get_pixels(p),
	       gdk_pixbuf_get_rowstride(p));

	gdk_pixbuf_unref(p);

	return TRUE;
}

static const struct gvnc_ops vnc_display_ops = {
	.auth_cred = on_auth_cred,
	.auth_type = on_auth_type,
	.auth_subtype = on_auth_subtype,
	.auth_failure = on_auth_failure,
	.update = on_update,
	.resize = on_resize,
        .pixel_format = on_pixel_format,
	.pointer_type_change = on_pointer_type_change,
	.local_cursor = on_local_cursor,
	.auth_unsupported = on_auth_unsupported,
	.server_cut_text = on_server_cut_text,
	.bell = on_bell,
	.render_jpeg = on_render_jpeg,
};

/* we use an idle function to allow the coroutine to exit before we actually
 * unref the object since the coroutine's state is part of the object */
static gboolean delayed_unref_object(gpointer data)
{
	VncDisplay *obj = VNC_DISPLAY(data);

	g_assert(obj->priv->coroutine.exited == TRUE);

	if (obj->priv->image) {
		g_object_unref(obj->priv->image);
		obj->priv->image = NULL;
	}

#if WITH_GTKGLEXT
	if (obj->priv->gl_tex_data)
		g_free(obj->priv->gl_tex_data);
	obj->priv->gl_tex_data = NULL;
	obj->priv->gl_enabled = 0;
#endif

	g_object_unref(G_OBJECT(data));
	return FALSE;
}

static void *vnc_coroutine(void *opaque)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);
	VncDisplayPrivate *priv = obj->priv;

	/* this order is extremely important! */
	int32_t encodings[] = {	GVNC_ENCODING_TIGHT_JPEG5,
				GVNC_ENCODING_TIGHT,
				GVNC_ENCODING_EXT_KEY_EVENT,
				GVNC_ENCODING_DESKTOP_RESIZE,
                                GVNC_ENCODING_WMVi,
				GVNC_ENCODING_RICH_CURSOR,
				GVNC_ENCODING_XCURSOR,
				GVNC_ENCODING_POINTER_CHANGE,
				GVNC_ENCODING_ZRLE,
				GVNC_ENCODING_HEXTILE,
				GVNC_ENCODING_RRE,
				GVNC_ENCODING_COPY_RECT,
				GVNC_ENCODING_RAW };
	int32_t *encodingsp;
	int n_encodings;
	int ret;
	struct signal_data s;

	if (priv->gvnc == NULL || gvnc_is_open(priv->gvnc)) {
		g_idle_add(delayed_unref_object, obj);
		return NULL;
	}

	GVNC_DEBUG("Started background coroutine\n");
	x_keymap_set_keymap_entries();

	if (priv->fd != -1) {
		if (!gvnc_open_fd(priv->gvnc, priv->fd))
			goto cleanup;
	} else {
		if (!gvnc_open_host(priv->gvnc, priv->host, priv->port))
			goto cleanup;
	}

	emit_signal_delayed(obj, VNC_CONNECTED, &s);

	GVNC_DEBUG("Protocol initialization\n");
	if (!gvnc_initialize(priv->gvnc, FALSE))
		goto cleanup;

	emit_signal_delayed(obj, VNC_INITIALIZED, &s);

	encodingsp = encodings;
	n_encodings = G_N_ELEMENTS(encodings);

	if (check_pixbuf_support("jpeg")) {
		if (!priv->allow_lossy) {
			encodingsp++;
			n_encodings--;
		}
	} else {
		encodingsp += 2;
		n_encodings -= 2;
	}

	if (!gvnc_set_encodings(priv->gvnc, n_encodings, encodingsp))
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
	emit_signal_delayed(obj, VNC_DISCONNECTED, &s);
	g_idle_add(delayed_unref_object, obj);
	x_keymap_free_keymap_entries();
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

	obj->priv->open_id = 0;

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
	obj->priv->open_id = g_idle_add(do_vnc_display_open, obj);

	return TRUE;
}

gboolean vnc_display_open_host(VncDisplay *obj, const char *host, const char *port)
{
	if (obj->priv->gvnc == NULL || gvnc_is_open(obj->priv->gvnc))
		return FALSE;

	obj->priv->host = g_strdup(host);
	if (!obj->priv->host) {
		return FALSE;
	}
	obj->priv->port = g_strdup(port);
	if (!obj->priv->port) {
		g_free(obj->priv->host);
		obj->priv->host = NULL;
		return FALSE;
	}

	g_object_ref(G_OBJECT(obj)); /* Unref'd when co-routine exits */
	obj->priv->open_id = g_idle_add(do_vnc_display_open, obj);
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
	VncDisplayPrivate *priv = obj->priv;
	GtkWidget *widget = GTK_WIDGET(obj);

	if (priv->open_id) {
		g_source_remove(priv->open_id);
		obj->priv->open_id = 0;
	}

	if (priv->gvnc == NULL)
		return;

	if (gvnc_is_open(priv->gvnc)) {
		GVNC_DEBUG("Requesting graceful shutdown of connection\n");
		gvnc_shutdown(priv->gvnc);
	}

#if WITH_GTKGLEXT
	if (priv->gl_tex_data) {
		gdk_gl_drawable_gl_begin(priv->gl_drawable,
					 priv->gl_context);
		glDeleteTextures(1, &priv->gl_tex);
		gdk_gl_drawable_gl_end(priv->gl_drawable);
	}
#endif

	if (widget->window) {
		gint width, height;

		gdk_drawable_get_size(widget->window, &width, &height);
		gtk_widget_queue_draw_area(widget, 0, 0, width, height);
	}
}


void vnc_display_send_keys(VncDisplay *obj, const guint *keyvals, int nkeyvals)
{
	vnc_display_send_keys_ex(obj, keyvals,
				 nkeyvals, VNC_DISPLAY_KEY_EVENT_CLICK);
}

static guint get_keycode_from_keyval(guint keyval)
{
	guint keycode = 0;
	GdkKeymapKey *keys = NULL;
	gint n_keys = 0;

	if (gdk_keymap_get_entries_for_keyval(gdk_keymap_get_default(),
					      keyval, &keys, &n_keys)) {
		/* FIXME what about levels? */
		keycode = keys[0].keycode;
		g_free(keys);
	}

	return keycode;
}

void vnc_display_send_keys_ex(VncDisplay *obj, const guint *keyvals,
			      int nkeyvals, VncDisplayKeyEvent kind)
{
	int i;

	if (obj->priv->gvnc == NULL || !gvnc_is_open(obj->priv->gvnc))
		return;

	if (kind & VNC_DISPLAY_KEY_EVENT_PRESS) {
		for (i = 0 ; i < nkeyvals ; i++)
			gvnc_key_event(obj->priv->gvnc, 1, keyvals[i],
				       get_keycode_from_keyval(keyvals[i]));
	}

	if (kind & VNC_DISPLAY_KEY_EVENT_RELEASE) {
		for (i = (nkeyvals-1) ; i >= 0 ; i--)
			gvnc_key_event(obj->priv->gvnc, 0, keyvals[i],
				       get_keycode_from_keyval(keyvals[i]));
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
	VncDisplayPrivate *priv = display->priv;

	GVNC_DEBUG("Releasing VNC widget\n");
	if (gvnc_is_open(priv->gvnc)) {
		g_warning("VNC widget finalized before the connection finished shutting down\n");
	}
	gvnc_free(priv->gvnc);
	display->priv->gvnc = NULL;

#if WITH_GTKGLEXT
	if (priv->gl_enabled) {
		gdk_gl_drawable_gl_begin(priv->gl_drawable,
					 priv->gl_context);
		glDeleteTextures(1, &priv->gl_tex);
		gdk_gl_drawable_gl_end(priv->gl_drawable);
		if (priv->gl_tex_data) {
			g_free(priv->gl_tex_data);
			priv->gl_tex_data = NULL;
		}
	}

	if (priv->gl_config) {
		g_object_unref(G_OBJECT(priv->gl_config));
		priv->gl_config = NULL;
	}
#endif

	if (priv->image) {
		g_object_unref(priv->image);
		priv->image = NULL;
	}

	G_OBJECT_CLASS (vnc_display_parent_class)->finalize (obj);
}

static void vnc_display_class_init(VncDisplayClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GtkObjectClass *gtkobject_class = GTK_OBJECT_CLASS (klass);

	object_class->finalize = vnc_display_finalize;
	object_class->get_property = vnc_display_get_property;
	object_class->set_property = vnc_display_set_property;

	gtkobject_class->destroy = vnc_display_destroy;

	g_object_class_install_property (object_class,
					 PROP_POINTER_LOCAL,
					 g_param_spec_boolean ( "local-pointer",
								"Local Pointer",
								"Whether we should use the local pointer",
								FALSE,
								G_PARAM_READWRITE |
								G_PARAM_CONSTRUCT |
								G_PARAM_STATIC_NAME |
								G_PARAM_STATIC_NICK |
								G_PARAM_STATIC_BLURB));
	g_object_class_install_property (object_class,
					 PROP_POINTER_GRAB,
					 g_param_spec_boolean ( "grab-pointer",
								"Grab Pointer",
								"Whether we should grab the pointer",
								FALSE,
								G_PARAM_READWRITE |
								G_PARAM_CONSTRUCT |
								G_PARAM_STATIC_NAME |
								G_PARAM_STATIC_NICK |
								G_PARAM_STATIC_BLURB));
	g_object_class_install_property (object_class,
					 PROP_KEYBOARD_GRAB,
					 g_param_spec_boolean ( "grab-keyboard",
								"Grab Keyboard",
								"Whether we should grab the keyboard",
								FALSE,
								G_PARAM_READWRITE |
								G_PARAM_CONSTRUCT |
								G_PARAM_STATIC_NAME |
								G_PARAM_STATIC_NICK |
								G_PARAM_STATIC_BLURB));
	g_object_class_install_property (object_class,
					 PROP_READ_ONLY,
					 g_param_spec_boolean ( "read-only",
								"Read Only",
								"Whether this connection is read-only mode",
								FALSE,
								G_PARAM_READWRITE |
								G_PARAM_CONSTRUCT |
								G_PARAM_STATIC_NAME |
								G_PARAM_STATIC_NICK |
								G_PARAM_STATIC_BLURB));
	g_object_class_install_property (object_class,
					 PROP_WIDTH,
					 g_param_spec_int     ( "width",
								"Width",
								"The width of the remote screen",
								0,
								G_MAXINT,
								0,
								G_PARAM_READABLE |
								G_PARAM_STATIC_NAME |
								G_PARAM_STATIC_NICK |
								G_PARAM_STATIC_BLURB));
	g_object_class_install_property (object_class,
					 PROP_HEIGHT,
					 g_param_spec_int     ( "height",
								"Height",
								"The height of the remote screen",
								0,
								G_MAXINT,
								0,
								G_PARAM_READABLE |
								G_PARAM_STATIC_NAME |
								G_PARAM_STATIC_NICK |
								G_PARAM_STATIC_BLURB));
	g_object_class_install_property (object_class,
					 PROP_NAME,
					 g_param_spec_string  ( "name",
								"Name",
								"The screen name of the remote connection",
								NULL,
								G_PARAM_READABLE |
								G_PARAM_STATIC_NAME |
								G_PARAM_STATIC_NICK |
								G_PARAM_STATIC_BLURB));
	g_object_class_install_property (object_class,
					 PROP_LOSSY_ENCODING,
					 g_param_spec_boolean ( "lossy-encoding",
								"Lossy Encoding",
								"Whether we should use a lossy encoding",
								FALSE,
								G_PARAM_READWRITE |
								G_PARAM_CONSTRUCT |
								G_PARAM_STATIC_NAME |
								G_PARAM_STATIC_NICK |
								G_PARAM_STATIC_BLURB));
	g_object_class_install_property (object_class,
					 PROP_SCALING,
					 g_param_spec_boolean ( "scaling",
								"Scaling",
								"Whether we should use scaling",
								FALSE,
								G_PARAM_READWRITE |
								G_PARAM_CONSTRUCT |
								G_PARAM_STATIC_NAME |
								G_PARAM_STATIC_NICK |
								G_PARAM_STATIC_BLURB));

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
			      g_cclosure_marshal_VOID__BOXED,
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
	VncDisplayPrivate *priv;

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
	g_signal_connect(obj, "focus-out-event",
			 G_CALLBACK(focus_event), NULL);
#if WITH_GTKGLEXT
	g_signal_connect(obj, "realize",
			 G_CALLBACK(realize_event), NULL);
	g_signal_connect(obj, "configure-event",
			 G_CALLBACK(configure_event), NULL);
#endif

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

	priv = display->priv = VNC_DISPLAY_GET_PRIVATE(display);
	memset(priv, 0, sizeof(VncDisplayPrivate));
	priv->last_x = -1;
	priv->last_y = -1;
	priv->absolute = 1;
	priv->fd = -1;
	priv->read_only = FALSE;
	priv->allow_lossy = FALSE;
	priv->allow_scaling = FALSE;
	priv->grab_pointer = FALSE;
	priv->grab_keyboard = FALSE;
	priv->local_pointer = FALSE;

#if WITH_GTKGLEXT
	if (gtk_gl_init_check(NULL, NULL)) {
		priv->gl_config = gdk_gl_config_new_by_mode(GDK_GL_MODE_RGB |
							    GDK_GL_MODE_DEPTH);
		if (!gtk_widget_set_gl_capability(widget,
						  priv->gl_config,
						  NULL,
						  TRUE,
						  GDK_GL_RGBA_TYPE)) {
			g_warning("Could not enable OpenGL");
			g_object_unref(G_OBJECT(priv->gl_config));
			priv->gl_config = NULL;
		}
	} else
		priv->gl_config = NULL;
#endif

	priv->gvnc = gvnc_new(&vnc_display_ops, obj);
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
		else if (obj->priv->in_pointer_grab || obj->priv->absolute)
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
}

void vnc_display_set_keyboard_grab(VncDisplay *obj, gboolean enable)
{
	VncDisplayPrivate *priv = obj->priv;

	priv->grab_keyboard = enable;
	if (!enable && priv->in_keyboard_grab && !priv->in_pointer_grab)
		do_keyboard_ungrab(obj, FALSE);
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

void vnc_display_set_lossy_encoding(VncDisplay *obj, gboolean enable)
{
	g_return_if_fail (VNC_IS_DISPLAY (obj));
	obj->priv->allow_lossy = enable;
}

#if WITH_GTKGLEXT
gboolean vnc_display_set_scaling(VncDisplay *obj, gboolean enable)
{
	GtkWidget *widget = GTK_WIDGET(obj);
	gint width, height;

	g_return_val_if_fail (VNC_IS_DISPLAY (obj), FALSE);
	if (obj->priv->gl_config == NULL)
		return FALSE;
	
	obj->priv->allow_scaling = enable;
	if (gvnc_is_open(obj->priv->gvnc) && widget->window) {
		gdk_drawable_get_size(widget->window, &width, &height);
		rescale_display(obj, width, height);
		gtk_widget_queue_draw_area(widget, 0, 0, width, height);
	}

	return TRUE;
}
#else
gboolean vnc_display_set_scaling(VncDisplay *obj G_GNUC_UNUSED,
	gboolean enable G_GNUC_UNUSED)
{
	return FALSE;
}
#endif

gboolean vnc_display_get_scaling(VncDisplay *obj)
{
	g_return_val_if_fail (VNC_IS_DISPLAY (obj), FALSE);

	return obj->priv->allow_scaling;
}

gboolean vnc_display_get_lossy_encoding(VncDisplay *obj)
{
	g_return_val_if_fail (VNC_IS_DISPLAY (obj), FALSE);

	return obj->priv->allow_lossy;
}

gboolean vnc_display_get_pointer_local(VncDisplay *obj)
{
	g_return_val_if_fail (VNC_IS_DISPLAY (obj), FALSE);

	return obj->priv->local_pointer;
}

gboolean vnc_display_get_pointer_grab(VncDisplay *obj)
{
	g_return_val_if_fail (VNC_IS_DISPLAY (obj), FALSE);

	return obj->priv->grab_pointer;
}

gboolean vnc_display_get_keyboard_grab(VncDisplay *obj)
{
	g_return_val_if_fail (VNC_IS_DISPLAY (obj), FALSE);

	return obj->priv->grab_keyboard;
}

gboolean vnc_display_get_read_only(VncDisplay *obj)
{
	g_return_val_if_fail (VNC_IS_DISPLAY (obj), FALSE);

	return obj->priv->read_only;
}

gboolean vnc_display_is_pointer_absolute(VncDisplay *obj)
{
	return obj->priv->absolute;
}

GOptionGroup *
vnc_display_get_option_group (void)
{
  GOptionGroup *group;

  group = g_option_group_new ("gtk-vnc", "GTK-VNC Options", "Show GTK-VNC Options", NULL, NULL);

  g_option_group_add_entries (group, gtk_vnc_args);
  
  return group;
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
