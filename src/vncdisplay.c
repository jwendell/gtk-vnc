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
#include <locale.h>

#include "vncdisplay.h"
#include "coroutine.h"
#include "vncconnection.h"
#include "vncimageframebuffer.h"
#include "utils.h"
#include "vncmarshal.h"
#include "config.h"
#include "x_keymap.h"
#include "vncdisplayenums.h"

#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <string.h>
#include <stdlib.h>
#include <gdk/gdkkeysyms.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef HAVE_PWD_H
#include <pwd.h>
#endif

#ifdef HAVE_WINSOCK2_H
#include <winsock2.h>
#endif

static void winsock_startup (void);
static void winsock_cleanup (void);

#define VNC_DISPLAY_GET_PRIVATE(obj) \
      (G_TYPE_INSTANCE_GET_PRIVATE((obj), VNC_TYPE_DISPLAY, VncDisplayPrivate))

struct _VncDisplayPrivate
{
	int fd;
	char *host;
	char *port;
	GdkGC *gc;
	GdkPixmap *pixmap;
	GdkCursor *null_cursor;
	GdkCursor *remote_cursor;

	struct coroutine coroutine;
	VncConnection *conn;
	VncImageFramebuffer *fb;

	guint open_id;
	VncDisplayDepthColor depth;

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
	gboolean shared_flag;
	gboolean force_size;

