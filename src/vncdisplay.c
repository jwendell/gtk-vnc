/*
 * GTK VNC Widget
 *
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
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

#include <config.h>
#include <locale.h>

#include "vncdisplay.h"
#include "vncconnection.h"
#include "vncutil.h"
#include "vncmarshal.h"
#include "vncdisplaykeymap.h"
#include "vncdisplayenums.h"
#include "vnccairoframebuffer.h"

#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <string.h>
#include <stdlib.h>
#include <gdk/gdkkeysyms.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#define VNC_DISPLAY_GET_PRIVATE(obj) \
      (G_TYPE_INSTANCE_GET_PRIVATE((obj), VNC_TYPE_DISPLAY, VncDisplayPrivate))

struct _VncDisplayPrivate
{
	GdkPixmap *pixmap;
	GdkCursor *null_cursor;
	GdkCursor *remote_cursor;

	VncConnection *conn;
	VncCairoFramebuffer *fb;

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
	GSList *preferable_vencrypt_subauths;
	size_t keycode_maplen;
	const guint16 const *keycode_map;

	VncGrabSequence *vncgrabseq; /* the configured key sequence */
	gboolean *vncactiveseq; /* the currently pressed keys */
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
  PROP_DEPTH,
  PROP_GRAB_KEYS,
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

static gboolean vnc_debug_option_arg(const gchar *option_name G_GNUC_UNUSED,
				     const gchar *value G_GNUC_UNUSED,
				     gpointer data G_GNUC_UNUSED,
				     GError **error G_GNUC_UNUSED)
{
	vnc_util_set_debug(TRUE);
	return TRUE;
}