	GSList *preferable_auths;
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
  PROP_SCALING,
  PROP_SHARED_FLAG,
  PROP_FORCE_SIZE,
  PROP_DEPTH
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
gboolean debug_enabled = FALSE;

static const GOptionEntry gtk_vnc_args[] =
{
  { "gtk-vnc-debug", 0, 0, G_OPTION_ARG_NONE, &debug_enabled, N_("Enables debug output"), 0 },
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
      case PROP_SHARED_FLAG:
        g_value_set_boolean (value, vnc->priv->shared_flag);
	break;
      case PROP_FORCE_SIZE:
        g_value_set_boolean (value, vnc->priv->force_size);
	break;
      case PROP_DEPTH:
        g_value_set_enum (value, vnc->priv->depth);
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
      case PROP_SHARED_FLAG:
        vnc_display_set_shared_flag (vnc, g_value_get_boolean (value));
        break;
      case PROP_FORCE_SIZE:
        vnc_display_set_force_size (vnc, g_value_get_boolean (value));
        break;
      case PROP_DEPTH:
        vnc_display_set_depth (vnc, g_value_get_enum (value));
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

GtkWidget *vnc_display_new(void)
{
	winsock_startup ();
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
	g_object_unref(image);

	return cursor;
}

static gboolean expose_event(GtkWidget *widget, GdkEventExpose *expose)
{
	VncDisplay *obj = VNC_DISPLAY(widget);
	VncDisplayPrivate *priv = obj->priv;
	int ww, wh;
	int mx = 0, my = 0;
#if WITH_GTK_CAIRO
	cairo_t *cr;
#else
	int x, y, w, h;
	GdkRectangle drawn;
	GdkRegion *clear, *copy;
#endif
	int fbw, fbh;

	fbw = vnc_framebuffer_get_width(VNC_FRAMEBUFFER(priv->fb));
	fbh = vnc_framebuffer_get_height(VNC_FRAMEBUFFER(priv->fb));

	GVNC_DEBUG("Expose area %dx%d at location %d,%d",
		   expose->area.width,
		   expose->area.height,
		   expose->area.x,
		   expose->area.y);

	gdk_drawable_get_size(gtk_widget_get_window(widget), &ww, &wh);

	if (ww > fbw)
		mx = (ww - fbw) / 2;
	if (wh > fbh)
		my = (wh - fbh) / 2;

#if WITH_GTK_CAIRO
	cr = gdk_cairo_create(gtk_widget_get_window(GTK_WIDGET(obj)));
	cairo_rectangle(cr,
			expose->area.x,
			expose->area.y,
			expose->area.width + 1,
			expose->area.height + 1);
	cairo_clip(cr);

	/* If we don't have a pixmap, or we're not scaling, then
	   we need to fill with background color */
	if (!priv->pixmap ||
	    !priv->allow_scaling) {
		cairo_rectangle(cr, 0, 0, ww, wh);
		/* Optionally cut out the inner area where the pixmap
		   will be drawn. This avoids 'flashing' since we're
		   not double-buffering. Note we're using the undocumented
		   behaviour of drawing the rectangle from right to left
		   to cut out the whole */
		if (priv->pixmap)
			cairo_rectangle(cr, mx + fbw, my,
					-1 * fbw, fbh);
		cairo_fill(cr);
	}

	/* Draw the VNC display */
	if (priv->pixmap) {
		if (priv->allow_scaling) {
			double sx, sy;
			/* Scale to fill window */
			sx = (double)ww / (double)fbw;
			sy = (double)wh / (double)fbh;
			cairo_scale(cr, sx, sy);
			gdk_cairo_set_source_pixmap(cr,
						    priv->pixmap,
						    0, 0);
		} else {
			gdk_cairo_set_source_pixmap(cr,
						    priv->pixmap,
						    mx, my);
		}
		cairo_paint(cr);
	}

	cairo_destroy(cr);
#else
	x = MIN(expose->area.x - mx, fbw);
	y = MIN(expose->area.y - my, fbh);
	w = MIN(expose->area.x + expose->area.width - mx, fbw);
	h = MIN(expose->area.y + expose->area.height - my, fbh);
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

	if (priv->pixmap != NULL) {
		gdk_gc_set_clip_region(priv->gc, copy);
		gdk_draw_drawable(widget->window, priv->gc, priv->pixmap,
				  x, y, x + mx, y + my, w, h);
	}

	gdk_gc_set_clip_region(priv->gc, clear);
	gdk_draw_rectangle(widget->window, priv->gc, TRUE,
			   expose->area.x, expose->area.y,
			   expose->area.width, expose->area.height);

	gdk_region_destroy(clear);
	gdk_region_destroy(copy);
#endif


	return TRUE;
}

static void do_keyboard_grab(VncDisplay *obj, gboolean quiet)
{
	VncDisplayPrivate *priv = obj->priv;

	gdk_keyboard_grab(gtk_widget_get_window(GTK_WIDGET(obj)),
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
	gdk_window_set_cursor(gtk_widget_get_window(GTK_WIDGET(obj)),
			      priv->remote_cursor ? priv->remote_cursor : priv->null_cursor);
}

static void do_pointer_show(VncDisplay *obj)
{
	VncDisplayPrivate *priv = obj->priv;
	gdk_window_set_cursor(gtk_widget_get_window(GTK_WIDGET(obj)),
			      priv->remote_cursor);
}

static void do_pointer_grab(VncDisplay *obj, gboolean quiet)
{
	VncDisplayPrivate *priv = obj->priv;

	/* If we're not already grabbing keyboard, grab it now */
	if (!priv->grab_keyboard)
		do_keyboard_grab(obj, quiet);

	/*
	 * For relative mouse to work correctly when grabbed we need to
	 * allow the pointer to move anywhere on the local desktop, so
	 * use NULL for the 'confine_to' argument. Furthermore we need
	 * the coords to be reported to our VNC window, regardless of
	 * what window the pointer is actally over, so use 'FALSE' for
	 * 'owner_events' parameter
	 */
	gdk_pointer_grab(gtk_widget_get_window(GTK_WIDGET(obj)),
			 FALSE, /* All events to come to our window directly */
			 GDK_POINTER_MOTION_MASK |
			 GDK_BUTTON_PRESS_MASK |
			 GDK_BUTTON_RELEASE_MASK |
			 GDK_BUTTON_MOTION_MASK |
			 GDK_SCROLL_MASK,
			 NULL, /* Allow cursor to move over entire desktop */
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

static gboolean button_event(GtkWidget *widget, GdkEventButton *button)
{
	VncDisplayPrivate *priv = VNC_DISPLAY(widget)->priv;
	int n;

	if (priv->conn == NULL || !vnc_connection_is_initialized(priv->conn))
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
		vnc_connection_pointer_event(priv->conn, priv->button_mask,
				   priv->last_x, priv->last_y);
	} else {
		vnc_connection_pointer_event(priv->conn, priv->button_mask,
				   0x7FFF, 0x7FFF);
	}

	return TRUE;
}

static gboolean scroll_event(GtkWidget *widget, GdkEventScroll *scroll)
{
	VncDisplayPrivate *priv = VNC_DISPLAY(widget)->priv;
	int mask;

	if (priv->conn == NULL || !vnc_connection_is_initialized(priv->conn))
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
		vnc_connection_pointer_event(priv->conn, priv->button_mask | mask,
				   priv->last_x, priv->last_y);
		vnc_connection_pointer_event(priv->conn, priv->button_mask,
				   priv->last_x, priv->last_y);
	} else {
		vnc_connection_pointer_event(priv->conn, priv->button_mask | mask,
				   0x7FFF, 0x7FFF);
		vnc_connection_pointer_event(priv->conn, priv->button_mask,
				   0x7FFF, 0x7FFF);
	}

	return TRUE;
}


/*
 * There are several scenarios to considier when handling client
 * mouse motion events:
 *
 *  - Mouse in relative mode + centered rendering of desktop
 *  - Mouse in relative mode + scaled rendering of desktop
 *  - Mouse in absolute mode + centered rendering of desktop
 *  - Mouse in absolute mode + scaled rendering of desktop
 *
 * Once scaled / offset, absolute mode is easy.
 *
 * Relative mode has a couple of special complications
 *
 *  - Need to turn client absolute events into a delta
 *  - Need to warp local pointer to avoid hitting a wall
 */
static gboolean motion_event(GtkWidget *widget, GdkEventMotion *motion)
{
	VncDisplayPrivate *priv = VNC_DISPLAY(widget)->priv;
	int ww, wh;
	int fbw, fbh;

	fbw = vnc_framebuffer_get_width(VNC_FRAMEBUFFER(priv->fb));
	fbh = vnc_framebuffer_get_height(VNC_FRAMEBUFFER(priv->fb));

	if (priv->conn == NULL || !vnc_connection_is_initialized(priv->conn))
		return FALSE;

	/* In relative mode, only move the server mouse pointer
	 * if the client grab is active */
	if (!priv->absolute && !priv->in_pointer_grab)
		return FALSE;

	if (priv->read_only)
		return FALSE;

	gdk_drawable_get_size(gtk_widget_get_window(widget), &ww, &wh);

	/* First apply adjustments to the coords in the motion event */
	if (priv->allow_scaling) {
		double sx, sy;
		sx = (double)fbw / (double)ww;
		sy = (double)fbh / (double)wh;

		/* Scaling the desktop, so scale the mouse coords
		 * by same ratio */
		motion->x *= sx;
		motion->y *= sy;
	} else {
		int mw = 0, mh = 0;

		if (ww > fbw)
			mw = (ww - fbw) / 2;
		if (wh > fbh)
			mh = (wh - fbh) / 2;

		/* Not scaling, drawing the desktop centered
		 * in the larger window, so offset the mouse
		 * coords to match centering */
		motion->x -= mw;
		motion->y -= mh;
	}

	/* Next adjust the real client pointer */
	if (!priv->absolute) {
		GdkDrawable *drawable = GDK_DRAWABLE(gtk_widget_get_window(widget));
		GdkDisplay *display = gdk_drawable_get_display(drawable);
		GdkScreen *screen = gdk_drawable_get_screen(drawable);
		int x = (int)motion->x_root;
		int y = (int)motion->y_root;

		/* In relative mode check to see if client pointer hit
		 * one of the screen edges, and if so move it back by
		 * 200 pixels. This is important because the pointer
		 * in the server doesn't correspond 1-for-1, and so
		 * may still be only half way across the screen. Without
		 * this warp, the server pointer would thus appear to hit
		 * an invisible wall */
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

	/* Finally send the event to server */
	if (priv->last_x != -1) {
		int dx, dy;
		if (priv->absolute) {
			dx = (int)motion->x;
			dy = (int)motion->y;

			/* Drop out of bounds motion to avoid upsetting
			 * the server */
			if (dx < 0 || dx >= fbw ||
			    dy < 0 || dy >= fbh)
				return FALSE;
		} else {
			/* Just send the delta since last motion event */
			dx = (int)motion->x + 0x7FFF - priv->last_x;
			dy = (int)motion->y + 0x7FFF - priv->last_y;
		}

		vnc_connection_pointer_event(priv->conn, priv->button_mask, dx, dy);
	}

	priv->last_x = (int)motion->x;
	priv->last_y = (int)motion->y;

	return TRUE;
}

static gboolean key_event(GtkWidget *widget, GdkEventKey *key)
{
	VncDisplayPrivate *priv = VNC_DISPLAY(widget)->priv;
	int i;
	int keyval = key->keyval;

	if (priv->conn == NULL || !vnc_connection_is_initialized(priv->conn))
		return FALSE;

	if (priv->read_only)
		return FALSE;

	GVNC_DEBUG("%s keycode: %d  state: %d  group %d, keyval: %d",
		   key->type == GDK_KEY_PRESS ? "press" : "release",
		   key->hardware_keycode, key->state, key->group, keyval);

	keyval = x_keymap_get_keyval_from_keycode(key->hardware_keycode, keyval);

	/*
	 * Some VNC suckiness with key state & modifiers in particular
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

	/*
	 * First the key release handling. This is *always* run, even for Key press
	 * events, because GTK will often merge sequential press+release pairs of
	 * the same key into a sequence of press+press+press+press+release. VNC
	 * servers don't like this, so we have to see if we're already pressed
	 * send release events. So, we run the release handling code all the time.
	 */
	for (i = 0 ; i < (int)(sizeof(priv->down_keyval)/sizeof(priv->down_keyval[0])) ; i++) {
		/* We were pressed, and now we're released, so... */
		if (priv->down_scancode[i] == key->hardware_keycode) {
			/*
			 * ..send the key release event we're dealing with
			 *
			 * NB, we use priv->down_keyval[i], and not our
			 * current 'keyval', because we need to make sure
			 * that the release keyval is identical to the
			 * press keyval. In some layouts, this isn't always
			 * true, with "Tab" generating Tab on press, and
			 * ISO_Prev_Group on release.
			 */
			vnc_connection_key_event(priv->conn, 0, priv->down_keyval[i], key->hardware_keycode);
			priv->down_keyval[i] = 0;
			priv->down_scancode[i] = 0;
			break;
		}
	}

	if (key->type == GDK_KEY_PRESS) {
		for (i = 0 ; i < (int)(sizeof(priv->down_keyval)/sizeof(priv->down_keyval[0])) ; i++) {
			if (priv->down_scancode[i] == 0) {
				priv->down_keyval[i] = keyval;
				priv->down_scancode[i] = key->hardware_keycode;
				/* Send the actual key event we're dealing with */
				vnc_connection_key_event(priv->conn, 1, keyval, key->hardware_keycode);
				break;
			}
		}
	}

	if (key->type == GDK_KEY_PRESS &&
	    ((keyval == GDK_Control_L && (key->state & GDK_MOD1_MASK)) ||
	     (keyval == GDK_Alt_L && (key->state & GDK_CONTROL_MASK)))) {
		if (priv->in_pointer_grab)
			do_pointer_ungrab(VNC_DISPLAY(widget), FALSE);
		else if (!priv->grab_keyboard || !priv->absolute)
			do_pointer_grab(VNC_DISPLAY(widget), FALSE);
	}

	return TRUE;
}

static gboolean enter_event(GtkWidget *widget, GdkEventCrossing *crossing G_GNUC_UNUSED)
{
        VncDisplayPrivate *priv = VNC_DISPLAY(widget)->priv;

        if (priv->conn == NULL || !vnc_connection_is_initialized(priv->conn))
                return FALSE;

        if (priv->grab_keyboard)
                do_keyboard_grab(VNC_DISPLAY(widget), FALSE);

        if (priv->local_pointer)
                do_pointer_show(VNC_DISPLAY(widget));

        return TRUE;
}

static gboolean leave_event(GtkWidget *widget, GdkEventCrossing *crossing G_GNUC_UNUSED)
{
        VncDisplayPrivate *priv = VNC_DISPLAY(widget)->priv;

        if (priv->conn == NULL || !vnc_connection_is_initialized(priv->conn))
                return FALSE;

        if (priv->grab_keyboard)
                do_keyboard_ungrab(VNC_DISPLAY(widget), FALSE);

        if (priv->grab_pointer)
                do_pointer_ungrab(VNC_DISPLAY(widget), FALSE);

        return TRUE;
}


static gboolean focus_event(GtkWidget *widget, GdkEventFocus *focus G_GNUC_UNUSED)
{
        VncDisplayPrivate *priv = VNC_DISPLAY(widget)->priv;
	int i;

        if (priv->conn == NULL || !vnc_connection_is_initialized(priv->conn))
                return FALSE;

	for (i = 0 ; i < (int)(sizeof(priv->down_keyval)/sizeof(priv->down_keyval[0])) ; i++) {
		/* We are currently pressed so... */
		if (priv->down_scancode[i] != 0) {
			/* ..send the fake key release event to match */
			vnc_connection_key_event(priv->conn, 0,
				       priv->down_keyval[i], priv->down_scancode[i]);
			priv->down_keyval[i] = 0;
			priv->down_scancode[i] = 0;
		}
	}

        return TRUE;
}

static gboolean on_update(void *opaque, int x, int y, int w, int h)
{
	GtkWidget *widget = GTK_WIDGET(opaque);
	VncDisplay *obj = VNC_DISPLAY(widget);
	VncDisplayPrivate *priv = obj->priv;
	int ww, wh;
	GdkRectangle r = { x, y, w, h };
	int fbw, fbh;

	fbw = vnc_framebuffer_get_width(VNC_FRAMEBUFFER(priv->fb));
	fbh = vnc_framebuffer_get_height(VNC_FRAMEBUFFER(priv->fb));

	/* Copy pixbuf to pixmap */
	gdk_gc_set_clip_rectangle(priv->gc, &r);
	gdk_draw_image(priv->pixmap, priv->gc,
		       vnc_image_framebuffer_get_image(priv->fb),
		       x, y, x, y, w, h);

	gdk_drawable_get_size(gtk_widget_get_window(widget), &ww, &wh);

	if (priv->allow_scaling) {
		double sx, sy;

		/* Scale the VNC region to produce expose region */

		sx = (double)ww / (double)fbw;
		sy = (double)wh / (double)fbh;
		x *= sx;
		y *= sy;
		w *= sx;
		h *= sy;
	} else {
		int mw = 0, mh = 0;

		/* Offset the VNC region to produce expose region */

		if (ww > fbw)
			mw = (ww - fbw) / 2;
		if (wh > fbh)
			mh = (wh - fbh) / 2;

		x += mw;
		y += mh;
	}

	gtk_widget_queue_draw_area(widget, x, y, w + 1, h + 1);

	return TRUE;
}


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

/* This function should be used to emit signals from VncConnection callbacks */
static void emit_signal_delayed(VncDisplay *obj, int signum,
				struct signal_data *data)
{
	data->obj = obj;
	data->caller = coroutine_self();
	data->signum = signum;

	g_idle_add(emit_signal_auth_cred, data);
	coroutine_yield(NULL);
}

static gboolean do_framebuffer_init(VncDisplay *obj,
				    const VncPixelFormat *remoteFormat,
				    int width, int height, gboolean quiet)
{
	VncDisplayPrivate *priv = obj->priv;
	struct signal_data s;
	GdkVisual *visual;
	GdkImage *image;

	if (priv->conn == NULL || !vnc_connection_is_initialized(priv->conn))
		return TRUE;

	if (priv->fb) {
		g_object_unref(priv->fb);
		priv->fb = NULL;
	}
	if (priv->pixmap) {
		g_object_unref(priv->pixmap);
		priv->pixmap = NULL;
	}

	if (priv->gc == NULL) {
		if (!priv->null_cursor)
			priv->null_cursor = create_null_cursor();
		if (priv->local_pointer)
			do_pointer_show(obj);
		else if (priv->in_pointer_grab || priv->absolute)
			do_pointer_hide(obj);
		priv->gc = gdk_gc_new(gtk_widget_get_window(GTK_WIDGET(obj)));
	}

	visual = gdk_drawable_get_visual(gtk_widget_get_window(GTK_WIDGET(obj)));
	image = gdk_image_new(GDK_IMAGE_FASTEST, visual, width, height);

	priv->fb = vnc_image_framebuffer_new(image, remoteFormat);
	priv->pixmap = gdk_pixmap_new(gtk_widget_get_window(GTK_WIDGET(obj)), width, height, -1);

	g_object_unref(G_OBJECT(image));

	vnc_connection_set_framebuffer(priv->conn, VNC_FRAMEBUFFER(priv->fb));

	if (priv->force_size)
		gtk_widget_set_size_request(GTK_WIDGET(obj), width, height);

	if (!quiet) {
		s.width = width;
		s.height = height;
		emit_signal_delayed(obj, VNC_DESKTOP_RESIZE, &s);
	}

	return TRUE;
}

static gboolean on_resize(void *opaque, int width, int height)
{
        VncDisplay *obj = VNC_DISPLAY(opaque);
	VncDisplayPrivate *priv = obj->priv;
	const VncPixelFormat *remoteFormat;

	remoteFormat = vnc_connection_get_pixel_format(priv->conn);

	return do_framebuffer_init(opaque, remoteFormat, width, height, FALSE);
}

static gboolean on_pixel_format(void *opaque,
				VncPixelFormat *remoteFormat)
{
        VncDisplay *obj = VNC_DISPLAY(opaque);
        VncDisplayPrivate *priv = obj->priv;
	gint16 width = vnc_connection_get_width(priv->conn);
	gint16 height = vnc_connection_get_height(priv->conn);

        return do_framebuffer_init(opaque, remoteFormat, width, height, TRUE);
}

static gboolean on_get_preferred_pixel_format(void *opaque,
					      VncPixelFormat *fmt)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);
	GdkVisual *v =  gdk_drawable_get_visual(gtk_widget_get_window(GTK_WIDGET(obj)));

	switch (obj->priv->depth) {
	case VNC_DISPLAY_DEPTH_COLOR_DEFAULT:
		if (fmt->true_color_flag == 1)
			break;
	case VNC_DISPLAY_DEPTH_COLOR_FULL:
		fmt->depth = 24;
		fmt->bits_per_pixel = 32;
		fmt->red_max = 255;
		fmt->green_max = 255;
		fmt->blue_max = 255;
		fmt->red_shift = 16;
		fmt->green_shift = 8;
		fmt->blue_shift = 0;
		fmt->true_color_flag = 1;
		fmt->byte_order = v->byte_order == GDK_LSB_FIRST ? G_BIG_ENDIAN : G_LITTLE_ENDIAN;
		break;
	case VNC_DISPLAY_DEPTH_COLOR_MEDIUM:
		fmt->depth = 15;
		fmt->bits_per_pixel = 16;
		fmt->red_max = 31;
		fmt->green_max = 31;
		fmt->blue_max = 31;
		fmt->red_shift = 11;
		fmt->green_shift = 6;
		fmt->blue_shift = 1;
		fmt->true_color_flag = 1;
		fmt->byte_order = v->byte_order == GDK_LSB_FIRST ? G_BIG_ENDIAN : G_LITTLE_ENDIAN;
		break;
	case VNC_DISPLAY_DEPTH_COLOR_LOW:
		fmt->depth = 8;
		fmt->bits_per_pixel = 8;
		fmt->red_max = 7;
		fmt->green_max = 7;
		fmt->blue_max = 3;
		fmt->red_shift = 5;
		fmt->green_shift = 2;
		fmt->blue_shift = 0;
		fmt->true_color_flag = 1;
		fmt->byte_order = v->byte_order == GDK_LSB_FIRST ? G_BIG_ENDIAN : G_LITTLE_ENDIAN;
		break;

	case VNC_DISPLAY_DEPTH_COLOR_ULTRA_LOW:
		fmt->depth = 3;
		fmt->bits_per_pixel = 8;
		fmt->red_max = 1;
		fmt->green_max = 1;
		fmt->blue_max = 1;
		fmt->red_shift = 7;
		fmt->green_shift = 6;
		fmt->blue_shift = 5;
		fmt->true_color_flag = 1;
		fmt->byte_order = v->byte_order == GDK_LSB_FIRST ? G_BIG_ENDIAN : G_LITTLE_ENDIAN;
		break;

	default:
		g_assert_not_reached ();
	}

	GVNC_DEBUG ("Setting depth color to %d (%d bpp)", fmt->depth, fmt->bits_per_pixel);
	return TRUE;
}

static void on_pointer_mode_changed(VncConnection *conn G_GNUC_UNUSED,
				    gboolean absPointer,
				    gpointer opaque)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);
	VncDisplayPrivate *priv = obj->priv;

	if (absPointer && priv->in_pointer_grab && priv->grab_pointer)
		do_pointer_ungrab(obj, FALSE);

	priv->absolute = absPointer;

	if (!priv->in_pointer_grab && !priv->absolute)
		do_pointer_show(obj);
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
	if (vnc_connection_wants_credential_username(obj->priv->conn)) {
		g_value_init(&username, VNC_TYPE_DISPLAY_CREDENTIAL);
		g_value_set_enum(&username, VNC_DISPLAY_CREDENTIAL_USERNAME);
		cred_list = g_value_array_append(cred_list, &username);
	}
	if (vnc_connection_wants_credential_password(obj->priv->conn)) {
		g_value_init(&password, VNC_TYPE_DISPLAY_CREDENTIAL);
		g_value_set_enum(&password, VNC_DISPLAY_CREDENTIAL_PASSWORD);
		cred_list = g_value_array_append(cred_list, &password);
	}
	if (vnc_connection_wants_credential_x509(obj->priv->conn)) {
		g_value_init(&clientname, VNC_TYPE_DISPLAY_CREDENTIAL);
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
	GSList *l;
	guint i;

	if (!ntype)
		return TRUE;

	for (l = priv->preferable_auths; l; l=l->next) {
		int pref = GPOINTER_TO_UINT (l->data);

		for (i=0; i<ntype; i++) {
			if (pref == types[i]) {
				vnc_connection_set_auth_type(priv->conn, types[i]);
				return TRUE;
			}
		}
	}

	vnc_connection_set_auth_type(priv->conn, types[0]);
	return TRUE;
}

static gboolean on_auth_subtype(void *opaque, unsigned int ntype, unsigned int *types)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);
	VncDisplayPrivate *priv = obj->priv;

	GSList *l;
	guint i;

	if (!ntype)
		return TRUE;

	for (l = priv->preferable_auths; l; l=l->next) {
		int pref = GPOINTER_TO_UINT (l->data);

		for (i=0; i<ntype; i++) {
			if (pref == types[i]) {
				vnc_connection_set_auth_subtype(priv->conn, types[i]);
				return TRUE;
			}
		}
	}

	vnc_connection_set_auth_subtype(priv->conn, types[0]);
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
	GString *str;
	struct signal_data s;

	if (obj->priv->read_only)
		return TRUE;

	str = g_string_new_len ((const gchar *)text, len);
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