static const GOptionEntry gtk_vnc_args[] =
{
	{ "gtk-vnc-debug", 0, G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK,
	  vnc_debug_option_arg, N_("Enables debug output"), 0 },
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
      case PROP_GRAB_KEYS:
	g_value_set_boxed(value, vnc->priv->vncgrabseq);
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
      case PROP_GRAB_KEYS:
	vnc_display_set_grab_keys(vnc, g_value_get_boxed(value));
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
	GdkPixbuf *pixbuf;
	GdkCursor *cursor;
	guchar data[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

	pixbuf = gdk_pixbuf_new_from_data (data,
					   GDK_COLORSPACE_RGB,
					   FALSE,
					   8,
					   2,
					   2,
					   8,
					   NULL,
					   NULL);
	cursor = gdk_cursor_new_from_pixbuf (gdk_display_get_default (),
					     pixbuf,
					     0,
					     0);
	g_object_unref (pixbuf);

	return cursor;
}
static gboolean expose_event(GtkWidget *widget, GdkEventExpose *expose)
{
	VncDisplay *obj = VNC_DISPLAY(widget);
	VncDisplayPrivate *priv = obj->priv;
	int ww, wh;
	int mx = 0, my = 0;
	cairo_t *cr;
	int fbw = 0, fbh = 0;
	GdkWindow *window;

	if (priv->fb) {
		fbw = vnc_framebuffer_get_width(VNC_FRAMEBUFFER(priv->fb));
		fbh = vnc_framebuffer_get_height(VNC_FRAMEBUFFER(priv->fb));
	}

	VNC_DEBUG("Expose area %dx%d at location %d,%d",
		   expose->area.width,
		   expose->area.height,
		   expose->area.x,
		   expose->area.y);

	window = gtk_widget_get_window(widget);
	ww = gdk_window_get_width(window);
	wh = gdk_window_get_height(window);

	if (ww > fbw)
		mx = (ww - fbw) / 2;
	if (wh > fbh)
		my = (wh - fbh) / 2;

	cr = gdk_cairo_create(window);
	cairo_rectangle(cr,
			expose->area.x-1,
			expose->area.y-1,
			expose->area.width + 2,
			expose->area.height + 2);
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
	GdkWindow *window;

	if (priv->conn == NULL || !vnc_connection_is_initialized(priv->conn))
		return FALSE;

	fbw = vnc_framebuffer_get_width(VNC_FRAMEBUFFER(priv->fb));
	fbh = vnc_framebuffer_get_height(VNC_FRAMEBUFFER(priv->fb));

	/* In relative mode, only move the server mouse pointer
	 * if the client grab is active */
	if (!priv->absolute && !priv->in_pointer_grab)
		return FALSE;

	if (priv->read_only)
		return FALSE;

	window = gtk_widget_get_window(widget);
	ww = gdk_window_get_width(window);
	wh = gdk_window_get_height(window);

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
		GdkDisplay *display = gdk_window_get_display(window);
		GdkScreen *screen = gdk_window_get_screen(window);
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


static gboolean check_for_grab_key(GtkWidget *widget, int type, int keyval)
{
	VncDisplayPrivate *priv = VNC_DISPLAY(widget)->priv;
	int i;

	if (!priv->vncgrabseq->nkeysyms)
		return FALSE;

	if (type == GDK_KEY_RELEASE) {
		/* Any key release resets the whole grab sequence */
		memset(priv->vncactiveseq, 0,
		       sizeof(gboolean)*priv->vncgrabseq->nkeysyms);

		return FALSE;
	} else {
		/* Record the new key press */
		for (i = 0 ; i < priv->vncgrabseq->nkeysyms ; i++)
			if (priv->vncgrabseq->keysyms[i] == keyval)
				priv->vncactiveseq[i] = TRUE;

		/* Return if any key is not pressed */
		for (i = 0 ; i < priv->vncgrabseq->nkeysyms ; i++)
			if (priv->vncactiveseq[i] == FALSE)
				return FALSE;

		return TRUE;
	}
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

	VNC_DEBUG("%s keycode: %d  state: %d  group %d, keyval: %d",
		   key->type == GDK_KEY_PRESS ? "press" : "release",
		   key->hardware_keycode, key->state, key->group, keyval);

	keyval = vnc_display_keyval_from_keycode(key->hardware_keycode, keyval);

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
			guint16 scancode = vnc_display_keymap_gdk2rfb(priv->keycode_map,
								      priv->keycode_maplen,
								      key->hardware_keycode);
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
			vnc_connection_key_event(priv->conn, 0, priv->down_keyval[i], scancode);
			priv->down_keyval[i] = 0;
			priv->down_scancode[i] = 0;
			break;
		}
	}

	if (key->type == GDK_KEY_PRESS) {
		for (i = 0 ; i < (int)(sizeof(priv->down_keyval)/sizeof(priv->down_keyval[0])) ; i++) {
			if (priv->down_scancode[i] == 0) {
				guint16 scancode = vnc_display_keymap_gdk2rfb(priv->keycode_map,
									      priv->keycode_maplen,
									      key->hardware_keycode);
				priv->down_keyval[i] = keyval;
				priv->down_scancode[i] = key->hardware_keycode;
				/* Send the actual key event we're dealing with */
				vnc_connection_key_event(priv->conn, 1, keyval, scancode);
				break;
			}
		}
	}

	if (check_for_grab_key(widget, key->type, key->keyval)) {
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
			guint16 scancode = vnc_display_keymap_gdk2rfb(priv->keycode_map,
								      priv->keycode_maplen,
								      priv->down_scancode[i]);
			/* ..send the fake key release event to match */
			vnc_connection_key_event(priv->conn, 0,
						 priv->down_keyval[i], scancode);
			priv->down_keyval[i] = 0;
			priv->down_scancode[i] = 0;
		}
	}

        return TRUE;
}

static void on_framebuffer_update(VncConnection *conn G_GNUC_UNUSED,
				  int x, int y, int w, int h,
				  gpointer opaque)
{
	GtkWidget *widget = GTK_WIDGET(opaque);
	VncDisplay *obj = VNC_DISPLAY(widget);
	VncDisplayPrivate *priv = obj->priv;
	int ww, wh;
	int fbw, fbh;
	cairo_surface_t *surface;
	cairo_t *cr;
	GdkWindow *window;

	fbw = vnc_framebuffer_get_width(VNC_FRAMEBUFFER(priv->fb));
	fbh = vnc_framebuffer_get_height(VNC_FRAMEBUFFER(priv->fb));

	cr = gdk_cairo_create(priv->pixmap);
	cairo_rectangle(cr, x, y, w, h);
	cairo_clip(cr);

	surface = vnc_cairo_framebuffer_get_surface(priv->fb);
	cairo_set_source_surface(cr, surface, 0, 0);
	cairo_paint(cr);

	cairo_destroy(cr);

	window = gtk_widget_get_window(widget);
	ww = gdk_window_get_width(window);
	wh = gdk_window_get_height(window);

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

	vnc_connection_framebuffer_update_request(priv->conn, 1,
						  0, 0,
						  vnc_connection_get_width(priv->conn),
						  vnc_connection_get_height(priv->conn));
}


static void do_framebuffer_init(VncDisplay *obj,
				const VncPixelFormat *remoteFormat,
				int width, int height, gboolean quiet)
{
	VncDisplayPrivate *priv = obj->priv;

	if (priv->conn == NULL || !vnc_connection_is_initialized(priv->conn))
		return;

	if (priv->fb) {
		g_object_unref(priv->fb);
		priv->fb = NULL;
	}
	if (priv->pixmap) {
		g_object_unref(priv->pixmap);
		priv->pixmap = NULL;
	}

	if (priv->null_cursor == NULL) {
		priv->null_cursor = create_null_cursor();
		if (priv->local_pointer)
			do_pointer_show(obj);
		else if (priv->in_pointer_grab || priv->absolute)
			do_pointer_hide(obj);
	}

	priv->fb = vnc_cairo_framebuffer_new(width, height, remoteFormat);
	priv->pixmap = gdk_pixmap_new(gtk_widget_get_window(GTK_WIDGET(obj)), width, height, -1);

	vnc_connection_set_framebuffer(priv->conn, VNC_FRAMEBUFFER(priv->fb));

	if (priv->force_size)
		gtk_widget_set_size_request(GTK_WIDGET(obj), width, height);

	if (!quiet) {
		g_signal_emit(G_OBJECT(obj),
			      signals[VNC_DESKTOP_RESIZE],
			      0,
			      width, height);
	}
}

static void on_desktop_resize(VncConnection *conn G_GNUC_UNUSED,
			      int width, int height,
			      gpointer opaque)
{
        VncDisplay *obj = VNC_DISPLAY(opaque);
	VncDisplayPrivate *priv = obj->priv;
	const VncPixelFormat *remoteFormat;

	remoteFormat = vnc_connection_get_pixel_format(priv->conn);

	do_framebuffer_init(opaque, remoteFormat, width, height, FALSE);

	vnc_connection_framebuffer_update_request(priv->conn, 0, 0, 0, width, height);
}

static void on_pixel_format_changed(VncConnection *conn G_GNUC_UNUSED,
				    VncPixelFormat *remoteFormat,
				    gpointer opaque)
{
        VncDisplay *obj = VNC_DISPLAY(opaque);
        VncDisplayPrivate *priv = obj->priv;
	gint16 width = vnc_connection_get_width(priv->conn);
	gint16 height = vnc_connection_get_height(priv->conn);

	do_framebuffer_init(opaque, remoteFormat, width, height, TRUE);

	vnc_connection_framebuffer_update_request(priv->conn, 0, 0, 0, width, height);
}

static gboolean vnc_display_set_preferred_pixel_format(VncDisplay *display)
{
	VncDisplayPrivate *priv = display->priv;
	GdkVisual *v =  gdk_window_get_visual(gtk_widget_get_window(GTK_WIDGET(display)));
	VncPixelFormat fmt;
	const VncPixelFormat *currentFormat;

	memset(&fmt, 0, sizeof(fmt));

	/* Get current pixel format for server */
	currentFormat = vnc_connection_get_pixel_format(priv->conn);

	switch (priv->depth) {
	case VNC_DISPLAY_DEPTH_COLOR_DEFAULT:
		VNC_DEBUG ("Using default colour depth %d (%d bpp) (true color? %d)",
			   currentFormat->depth, currentFormat->bits_per_pixel,
			   currentFormat->true_color_flag);
#if 0
		/* TigerVNC always sends back the encoding even if
		   unchanged from what the server suggested. This
		   does not appear to matter, so lets save the bytes */
		memcpy(&fmt, currentFormat, sizeof(fmt));
		break;
#else
		return TRUE;
#endif

	case VNC_DISPLAY_DEPTH_COLOR_FULL:
		fmt.depth = 24;
		fmt.bits_per_pixel = 32;
		fmt.red_max = 255;
		fmt.green_max = 255;
		fmt.blue_max = 255;
		fmt.red_shift = 16;
		fmt.green_shift = 8;
		fmt.blue_shift = 0;
		fmt.true_color_flag = 1;
		break;

	case VNC_DISPLAY_DEPTH_COLOR_MEDIUM:
		fmt.depth = 15;
		fmt.bits_per_pixel = 16;
		fmt.red_max = 31;
		fmt.green_max = 31;
		fmt.blue_max = 31;
		fmt.red_shift = 11;
		fmt.green_shift = 6;
		fmt.blue_shift = 1;
		fmt.true_color_flag = 1;
		break;

	case VNC_DISPLAY_DEPTH_COLOR_LOW:
		fmt.depth = 8;
		fmt.bits_per_pixel = 8;
		fmt.red_max = 7;
		fmt.green_max = 7;
		fmt.blue_max = 3;
		fmt.red_shift = 5;
		fmt.green_shift = 2;
		fmt.blue_shift = 0;
		fmt.true_color_flag = 1;
		break;

	case VNC_DISPLAY_DEPTH_COLOR_ULTRA_LOW:
		fmt.depth = 3;
		fmt.bits_per_pixel = 8;
		fmt.red_max = 1;
		fmt.green_max = 1;
		fmt.blue_max = 1;
		fmt.red_shift = 7;
		fmt.green_shift = 6;
		fmt.blue_shift = 5;
		fmt.true_color_flag = 1;
		break;

	default:
		g_assert_not_reached ();
	}

	#if GTK_CHECK_VERSION (2, 21, 1)
	fmt.byte_order = gdk_visual_get_byte_order (v) == GDK_LSB_FIRST ? G_BIG_ENDIAN : G_LITTLE_ENDIAN;
	#else
	fmt.byte_order = v->byte_order == GDK_LSB_FIRST ? G_BIG_ENDIAN : G_LITTLE_ENDIAN;
	#endif

	VNC_DEBUG ("Set depth color to %d (%d bpp)", fmt.depth, fmt.bits_per_pixel);
	if (!vnc_connection_set_pixel_format(priv->conn, &fmt))
		return FALSE;

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

static void on_auth_cred(VncConnection *conn G_GNUC_UNUSED,
			 GValueArray *creds,
			 gpointer opaque)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);

	g_signal_emit(G_OBJECT(obj), signals[VNC_AUTH_CREDENTIAL], 0, creds);
}