static void on_cursor_changed(VncConnection *conn G_GNUC_UNUSED,
			      VncCursor *cursor,
			      gpointer opaque)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);
	VncDisplayPrivate *priv = obj->priv;

	GVNC_DEBUG("Cursor changed %p x=%d y=%d w=%d h=%d",
		   cursor,
		   cursor ? vnc_cursor_get_hotx(cursor) : -1,
		   cursor ? vnc_cursor_get_hoty(cursor) : -1,
		   cursor ? vnc_cursor_get_width(cursor) : -1,
		   cursor ? vnc_cursor_get_height(cursor) : -1);

	if (priv->remote_cursor) {
		gdk_cursor_unref(priv->remote_cursor);
		priv->remote_cursor = NULL;
	}

	if (cursor) {
		GdkDisplay *display = gdk_drawable_get_display(GDK_DRAWABLE(gtk_widget_get_window(GTK_WIDGET(obj))));
		GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(vnc_cursor_get_data(cursor),
							     GDK_COLORSPACE_RGB,
							     TRUE, 8,
							     vnc_cursor_get_width(cursor),
							     vnc_cursor_get_height(cursor),
							     vnc_cursor_get_width(cursor) * 4,
							     NULL, NULL);
		priv->remote_cursor = gdk_cursor_new_from_pixbuf(display,
								 pixbuf,
								 vnc_cursor_get_hotx(cursor),
								 vnc_cursor_get_hoty(cursor));
		g_object_unref(pixbuf);
	}

	if (priv->in_pointer_grab) {
		do_pointer_ungrab(obj, TRUE);
		do_pointer_grab(obj, TRUE);
	} else if (priv->absolute) {
		do_pointer_hide(obj);
	}
}