static void on_auth_choose_type(VncConnection *conn,
				GValueArray *types,
				gpointer opaque)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);
	VncDisplayPrivate *priv = obj->priv;
	GSList *l;
	guint i;

	if (!types->n_values) {
		VNC_DEBUG("No auth types available to choose from");
		vnc_connection_shutdown(conn);
		return;
	}

	for (l = priv->preferable_auths; l; l=l->next) {
		int pref = GPOINTER_TO_UINT (l->data);

		for (i=0; i< types->n_values; i++) {
			GValue *type = g_value_array_get_nth(types, i);
			if (pref == g_value_get_enum(type)) {
				vnc_connection_set_auth_type(conn, pref);
				return;
			}
		}
	}

	/* No sub-auth matching our supported auth so have to give up */
	VNC_DEBUG("No preferred auth type found");
	vnc_connection_shutdown(conn);
}

static void on_auth_choose_subtype(VncConnection *conn,
				   unsigned int type,
				   GValueArray *subtypes,
				   gpointer opaque)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);
	VncDisplayPrivate *priv = obj->priv;
	GSList *l;
	guint i;

	if (!subtypes->n_values) {
		VNC_DEBUG("No subtypes available to choose from");
		vnc_connection_shutdown(conn);
		return;
	}

	if (type == VNC_CONNECTION_AUTH_TLS) {
		l = priv->preferable_auths;
	} else if (type == VNC_CONNECTION_AUTH_VENCRYPT) {
		l = priv->preferable_vencrypt_subauths;
	} else {
		VNC_DEBUG("Unexpected stackable auth type %d", type);
		vnc_connection_shutdown(conn);
		return;
	}

	for (; l; l=l->next) {
		int pref = GPOINTER_TO_UINT (l->data);

		/* Don't want to recursively do the same major auth */
		if (pref == type)
			continue;

		for (i=0; i< subtypes->n_values; i++) {
			GValue *subtype = g_value_array_get_nth(subtypes, i);
			if (pref == g_value_get_enum(subtype)) {
				vnc_connection_set_auth_subtype(conn, pref);
				return;
			}
		}
	}

	/* No sub-auth matching our supported auth so have to give up */
	VNC_DEBUG("No preferred auth subtype found");
	vnc_connection_shutdown(conn);
}

static void on_auth_failure(VncConnection *conn G_GNUC_UNUSED,
			    const char *reason,
			    gpointer opaque)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);

	g_signal_emit(G_OBJECT(obj), signals[VNC_AUTH_FAILURE], 0, reason);
}

static void on_auth_unsupported(VncConnection *conn G_GNUC_UNUSED,
				unsigned int authType,
				gpointer opaque)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);

	g_signal_emit(G_OBJECT(obj), signals[VNC_AUTH_UNSUPPORTED], 0, authType);
}

static void on_server_cut_text(VncConnection *conn G_GNUC_UNUSED,
			       const gchar *text,
			       gpointer opaque)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);

	if (obj->priv->read_only)
		return;

	g_signal_emit(G_OBJECT(obj), signals[VNC_SERVER_CUT_TEXT], 0, text);
}

static void on_bell(VncConnection *conn G_GNUC_UNUSED,
		    gpointer opaque)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);

	g_signal_emit(G_OBJECT(obj), signals[VNC_BELL], 0);
}

static void on_cursor_changed(VncConnection *conn G_GNUC_UNUSED,
			      VncCursor *cursor,
			      gpointer opaque)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);
	VncDisplayPrivate *priv = obj->priv;

	VNC_DEBUG("Cursor changed %p x=%d y=%d w=%d h=%d",
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
		GdkDisplay *display = gdk_window_get_display(GDK_DRAWABLE(gtk_widget_get_window(GTK_WIDGET(obj))));
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

static void on_connected(VncConnection *conn G_GNUC_UNUSED,
			 gpointer opaque)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);

	g_signal_emit(G_OBJECT(obj), signals[VNC_CONNECTED], 0);
	VNC_DEBUG("Connected to VNC server");
}


static void on_initialized(VncConnection *conn G_GNUC_UNUSED,
			   gpointer opaque)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);
	VncDisplayPrivate *priv = obj->priv;
	int i;

	/* The order determines which encodings the
	 * server prefers when it has a choice to use */
	gint32 encodings[] = {  VNC_CONNECTION_ENCODING_TIGHT_JPEG5,
				VNC_CONNECTION_ENCODING_TIGHT,
				VNC_CONNECTION_ENCODING_EXT_KEY_EVENT,
				VNC_CONNECTION_ENCODING_DESKTOP_RESIZE,
                                VNC_CONNECTION_ENCODING_WMVi,
				VNC_CONNECTION_ENCODING_RICH_CURSOR,
				VNC_CONNECTION_ENCODING_XCURSOR,
				VNC_CONNECTION_ENCODING_POINTER_CHANGE,
				VNC_CONNECTION_ENCODING_ZRLE,
				VNC_CONNECTION_ENCODING_HEXTILE,
				VNC_CONNECTION_ENCODING_RRE,
				VNC_CONNECTION_ENCODING_COPY_RECT,
				VNC_CONNECTION_ENCODING_RAW };
	int n_encodings = G_N_ELEMENTS(encodings);

#define REMOVE_ENCODING(e)                                             \
	for (i = 0 ; i < n_encodings ; i++) {			       \
		if (encodings[i] == e) {			       \
			if (i < (n_encodings - 1))		       \
				memmove(encodings + i,		       \
					encodings + (i + 1),	       \
					sizeof(gint32) *	       \
					(n_encodings - (i + 1)));      \
			n_encodings--;				       \
			VNC_DEBUG("Removed encoding %d", e);	       \
			break;					       \
		}						       \
	}

	if (!vnc_display_set_preferred_pixel_format(obj))
		goto error;

	do_framebuffer_init(obj,
			    vnc_connection_get_pixel_format(priv->conn),
			    vnc_connection_get_width(priv->conn),
			    vnc_connection_get_height(priv->conn),
			    FALSE);

	if (check_pixbuf_support("jpeg")) {
		if (!priv->allow_lossy)
			REMOVE_ENCODING(VNC_CONNECTION_ENCODING_TIGHT_JPEG5);
	} else {
		REMOVE_ENCODING(VNC_CONNECTION_ENCODING_TIGHT_JPEG5);
		REMOVE_ENCODING(VNC_CONNECTION_ENCODING_TIGHT);
	}

	if (priv->keycode_map == NULL)
		REMOVE_ENCODING(VNC_CONNECTION_ENCODING_EXT_KEY_EVENT);

	VNC_DEBUG("Sending %d encodings", n_encodings);
	if (!vnc_connection_set_encodings(priv->conn, n_encodings, encodings))
		goto error;

	VNC_DEBUG("Requesting first framebuffer update");
	if (!vnc_connection_framebuffer_update_request(priv->conn, 0, 0, 0,
						       vnc_connection_get_width(priv->conn),
						       vnc_connection_get_height(priv->conn)))
		goto error;

	g_signal_emit(G_OBJECT(obj), signals[VNC_INITIALIZED], 0);

	VNC_DEBUG("Initialized VNC server");
	return;

 error:
	vnc_connection_shutdown(priv->conn);
}


static void on_disconnected(VncConnection *conn G_GNUC_UNUSED,
			    gpointer opaque)
{
	VncDisplay *obj = VNC_DISPLAY(opaque);
	VNC_DEBUG("Disconnected from VNC server");

	g_signal_emit(G_OBJECT(obj), signals[VNC_DISCONNECTED], 0);
	g_object_unref(G_OBJECT(obj));
}