static gboolean check_pixbuf_support(const char *name)
{
	GSList *list, *i;

	list = gdk_pixbuf_get_formats();

	for (i = list; i; i = i->next) {
		GdkPixbufFormat *fmt = i->data;
		gchar *fmt_name = gdk_pixbuf_format_get_name(fmt);
		int cmp;

		cmp = strcmp(fmt_name, name);
		g_free (fmt_name);

		if (!cmp)
			break;
	}

	g_slist_free(list);

	return !!(i);
}

static gboolean on_render_jpeg(void *opaque G_GNUC_UNUSED,
			       rgb24_render_func *render, void *render_opaque,
			       int x, int y, int w, int h,
			       guint8 *data, int size)
{
	GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
	GdkPixbuf *p;
	guint8 *pixels;

	if (!gdk_pixbuf_loader_write(loader, data, size, NULL))
		return FALSE;

	gdk_pixbuf_loader_close(loader, NULL);

	p = g_object_ref(gdk_pixbuf_loader_get_pixbuf(loader));
	g_object_unref(loader);

	pixels = gdk_pixbuf_get_pixels(p);

	render(render_opaque, x, y, w, h,
	       gdk_pixbuf_get_pixels(p),
	       gdk_pixbuf_get_rowstride(p));

	g_object_unref(p);

	return TRUE;
}

static const struct vnc_connection_ops vnc_display_ops = {
	.auth_cred = on_auth_cred,
	.auth_type = on_auth_type,
	.auth_subtype = on_auth_subtype,
	.auth_failure = on_auth_failure,
	.update = on_update,
	.resize = on_resize,
        .pixel_format = on_pixel_format,
	.auth_unsupported = on_auth_unsupported,
	.server_cut_text = on_server_cut_text,
	.bell = on_bell,
	.render_jpeg = on_render_jpeg,
	.get_preferred_pixel_format = on_get_preferred_pixel_format
};