gboolean vnc_display_open_fd(VncDisplay *obj, int fd)
{
	VncDisplayPrivate *priv = obj->priv;

	if (vnc_connection_is_open(priv->conn))
		return FALSE;

	if (!vnc_connection_open_fd(priv->conn, fd))
		return FALSE;

	g_object_ref(G_OBJECT(obj));

	return TRUE;
}

gboolean vnc_display_open_host(VncDisplay *obj, const char *host, const char *port)
{
	VncDisplayPrivate *priv = obj->priv;

	if (vnc_connection_is_open(priv->conn))
		return FALSE;

	if (!vnc_connection_open_host(priv->conn, host, port))
		return FALSE;

	g_object_ref(G_OBJECT(obj));

	return TRUE;
}

gboolean vnc_display_is_open(VncDisplay *obj)
{
	VncDisplayPrivate *priv = obj->priv;

	return vnc_connection_is_open(priv->conn);
}

void vnc_display_close(VncDisplay *obj)
{
	VncDisplayPrivate *priv = obj->priv;
	GtkWidget *widget = GTK_WIDGET(obj);
	GdkWindow *window;

	if (vnc_connection_is_open(priv->conn)) {
		VNC_DEBUG("Requesting graceful shutdown of connection");
		vnc_connection_shutdown(priv->conn);
	}

	window = gtk_widget_get_window(widget);
	if (window)
		gtk_widget_queue_draw_area(widget,
					   0,
					   0,
					   gdk_window_get_width(window),
					   gdk_window_get_height(window));
}


void vnc_display_send_keys(VncDisplay *obj, const guint *keyvals, int nkeyvals)
{
	vnc_display_send_keys_ex(obj, keyvals,
				 nkeyvals, VNC_DISPLAY_KEY_EVENT_CLICK);
}