/* we use an idle function to allow the coroutine to exit before we actually
 * unref the object since the coroutine's state is part of the object */
static gboolean delayed_unref_object(gpointer data)
{
	VncDisplay *obj = VNC_DISPLAY(data);

	g_assert(obj->priv->coroutine.exited == TRUE);

	if (obj->priv->fb) {
		g_object_unref(obj->priv->fb);
		obj->priv->fb = NULL;
	}
	if (obj->priv->pixmap) {
		g_object_unref(obj->priv->pixmap);
		obj->priv->pixmap = NULL;
	}

	g_object_unref(G_OBJECT(data));

	winsock_cleanup ();

	return FALSE;
}

static void *vnc_coroutine(void *opaque)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);
	VncDisplayPrivate *priv = obj->priv;

	/* this order is extremely important! */
	gint32 encodings[] = {	GVNC_ENCODING_TIGHT_JPEG5,
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
	gint32 *encodingsp;
	int n_encodings;
	int ret;
	struct signal_data s;

	if (priv->conn == NULL || vnc_connection_is_open(priv->conn)) {
		g_idle_add(delayed_unref_object, obj);
		return NULL;
	}

	GVNC_DEBUG("Started background coroutine");
	x_keymap_set_keymap_entries();

	if (priv->fd != -1) {
		if (!vnc_connection_open_fd(priv->conn, priv->fd))
			goto cleanup;
	} else {
		if (!vnc_connection_open_host(priv->conn, priv->host, priv->port))
			goto cleanup;
	}

	emit_signal_delayed(obj, VNC_CONNECTED, &s);

	GVNC_DEBUG("Protocol initialization");
	if (!vnc_connection_initialize(priv->conn, priv->shared_flag))
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

	if (!vnc_connection_set_encodings(priv->conn, n_encodings, encodingsp))
			goto cleanup;

	if (!vnc_connection_framebuffer_update_request(priv->conn, 0, 0, 0,
						       vnc_connection_get_width(priv->conn),
						       vnc_connection_get_height(priv->conn)))
		goto cleanup;

	GVNC_DEBUG("Running main loop");
	while ((ret = vnc_connection_server_message(priv->conn))) {
		if (!vnc_connection_framebuffer_update_request(priv->conn, 1, 0, 0,
							       vnc_connection_get_width(priv->conn),
							       vnc_connection_get_height(priv->conn)))
			goto cleanup;
	}

 cleanup:
	GVNC_DEBUG("Doing final VNC cleanup");
	vnc_connection_close(priv->conn);
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

	if (obj->priv->conn == NULL || vnc_connection_is_open(obj->priv->conn)) {
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
	if (obj->priv->conn == NULL || vnc_connection_is_open(obj->priv->conn))
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
	if (obj->priv->conn == NULL || vnc_connection_is_open(obj->priv->conn))
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
	if (obj->priv->conn == NULL)
		return FALSE;
	return vnc_connection_is_open(obj->priv->conn);
}

void vnc_display_close(VncDisplay *obj)
{
	VncDisplayPrivate *priv = obj->priv;
	GtkWidget *widget = GTK_WIDGET(obj);

	if (priv->open_id) {
		g_source_remove(priv->open_id);
		obj->priv->open_id = 0;
	}

	if (priv->conn == NULL)
		return;

	if (vnc_connection_is_open(priv->conn)) {
		GVNC_DEBUG("Requesting graceful shutdown of connection");
		vnc_connection_shutdown(priv->conn);
	}

	if (gtk_widget_get_window(widget)) {
		gint width, height;

		gdk_drawable_get_size(gtk_widget_get_window(widget), &width, &height);
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

	if (obj->priv->conn == NULL || !vnc_connection_is_open(obj->priv->conn) || obj->priv->read_only)
		return;

	if (kind & VNC_DISPLAY_KEY_EVENT_PRESS) {
		for (i = 0 ; i < nkeyvals ; i++)
			vnc_connection_key_event(obj->priv->conn, 1, keyvals[i],
				       get_keycode_from_keyval(keyvals[i]));
	}

	if (kind & VNC_DISPLAY_KEY_EVENT_RELEASE) {
		for (i = (nkeyvals-1) ; i >= 0 ; i--)
			vnc_connection_key_event(obj->priv->conn, 0, keyvals[i],
				       get_keycode_from_keyval(keyvals[i]));
	}
}

void vnc_display_send_pointer(VncDisplay *obj, gint x, gint y, int button_mask)
{
	VncDisplayPrivate *priv = obj->priv;

	if (priv->conn == NULL || !vnc_connection_is_open(obj->priv->conn))
		return;

	if (priv->absolute) {
		priv->button_mask = button_mask;
		priv->last_x = x;
		priv->last_y = y;
		vnc_connection_pointer_event(priv->conn, priv->button_mask, x, y);
	}
}

static void vnc_display_destroy (GtkObject *obj)
{
	VncDisplay *display = VNC_DISPLAY (obj);
	GVNC_DEBUG("Requesting that VNC close");
	vnc_display_close(display);
	GTK_OBJECT_CLASS (vnc_display_parent_class)->destroy (obj);
}


static void vnc_display_finalize (GObject *obj)
{
	VncDisplay *display = VNC_DISPLAY (obj);
	VncDisplayPrivate *priv = display->priv;

	GVNC_DEBUG("Releasing VNC widget");
	if (vnc_connection_is_open(priv->conn)) {
		g_warning("VNC widget finalized before the connection finished shutting down\n");
	}
	g_object_unref(G_OBJECT(priv->conn));
	display->priv->conn = NULL;

	if (priv->fb) {
		g_object_unref(priv->fb);
		priv->fb = NULL;
	}

	if (priv->null_cursor) {
		gdk_cursor_unref (priv->null_cursor);
		priv->null_cursor = NULL;
	}

	if (priv->remote_cursor) {
		gdk_cursor_unref(priv->remote_cursor);
		priv->remote_cursor = NULL;
	}

	if (priv->gc) {
		g_object_unref (priv->gc);
		priv->gc = NULL;
	}

	g_free (priv->host);
	g_free (priv->port);

	g_slist_free (priv->preferable_auths);

	G_OBJECT_CLASS (vnc_display_parent_class)->finalize (obj);
}

static void vnc_display_class_init(VncDisplayClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GtkObjectClass *gtkobject_class = GTK_OBJECT_CLASS (klass);
	GtkWidgetClass *gtkwidget_class = GTK_WIDGET_CLASS (klass);

	gtkwidget_class->expose_event = expose_event;
	gtkwidget_class->motion_notify_event = motion_event;
	gtkwidget_class->button_press_event = button_event;
	gtkwidget_class->button_release_event = button_event;
	gtkwidget_class->scroll_event = scroll_event;
	gtkwidget_class->key_press_event = key_event;
	gtkwidget_class->key_release_event = key_event;
	gtkwidget_class->enter_notify_event = enter_event;
	gtkwidget_class->leave_notify_event = leave_event;
	gtkwidget_class->focus_out_event = focus_event;

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
	g_object_class_install_property (object_class,
					 PROP_SHARED_FLAG,
					 g_param_spec_boolean ( "shared-flag",
								"Shared Flag",
								"Whether we should leave other clients connected to the server",
								FALSE,
								G_PARAM_READWRITE |
								G_PARAM_CONSTRUCT |
								G_PARAM_STATIC_NAME |
								G_PARAM_STATIC_NICK |
								G_PARAM_STATIC_BLURB));
	g_object_class_install_property (object_class,
					 PROP_FORCE_SIZE,
					 g_param_spec_boolean ( "force-size",
								"Force widget size",
								"Whether we should define the widget size",
								TRUE,
								G_PARAM_READWRITE |
								G_PARAM_CONSTRUCT |
								G_PARAM_STATIC_NAME |
								G_PARAM_STATIC_NICK |
								G_PARAM_STATIC_BLURB));

	g_object_class_install_property (object_class,
					 PROP_DEPTH,
					 g_param_spec_enum    ( "depth",
								"Depth",
								"The color depth",
								VNC_TYPE_DISPLAY_DEPTH_COLOR,
								VNC_DISPLAY_DEPTH_COLOR_DEFAULT,
								G_PARAM_READWRITE |
								G_PARAM_CONSTRUCT |
								G_PARAM_STATIC_NAME |
								G_PARAM_STATIC_NICK |
								G_PARAM_STATIC_BLURB));

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
	priv->shared_flag = FALSE;
	priv->force_size = TRUE;

	/*
	 * Both these two provide TLS based auth, and can layer
	 * all the other auth types on top. So these two must
	 * be the first listed
	 */
	priv->preferable_auths = g_slist_append (priv->preferable_auths, GUINT_TO_POINTER (GVNC_AUTH_VENCRYPT));
	priv->preferable_auths = g_slist_append (priv->preferable_auths, GUINT_TO_POINTER (GVNC_AUTH_TLS));

	/*
	 * Then stackable auth types in order of preference
	 */
	priv->preferable_auths = g_slist_append (priv->preferable_auths, GUINT_TO_POINTER (GVNC_AUTH_SASL));
	priv->preferable_auths = g_slist_append (priv->preferable_auths, GUINT_TO_POINTER (GVNC_AUTH_MSLOGON));
	priv->preferable_auths = g_slist_append (priv->preferable_auths, GUINT_TO_POINTER (GVNC_AUTH_VNC));

	/*
	 * Or nothing at all
	 */
	priv->preferable_auths = g_slist_append (priv->preferable_auths, GUINT_TO_POINTER (GVNC_AUTH_NONE));

	priv->conn = vnc_connection_new(&vnc_display_ops, obj);

	g_signal_connect(G_OBJECT(priv->conn), "vnc-cursor-changed",
			 G_CALLBACK(on_cursor_changed), display);
	g_signal_connect(G_OBJECT(priv->conn), "vnc-pointer-mode-changed",
			 G_CALLBACK(on_pointer_mode_changed), display);
}

static char *
vnc_display_best_path(const char *basedir,
		      const char *basefile,
		      char **dirs,
		      unsigned int ndirs)
{
	unsigned int i;
	char *path;
	for (i = 0 ; i < ndirs ; i++) {
		struct stat sb;
		path = g_strdup_printf ("%s/%s/%s", dirs[i], basedir, basefile);
		if (stat(path, &sb) == 0)
			return path;
		g_free (path);
	}
	return NULL;
}

static int vnc_display_set_x509_credential(VncDisplay *obj, const char *name)
{
	gboolean ret = FALSE;
	char *file;
	char *sysdir = g_strdup_printf("%s/pki", SYSCONFDIR);
#ifndef WIN32
	struct passwd *pw;

	if (!(pw = getpwuid(getuid())))
		return TRUE;

	char *userdir = g_strdup_printf("%s/.pki", pw->pw_dir);
	char *dirs[] = { sysdir, userdir };
#else
	char *dirs[] = { sysdir };
#endif

	if ((file = vnc_display_best_path("CA", "cacert.pem", dirs,
				  sizeof(dirs)/sizeof(dirs[0]))) == NULL) {
		ret = TRUE;
		goto ret;
	}
	vnc_connection_set_credential_x509_cacert(obj->priv->conn, file);
	g_free (file);

	/* Don't mind failures of CRL */
	if ((file = vnc_display_best_path("CA", "cacrl.pem", dirs,
				  sizeof(dirs)/sizeof(dirs[0]))) != NULL)
		vnc_connection_set_credential_x509_cacert(obj->priv->conn, file);
	g_free (file);

	/* Set client key & cert if we have them. Server will reject auth
	 * if it decides it requires them*/
	if ((file = vnc_display_best_path(name, "private/clientkey.pem", dirs,
				  sizeof(dirs)/sizeof(dirs[0]))) != NULL)
		vnc_connection_set_credential_x509_key(obj->priv->conn, file);
	g_free (file);
	if ((file = vnc_display_best_path(name, "clientcert.pem", dirs,
				  sizeof(dirs)/sizeof(dirs[0]))) != NULL)
		vnc_connection_set_credential_x509_cert(obj->priv->conn, file);
	g_free (file);

     ret:
#ifndef WIN32
	g_free (userdir);
#endif
	g_free (sysdir);
	return ret;
}

gboolean vnc_display_set_credential(VncDisplay *obj, int type, const gchar *data)
{
	switch (type) {
	case VNC_DISPLAY_CREDENTIAL_PASSWORD:
		if (vnc_connection_set_credential_password(obj->priv->conn, data))
			return FALSE;
		return TRUE;

	case VNC_DISPLAY_CREDENTIAL_USERNAME:
		if (vnc_connection_set_credential_username(obj->priv->conn, data))
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

GdkPixbuf *vnc_display_get_pixbuf(VncDisplay *obj)
{
	VncDisplayPrivate *priv = obj->priv;
	GdkPixbuf *pixbuf;
	GdkImage *image;

	if (!priv->conn ||
	    !vnc_connection_is_initialized(priv->conn))
		return NULL;

	image = vnc_image_framebuffer_get_image(priv->fb);

	pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8,
				image->width, image->height);

	if (!gdk_pixbuf_get_from_image(pixbuf,
				       image,
				       gdk_colormap_get_system(),
				       0, 0, 0, 0,
				       image->width,
				       image->height))
		return NULL;

	return pixbuf;
}


int vnc_display_get_width(VncDisplay *obj)
{
	g_return_val_if_fail (VNC_IS_DISPLAY (obj), -1);

	return vnc_connection_get_width (obj->priv->conn);
}

int vnc_display_get_height(VncDisplay *obj)
{
	g_return_val_if_fail (VNC_IS_DISPLAY (obj), -1);

	return vnc_connection_get_height (obj->priv->conn);
}

const char * vnc_display_get_name(VncDisplay *obj)
{
	g_return_val_if_fail (VNC_IS_DISPLAY (obj), NULL);

	return vnc_connection_get_name (obj->priv->conn);
}

void vnc_display_client_cut_text(VncDisplay *obj, const gchar *text)
{
	g_return_if_fail (VNC_IS_DISPLAY (obj));

	if (!obj->priv->read_only)
		vnc_connection_client_cut_text(obj->priv->conn, text, strlen (text));
}

void vnc_display_set_lossy_encoding(VncDisplay *obj, gboolean enable)
{
	g_return_if_fail (VNC_IS_DISPLAY (obj));
	obj->priv->allow_lossy = enable;
}

void vnc_display_set_shared_flag(VncDisplay *obj, gboolean shared)
{
	g_return_if_fail (VNC_IS_DISPLAY (obj));
	obj->priv->shared_flag = shared;
}

#if WITH_GTK_CAIRO
gboolean vnc_display_set_scaling(VncDisplay *obj,
				 gboolean enable)
{
	int ww, wh;

	obj->priv->allow_scaling = enable;

	if (obj->priv->pixmap != NULL) {
		gdk_drawable_get_size(gtk_widget_get_window(GTK_WIDGET(obj)), &ww, &wh);
		gtk_widget_queue_draw_area(GTK_WIDGET(obj), 0, 0, ww, wh);
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


void vnc_display_set_force_size(VncDisplay *obj, gboolean enabled)
{
	g_return_if_fail (VNC_IS_DISPLAY (obj));
	obj->priv->force_size = enabled;
}

void vnc_display_set_depth(VncDisplay *obj, VncDisplayDepthColor depth)
{
	g_return_if_fail (VNC_IS_DISPLAY (obj));

	/* Ignore if we are already connected */
	if (obj->priv->conn && vnc_connection_is_initialized(obj->priv->conn))
		return;

	if (obj->priv->depth == depth)
		return;

	obj->priv->depth = depth;
}

VncDisplayDepthColor vnc_display_get_depth(VncDisplay *obj)
{
	g_return_val_if_fail (VNC_IS_DISPLAY (obj), 0);

	return obj->priv->depth;
}

gboolean vnc_display_get_force_size(VncDisplay *obj)
{
	g_return_val_if_fail (VNC_IS_DISPLAY (obj), FALSE);

	return obj->priv->force_size;
}

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

gboolean vnc_display_get_shared_flag(VncDisplay *obj)
{
	g_return_val_if_fail (VNC_IS_DISPLAY (obj), FALSE);

	return obj->priv->shared_flag;
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

	group = g_option_group_new ("gtk-vnc", N_("GTK-VNC Options:"), N_("Show GTK-VNC Options"), NULL, NULL);
	g_option_group_set_translation_domain (group, GETTEXT_PACKAGE);

	g_option_group_add_entries (group, gtk_vnc_args);

	return group;
}

const GOptionEntry *
vnc_display_get_option_entries (void)
{
	return gtk_vnc_args;
}

gboolean
vnc_display_request_update(VncDisplay *obj)
{
	g_return_val_if_fail (VNC_IS_DISPLAY (obj), FALSE);

	if (!obj->priv->conn || !vnc_connection_is_initialized(obj->priv->conn))
		return FALSE;

	GVNC_DEBUG ("Requesting a full update");
	return vnc_connection_framebuffer_update_request(obj->priv->conn,
							 0,
							 0,
							 0,
							 vnc_connection_get_width(obj->priv->conn),
							 vnc_connection_get_width(obj->priv->conn));
}

#ifdef WIN32

/* On Windows, we must call WSAStartup before using any sockets and we
 * must call WSACleanup afterwards.  And we have to balance any calls
 * to WSAStartup with a corresponding call to WSACleanup.
 *
 * Note that Wine lets you do socket calls anyway, but real Windows
 * doesn't. (http://bugs.winehq.org/show_bug.cgi?id=11965)
 */

static void
winsock_startup (void)
{
	WORD winsock_version, err;
	WSADATA winsock_data;

	/* http://msdn2.microsoft.com/en-us/library/ms742213.aspx */
	winsock_version = MAKEWORD (2, 2);
	err = WSAStartup (winsock_version, &winsock_data);
	if (err != 0)
		GVNC_DEBUG ("ignored error %d from WSAStartup", err);
}

static void
winsock_cleanup (void)
{
	WSACleanup ();
}

#else /* !WIN32 */

static void
winsock_startup (void)
{
}

static void
winsock_cleanup (void)
{
}

#endif /* !WIN32 */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