static guint get_scancode_from_keyval(VncDisplay *obj, guint keyval)
{
	VncDisplayPrivate *priv = obj->priv;
	guint keycode = 0;
	GdkKeymapKey *keys = NULL;
	gint n_keys = 0;

	if (gdk_keymap_get_entries_for_keyval(gdk_keymap_get_default(),
					      keyval, &keys, &n_keys)) {
		/* FIXME what about levels? */
		keycode = keys[0].keycode;
		g_free(keys);
	}

	return vnc_display_keymap_gdk2rfb(priv->keycode_map, priv->keycode_maplen, keycode);
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
						 get_scancode_from_keyval(obj, keyvals[i]));
	}

	if (kind & VNC_DISPLAY_KEY_EVENT_RELEASE) {
		for (i = (nkeyvals-1) ; i >= 0 ; i--)
			vnc_connection_key_event(obj->priv->conn, 0, keyvals[i],
						 get_scancode_from_keyval(obj, keyvals[i]));
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

static void vnc_display_destroy (GtkWidget *obj)
{
	VncDisplay *display = VNC_DISPLAY (obj);
	VNC_DEBUG("Display destroy, requesting that VNC connection close");
	vnc_display_close(display);
	GTK_WIDGET_CLASS (vnc_display_parent_class)->destroy (obj);
}


static void vnc_display_finalize (GObject *obj)
{
	VncDisplay *display = VNC_DISPLAY (obj);
	VncDisplayPrivate *priv = display->priv;

	VNC_DEBUG("Releasing VNC widget");
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

	if (priv->vncgrabseq) {
		vnc_grab_sequence_free(priv->vncgrabseq);
		priv->vncgrabseq = NULL;
	}

	g_slist_free (priv->preferable_auths);
	g_slist_free (priv->preferable_vencrypt_subauths);

	vnc_display_keyval_free_entries();

	G_OBJECT_CLASS (vnc_display_parent_class)->finalize (obj);
}

static void vnc_display_class_init(VncDisplayClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
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
	gtkwidget_class->destroy = vnc_display_destroy;

	object_class->finalize = vnc_display_finalize;
	object_class->get_property = vnc_display_get_property;
	object_class->set_property = vnc_display_set_property;

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
	g_object_class_install_property (object_class,
					 PROP_GRAB_KEYS,
					 g_param_spec_boxed( "grab-keys",
							     "Grab keys",
							     "The key grab sequence",
							     VNC_TYPE_GRAB_SEQUENCE,
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
	GtkWidget *widget = GTK_WIDGET(display);
	VncDisplayPrivate *priv;

	gtk_widget_set_can_focus (widget, TRUE);

	vnc_display_keyval_set_entries();

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
	priv->absolute = TRUE;
	priv->read_only = FALSE;
	priv->allow_lossy = FALSE;
	priv->allow_scaling = FALSE;
	priv->grab_pointer = FALSE;
	priv->grab_keyboard = FALSE;
	priv->local_pointer = FALSE;
	priv->shared_flag = FALSE;
	priv->force_size = TRUE;
	priv->vncgrabseq = vnc_grab_sequence_new_from_string("Control_L+Alt_L");
	priv->vncactiveseq = g_new0(gboolean, priv->vncgrabseq->nkeysyms);

	/*
	 * Both these two provide TLS based auth, and can layer
	 * all the other auth types on top. So these two must
	 * be the first listed
	 */
	priv->preferable_auths = g_slist_append (priv->preferable_auths, GUINT_TO_POINTER (VNC_CONNECTION_AUTH_VENCRYPT));
	priv->preferable_auths = g_slist_append (priv->preferable_auths, GUINT_TO_POINTER (VNC_CONNECTION_AUTH_TLS));

	/*
	 * Then stackable auth types in order of preference
	 */
	priv->preferable_auths = g_slist_append (priv->preferable_auths, GUINT_TO_POINTER (VNC_CONNECTION_AUTH_SASL));
	priv->preferable_auths = g_slist_append (priv->preferable_auths, GUINT_TO_POINTER (VNC_CONNECTION_AUTH_MSLOGON));
	priv->preferable_auths = g_slist_append (priv->preferable_auths, GUINT_TO_POINTER (VNC_CONNECTION_AUTH_ARD));
	priv->preferable_auths = g_slist_append (priv->preferable_auths, GUINT_TO_POINTER (VNC_CONNECTION_AUTH_VNC));

	/*
	 * Or nothing at all
	 */
	priv->preferable_auths = g_slist_append (priv->preferable_auths, GUINT_TO_POINTER (VNC_CONNECTION_AUTH_NONE));


	/* Prefered order for VeNCrypt subtypes */
	priv->preferable_vencrypt_subauths = g_slist_append(priv->preferable_vencrypt_subauths,
							    GUINT_TO_POINTER(VNC_CONNECTION_AUTH_VENCRYPT_X509SASL));
	priv->preferable_vencrypt_subauths = g_slist_append(priv->preferable_vencrypt_subauths,
							    GUINT_TO_POINTER(VNC_CONNECTION_AUTH_VENCRYPT_X509PLAIN));
	priv->preferable_vencrypt_subauths = g_slist_append(priv->preferable_vencrypt_subauths,
							    GUINT_TO_POINTER(VNC_CONNECTION_AUTH_VENCRYPT_X509VNC));
	priv->preferable_vencrypt_subauths = g_slist_append(priv->preferable_vencrypt_subauths,
							    GUINT_TO_POINTER(VNC_CONNECTION_AUTH_VENCRYPT_X509NONE));
	priv->preferable_vencrypt_subauths = g_slist_append(priv->preferable_vencrypt_subauths,
							    GUINT_TO_POINTER(VNC_CONNECTION_AUTH_VENCRYPT_TLSSASL));
	priv->preferable_vencrypt_subauths = g_slist_append(priv->preferable_vencrypt_subauths,
							    GUINT_TO_POINTER(VNC_CONNECTION_AUTH_VENCRYPT_TLSPLAIN));
	priv->preferable_vencrypt_subauths = g_slist_append(priv->preferable_vencrypt_subauths,
							    GUINT_TO_POINTER(VNC_CONNECTION_AUTH_VENCRYPT_TLSVNC));
	priv->preferable_vencrypt_subauths = g_slist_append(priv->preferable_vencrypt_subauths,
							    GUINT_TO_POINTER(VNC_CONNECTION_AUTH_VENCRYPT_TLSNONE));
	/*
	 * Refuse fully cleartext passwords
	priv->preferable_vencrypt_subauths = g_slist_append(priv->preferable_vencrypt_subauths,
							    GUINT_TO_POINTER(VNC_CONNECTION_AUTH_VENCRYPT_PLAIN));
	*/

	priv->conn = vnc_connection_new();

	g_signal_connect(G_OBJECT(priv->conn), "vnc-cursor-changed",
			 G_CALLBACK(on_cursor_changed), display);
	g_signal_connect(G_OBJECT(priv->conn), "vnc-pointer-mode-changed",
			 G_CALLBACK(on_pointer_mode_changed), display);
	g_signal_connect(G_OBJECT(priv->conn), "vnc-bell",
			 G_CALLBACK(on_bell), display);
	g_signal_connect(G_OBJECT(priv->conn), "vnc-server-cut-text",
			 G_CALLBACK(on_server_cut_text), display);
	g_signal_connect(G_OBJECT(priv->conn), "vnc-framebuffer-update",
			 G_CALLBACK(on_framebuffer_update), display);
	g_signal_connect(G_OBJECT(priv->conn), "vnc-desktop-resize",
			 G_CALLBACK(on_desktop_resize), display);
	g_signal_connect(G_OBJECT(priv->conn), "vnc-pixel-format-changed",
			 G_CALLBACK(on_pixel_format_changed), display);
	g_signal_connect(G_OBJECT(priv->conn), "vnc-auth-failure",
			 G_CALLBACK(on_auth_failure), display);
	g_signal_connect(G_OBJECT(priv->conn), "vnc-auth-unsupported",
			 G_CALLBACK(on_auth_unsupported), display);
	g_signal_connect(G_OBJECT(priv->conn), "vnc-auth-credential",
			 G_CALLBACK(on_auth_cred), display);
	g_signal_connect(G_OBJECT(priv->conn), "vnc-auth-choose-type",
			 G_CALLBACK(on_auth_choose_type), display);
	g_signal_connect(G_OBJECT(priv->conn), "vnc-auth-choose-subtype",
			 G_CALLBACK(on_auth_choose_subtype), display);
	g_signal_connect(G_OBJECT(priv->conn), "vnc-connected",
			 G_CALLBACK(on_connected), display);
	g_signal_connect(G_OBJECT(priv->conn), "vnc-initialized",
			 G_CALLBACK(on_initialized), display);
	g_signal_connect(G_OBJECT(priv->conn), "vnc-disconnected",
			 G_CALLBACK(on_disconnected), display);

	priv->keycode_map = vnc_display_keymap_gdk2rfb_table(&priv->keycode_maplen);
}

gboolean vnc_display_set_credential(VncDisplay *obj, int type, const gchar *data)
{
	return !vnc_connection_set_credential(obj->priv->conn, type, data);
}

void vnc_display_set_pointer_local(VncDisplay *obj, gboolean enable)
{
	if (obj->priv->null_cursor) {
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

void vnc_display_set_grab_keys(VncDisplay *obj, VncGrabSequence *seq)
{
	if (obj->priv->vncgrabseq) {
		vnc_grab_sequence_free(obj->priv->vncgrabseq);
		g_free(obj->priv->vncactiveseq);
	}
	if (seq)
		obj->priv->vncgrabseq = vnc_grab_sequence_copy(seq);
	else
		obj->priv->vncgrabseq = vnc_grab_sequence_new_from_string("Control_L+Alt_L");
	obj->priv->vncactiveseq = g_new0(gboolean, obj->priv->vncgrabseq->nkeysyms);
}

VncGrabSequence *vnc_display_get_grab_keys(VncDisplay *obj)
{
	return obj->priv->vncgrabseq;
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
	cairo_surface_t *surface;
	gint w, h;

	if (!priv->conn ||
	    !vnc_connection_is_initialized(priv->conn))
		return NULL;

	surface = vnc_cairo_framebuffer_get_surface(priv->fb);
	w = vnc_framebuffer_get_width(VNC_FRAMEBUFFER(priv->fb));
	h = vnc_framebuffer_get_height(VNC_FRAMEBUFFER(priv->fb));

	pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8,
				w,
				h);

	if (!gdk_pixbuf_get_from_drawable(pixbuf,
					  priv->pixmap,
					  NULL,
					  0, 0, 0, 0,
					  w, h))
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

gboolean vnc_display_set_scaling(VncDisplay *obj,
				 gboolean enable)
{
	obj->priv->allow_scaling = enable;

	if (obj->priv->pixmap != NULL) {
		GdkWindow *window = gtk_widget_get_window(GTK_WIDGET(obj));
		gtk_widget_queue_draw_area(GTK_WIDGET(obj),
					   0,
					   0,
					   gdk_window_get_width(window),
					   gdk_window_get_height(window));
	}

	return TRUE;
}


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

	VNC_DEBUG ("Requesting a full update");
	return vnc_connection_framebuffer_update_request(obj->priv->conn,
							 0,
							 0,
							 0,
							 vnc_connection_get_width(obj->priv->conn),
							 vnc_connection_get_width(obj->priv->conn));
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
